#include <queue>
#include <vector>
#include "tandemIndex.h"
#include "valuelist.h"
#include "spinLock.h"
#include "workerThread.h"
#include "checkpoint.h"
#include "common.h"
#include "concurrentqueue/concurrentqueue.h"
#include <sys/syscall.h>
#include "insert_tracker.h"
#include <iomanip>
#include <numeric>   // std::accumulate
#include <sstream>

std::queue<CheckpointVector *> g_checkpointQueue;
bool wqReady[WORKERQUEUE_NUM] = {false};
volatile bool wtInitialized = false;
volatile bool mgInitialized = false;
std::atomic<bool> g_endTandem;
SpinLock g_spinLock;

class AsyncSampler {
public:
    // Constructor now takes shared_ptr
    AsyncSampler(std::shared_ptr<tl::InsertTracker> tracker, size_t num_threads = 1)
        : tracker_(std::move(tracker)), stop_flag_(false) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this]() { WorkerLoop(); });
        }
    }

    ~AsyncSampler() {
        Stop();
    }

    void Submit(uint64_t key) {
        queue_.enqueue(key);  // Non-blocking enqueue
    }

    void Stop() {
        stop_flag_ = true;
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

private:
    void WorkerLoop() {
        uint64_t key;
        while (!stop_flag_) {
            if (queue_.try_dequeue(key)) {
                if (tracker_) tracker_->Add(key);  // use -> for shared_ptr
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    }

    std::shared_ptr<tl::InsertTracker> tracker_;           // shared_ptr to tracker
    moodycamel::ConcurrentQueue<uint64_t> queue_;          // queue member added
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_flag_;
};

class AsyncSpeculator {
public:
    AsyncSpeculator() : stop_flag_(false) {
        worker_ = std::thread([this](){ WorkerLoop(); });
    }

    ~AsyncSpeculator() { Stop(); }

    // Called by insert threads
    bool TrySubmit() {
        bool expected = false;
        if (speculation_running_.compare_exchange_strong(
                expected, true,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            // only leader schedules job
            queue_.enqueue(1);
            return true;
        }
        return false;  // speculation already running
    }

    void SetTask(const std::function<void()>& t) { task_ = t; }

    void Stop() {
        stop_flag_.store(true, std::memory_order_release);
        queue_.enqueue(1); // wake thread
        if (worker_.joinable()) worker_.join();
    }

private:
    void WorkerLoop() {
        int _;
        while (!stop_flag_.load(std::memory_order_acquire)) {
            if (queue_.try_dequeue(_)) {
                if (task_) task_();

                // speculation finished → allow new tasks
                speculation_running_.store(false, std::memory_order_release);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }

    moodycamel::ConcurrentQueue<int> queue_;

    std::thread worker_;
    std::function<void()> task_;

public:
    std::atomic<bool> speculation_running_{false};

private:
    std::atomic<bool> stop_flag_;
};


std::shared_ptr<tl::InsertTracker> tracker_;
std::unique_ptr<AsyncSampler> sampler_;
std::unique_ptr<AsyncSpeculator> speculator_;



struct InsertForecastingOptions {
    bool use_insert_forecasting = true;
    size_t num_inserts_per_epoch = 10000; // The number of inserts in each InsertTracker epoch; the total elements of the equi-depth histogram used for insert forecasting.
    size_t num_partitions = 1000; // The number of bins in the insert forecasitng histogram. TODO: make adaptive with increased node numbers
    size_t sample_size = 1000; // The size of the reservoir sample based on which the partition boundaries are set at the beginning of each epoch.
    size_t random_seed = 42; // The random seed to be used by the insert tracker.
    double overestimation_factor = 1.5; // Estimated ratio of (number of records in reorg range) / (number of records that fit in base pages in reorg range).
    size_t num_future_epochs = 1; // During reorganization, the system will leave sufficient space to accommodate forecasted inserts for the next `num_future_epochs` epochs.
};

std::atomic<bool> speculation_running_{false};
struct SpeculationToken {
  std::atomic<bool>& flag;
  bool is_leader{false};

  explicit SpeculationToken(std::atomic<bool>& f) : flag(f) {
    bool expected = false;
    // become leader iff flag was false
    is_leader = flag.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire);
  }
  ~SpeculationToken() {
    if (is_leader) flag.store(false, std::memory_order_release);
  }

  // non-copyable
  SpeculationToken(const SpeculationToken&) = delete;
  SpeculationToken& operator=(const SpeculationToken&) = delete;
};

#define LOG_SIZE 10UL*1024UL*1024UL*1024UL

TandemIndex::TandemIndex() {
    g_endTandem.store(false,std::memory_order_relaxed);
    pmemBFPool = new PmemBFPool(MAX_VALUE_NODES);
    valueList = new ValueList();
    if(valueList->pmemVnodePool->getCurrentIdx() > 1) {
        PmemManager::memcpyToDRAM(4,
            reinterpret_cast<char *>(valueList->bf),
            reinterpret_cast<char *>(pmemBFPool->at(0)),
            sizeof(BloomFilter) * (valueList->pmemVnodePool->getCurrentIdx() + 1));
        setDataLoaded(true);
    }else{
        setDataLoaded(false);
    }
    pmemRecoveryArray = new PmemInodePool(sizeof(Inode), MAX_NODES);
    recoveryManager = new RecoveryManager(pmemRecoveryArray); 
    int level = recoveryManager->recoveryOperation();
    dramInodePool = recoveryManager->getDramInodePool();
    ckptLog = new CkptLog(LOG_SIZE);
    PmemManager::flushToNVM(3, reinterpret_cast<char *>(ckptLog), sizeof(ckptLog));
    mainIndex = new DramSkiplist(ckptLog, dramInodePool, valueList);
    mainIndex->setLevel(level);
    createLogFlushThread();
    createLogMergeThread();
    createRebalanceThread();
    Inode *index_header = mainIndex->getHeader();
    Vnode *value_header = valueList->getHeader();
    index_header->gps[0].value = value_header->getId();
    dram_log_entry_t *header_entry = new dram_log_entry_t(index_header->getId(),
        index_header->hdr.last_index,index_header->hdr.next,
        index_header->hdr.level, index_header->hdr.parent_id);
    header_entry->setKeyVal(0, index_header->gps[0].key, index_header->gps[0].value, 1);
    // 改为批量累积
    ckptLog->batcher().addFull(header_entry);
    
    // 初始化rebalanceThread数组
    for(int i = 0; i < MAX_REBALANCE_THREADS; i++) {
        rebalanceThread[i] = nullptr;
    }
    
    InsertForecastingOptions forecasting;
    if (forecasting.use_insert_forecasting) {
        tracker_ = std::make_shared<tl::InsertTracker>(
        forecasting.num_inserts_per_epoch,
        forecasting.num_partitions,
        forecasting.sample_size,
        forecasting.random_seed);
        sampler_ = std::make_unique<AsyncSampler>(tracker_, 2);
        speculator_ = std::make_unique<AsyncSpeculator>();
        speculator_->SetTask([this]() { 
            this->maybeActivateHotRegion();
        });
    } else {
        tracker_.reset(); // or leave null
        sampler_.reset();
    }
    if(is_data_loaded == false) {
        insert(0,1); 
        ckptLog->drainAndPersistOnce();
    }
    

}

TandemIndex::~TandemIndex() {
    // 先提交主线程的尾批，并阻止 TLS 析构时再次 flush
    g_endTandem.store(true, std::memory_order_relaxed);

    // 通知退出
    
    // 1) 等重平衡线程
    for(int i = 0; i < MAX_REBALANCE_THREADS; i++) {
        if(rebalanceThread[i] != nullptr) {
            if(rebalanceThread[i]->joinable()) rebalanceThread[i]->join();
            delete rebalanceThread[i];
            rebalanceThread[i] = nullptr;
        }
    }
    //cout << "after finish rebalance" << pmemRecoveryArray->at(40)->hdr.next << endl;
    ckptLog->drainAndPerssistBatchers();
    // 2) 等日志线程
    if (logFlushThread && logFlushThread->joinable()) {
        logFlushThread->join();
        delete logFlushThread;
        logFlushThread = nullptr;
    }
    //cout << "after finish log flush" << pmemRecoveryArray->at(40)->hdr.next << endl;
    if (logMergeThread && logMergeThread->joinable()) {
        logMergeThread->join();
        delete logMergeThread;
        logMergeThread = nullptr;
    }
    // 3) 所有线程都结束后再安全销毁 ckptLog
    if (ckptLog) {
        if(!ckptLog->isLogEmpty()) {
            ckptLog->forceReclaim(pmemRecoveryArray);
        }
        assert(ckptLog->isLogEmpty());
        delete ckptLog;
        ckptLog = nullptr;
    }

    Inode *superNode = pmemRecoveryArray->at(MAX_NODES - 1);
    if(superNode != nullptr) {
        superNode->hdr.next = dramInodePool->getCurrentIdx();
        superNode->hdr.level = mainIndex->getLevel();
        PmemManager::flushToNVM(0, reinterpret_cast<char *>(superNode), sizeof(Inode));
    }

    Vnode *metaVnode = valueList->pmemVnodePool->at(MAX_VALUE_NODES - 1);
    if(metaVnode != nullptr) {
        metaVnode->hdr.next = valueList->pmemVnodePool->getCurrentIdx();
        PmemManager::flushToNVM(0, reinterpret_cast<char *>(metaVnode), sizeof(Vnode));
    }

    PmemManager::memcpyToNVM(4,reinterpret_cast<char *>(pmemBFPool->at(0)),
        reinterpret_cast<char *>(valueList->bf),
        sizeof(BloomFilter) * (valueList->pmemVnodePool->getCurrentIdx() + 1));

    cout << "vnode count: " << valueList->pmemVnodePool->getCurrentIdx() << endl;
    mainIndex->printStats();

    tracker_.reset();
    if (sampler_) {
        sampler_->Stop();
        sampler_.reset();
    }
    if (speculator_) speculator_->Stop();
    
}

bool TandemIndex::insert(Key_t key, Val_t value)
{
#if 0
    tracker_->Add(key); //TODO: sampling out of the critical section
    if (tracker_->LastEpochHistogramValid()) {
        SpeculationToken tok(speculation_running_); 
        if (tok.is_leader) {
            maybeActivateHotRegion();
        }
    }
#endif
    for (;;) { // 重试环
        int idx = -1;
        Vnode *target_vnode = nullptr;
        std::vector<Inode *> updates;
        updates.reserve(MAX_LEVEL);

        int current_level = mainIndex->getLevel();
        if (current_level <= 0) return false;

        Inode *header = mainIndex->getHeader(current_level - 1);
        if (!header) return false;

        // 新：lookup 时当场捕获叶子的短快照
        InodeSnapShort snap{};
        Inode *parent_inode = mainIndex->lookupForInsertWithSnap(key, header, current_level - 1, idx, updates, snap);
        
        // 空结构（只 header）
        if (parent_inode == nullptr) {
            bool ret = insertWithNewInodes(key, value, target_vnode);
            if (!ret) {
                std::cerr << "Failed to insert with new inodes." << std::endl;
                return false;
            }
            if (target_vnode != nullptr) {
                ret = mainIndex->add(target_vnode);
                if (!ret) {
                    std::cout << "There is smaller key already inserted in the index." << std::endl;
                }
            }
            return true;
        }

        // 使用短快照快速验证（优先用 ver_snap）
        if (!mainIndex->validateSnapShort(parent_inode, snap, key)) {
            //cout << "SnapShort validation failed for key: " << key << endl;
            continue; // 并发修改导致失效，整条路径重试
        }
        //mainIndex->populate_cache_shards(key, parent_inode, current_level);

        assert(parent_inode->hdr.level == 0); // 叶子层

        int vnode_id ;
        if(snap.sgp_key != 0) {
            vnode_id = snap.sgp_value;
        } else {
            vnode_id = snap.gp_value;
        }
        const int start_vnode_id = vnode_id;
        const int current_last_idx = snap.last_index; // the last index of the target inode at the time of lookup

        Vnode *start_vnode = valueList->pmemVnodePool->at(start_vnode_id);
        if (!start_vnode) {
            std::cerr << "Failed to get vnode (id=" << start_vnode_id << ")." << std::endl;
            return false;
        }
        target_vnode = start_vnode;
        BloomFilter *target_vnode_bloom = &valueList->bf[target_vnode->getId()];

        //Vnode *start_vnode_replica = new Vnode(*start_vnode); // create a replica of the target vnode for validation

        // 快路径：尝试直接插入
        if (insertInVnodeChain(target_vnode, target_vnode_bloom, key, value)) {
            return true;
        }

        // 慢路径：节点满 -> split
        Vnode* new_vnode = nullptr; //new_vnode and target_vnode share the contents of old target_vnode before split
        BloomFilter* new_bloom = nullptr;
        if (!handleNodeFullAndSplit(target_vnode, target_vnode_bloom, key, value, new_vnode, new_bloom)) {
            std::cerr << "Failed to handle node full and split." << std::endl;
            return false;
        }

        // 分裂后更新父节点（内部会短写锁再次核对 last_index）
        int last_idx_mut = current_last_idx;
        if (!updateParentInodeAfterSplit(parent_inode, new_vnode, updates, last_idx_mut, idx)) {
            std::cerr << "Failed to update the parent inode after split." << std::endl;
            return false;
        }
        return true;
    } // for retry
}

#if 0
bool TandemIndex::insert(Key_t key, Val_t value)
{
#if 0
    tracker_->Add(key); //TODO: sampling out of the critical section
    if (tracker_->LastEpochHistogramValid()) {
        SpeculationToken tok(speculation_running_); 
        if (tok.is_leader) {
            maybeActivateHotRegion();
        }
    }
#endif
    int idx = -1;
    bool ret = false;
    Vnode *target_vnode = nullptr;
    std::vector<Inode *> updates;
    updates.reserve(MAX_LEVEL);

    int current_level = mainIndex->getLevel();
    Inode *header = mainIndex->getHeader(current_level - 1);

    std::shared_lock<std::shared_mutex> header_lock(mainIndex->inode_locks[header->getId()]);
    Inode *target_inode = mainIndex->lookupForInsert(key, header, current_level - 1, header_lock, idx, updates);

    if(target_inode == nullptr) {
        //header_lock is still the shared lock for the header inode
        header_lock.unlock();
        ret = insertWithNewInodes(key, value, target_vnode);
        if(!ret) {
            std::cerr << "Failed to insert with new j knodes." << std::endl;
            return false;
        }
        if(target_vnode != nullptr) {
            ret = mainIndex->add(target_vnode);
            if(ret == false)
            {
                cout << "There is smaller key already inserted in the index." << endl;
            }
        } 
        return true;
    }   

    // target should not be nullptr, because the smallest key is always in the header
    assert(target_inode != nullptr);
#ifdef RB_DEBUG
    Inode *temp_inode = new Inode(*target_inode); // create a copy of the target inode
    Inode *next_temp_inode = new Inode (*dramInodePool->at(temp_inode->hdr.next));
    
    if(temp_inode->getMaxKey() > next_temp_inode->getMinKey()) {
        cout << "id: " << target_inode->getId() << " idx: " << idx << " last_index: " << target_inode->hdr.last_index << " min: "<< target_inode->getMinKey() << " max: " << target_inode->getMaxKey()<< endl;
        cout << "next id: " << next_temp_inode->getId() <<" last_index: " << next_temp_inode->hdr.last_index << " min: " <<next_temp_inode->getMinKey() << " max: " << next_temp_inode->getMaxKey()<< endl;
    }
#endif

    int current_last_idx = target_inode->hdr.last_index;
    int vnode_id = target_inode->gps[idx].value;

    header_lock.unlock();
    target_vnode = valueList->pmemVnodePool->at(vnode_id);
    if(target_vnode == nullptr) {
        std::cout << "Failed to get the vnode from the pmemVnodePool." << std::endl;
        return false;
    }
    BloomFilter *bloom = &valueList->bf[target_vnode->getId()];

    if (insertInVnodeChain(target_vnode, bloom, key, value)) {
        return true;
    }
    // 6) Slow path: vnode full → split (versioned writes inside split), then update parent
    Vnode* new_vnode = nullptr;
    if (!handleNodeFullAndSplit(target_vnode, bloom, key, value, new_vnode)) {
        std::cerr << "Failed to handle node full and split." << std::endl;
        return false;
    }

    // 7) Patch parent inode (short exclusive lock inside updateParentInodeAfterSplit)
    if (!updateParentInodeAfterSplit(target_inode, new_vnode, updates, current_last_idx, idx)) {
        std::cerr << "Failed to update the parent inode after split." << std::endl;
        return false;
    }
    return true;
}
#endif

bool TandemIndex::insertWithNewInodes(Key_t key, Val_t value, Vnode *&target_vnode)
{
    Vnode* headerVnode = valueList->getHeader();
    BloomFilter* headerbloom = &valueList->bf[headerVnode->getId()];
    //std::unique_lock<std::shared_mutex> vheader_lock(headerbloom->vnode_mtx);
    write_lock(headerbloom->version);
      // create new vnode
    Vnode* newVnode = valueList->pmemVnodePool->getNextNode();
    if (!newVnode) {
        std::cerr << "Failed to get new vnode from pool" << std::endl;
        return false;
    }
    //lock state: headerVnode: yes, newVnode: no
    valueList->append(headerVnode, newVnode);
    headerbloom->setNextId(newVnode->getId());

    // 在新vnode中插入键值对
    BloomFilter* bloom = &valueList->bf[newVnode->getId()];
    //std::unique_lock<std::shared_mutex> vnode_lock(bloom->vnode_mtx);
    write_lock(bloom->version);

    // release header vnode lock, because new vnode is ready to be used
    write_unlock(headerbloom->version);
    
    if (!newVnode->insert(key, value, bloom)) {
        std::cerr << "Failed to insert into new vnode" << std::endl;
        write_unlock(bloom->version);
        return false;
    }
    if(key < bloom->getMinKey()) {
        bloom->setMinKey(key);
    }
    write_unlock(bloom->version);
    target_vnode = newVnode;
    return true;    
}

bool TandemIndex::insertInVnodeChain(Vnode* &start_vnode, BloomFilter* &start_bloom, Key_t key, Val_t value)
{
    int32_t start_vnode_id = start_vnode->getId();
    int32_t current_vnode_id = start_vnode_id;
    BloomFilter *current_bloom = start_bloom;
    Vnode *current_vnode = start_vnode;
    for (;;) {
        // ------------ 1) lock-free forward traversal ------------
        for (;;) {
            struct Step {
                bool can_move{false};
                int  next_id{-1};
            };

            Step s = read_consistent(current_bloom->version, [&]() -> Step {
                Step r;
                int nid = current_bloom->next_id;              // 读取右节点id
                if (nid == -1) {
                    return r;
                }

                BloomFilter* next_bloom = &valueList->bf[nid]; // 新增：右节点的版本源
                if(!next_bloom) return r;
                // read next's min under next's version
                Key_t next_min = read_consistent(next_bloom->version, [&]() {
                    return next_bloom->getMinKey();
                });

                r.can_move = (key >= next_min);
                r.next_id  = nid;
                return r;
            });

            if (!s.can_move) break;

            BloomFilter* next_bloom = &valueList->bf[s.next_id];
            if (!next_bloom) break; // defensive
            __builtin_prefetch(&next_bloom->min_key, 0, 1);
            current_bloom = next_bloom;
            current_vnode_id = s.next_id; 
        }

        // ------------ 2) lock & re-validate ------------
        // A concurrent split may have made our key belong to the next vnode.
        {
            // Re-check routing decision while holding this vnode's writer lock.
            write_lock(current_bloom->version);
            int nid = current_bloom->next_id;
            if (nid != -1) {
                BloomFilter* next_bloom= &valueList->bf[nid]; // 新增：右节点的版本源
                if (next_bloom) {
                    Key_t next_min = read_consistent(next_bloom->version, [&]() {
                        return next_bloom->getMinKey();
                    });

                    if (key >= next_min) {
                        // We should move right; drop the lock and loop.
                        write_unlock(current_bloom->version);
                        current_bloom = next_bloom;
                        current_vnode_id = nid;
                        continue; // go back to (1)
                    }
                }
            }

            // ------------ 3) do the insert under versioned write ------------
            current_vnode = valueList->pmemVnodePool->at(current_vnode_id);
            bool ok = current_vnode->insert(key, value, current_bloom);
            if(!ok) {
                start_vnode = current_vnode;
                start_bloom = current_bloom;
                write_unlock(current_bloom->version);
                return ok; // vnode full, caller will do split path
            }
            if(key < current_bloom->getMinKey()) {
                current_bloom->setMinKey(key);
            }
            start_vnode = current_vnode;
            start_bloom = current_bloom;
            write_unlock(current_bloom->version);

            return ok; // if false, caller will do split path
        }
    }
}

bool TandemIndex::handleNodeFullAndSplit(Vnode* &left_vnode, BloomFilter* &left_bloom,  
                                         Key_t key, Val_t value, Vnode* &right_vnode, BloomFilter* &right_bloom)
{
    write_lock(left_bloom->version);
#if 0
    right_vnode = valueList->pmemVnodePool->getNextNode();
    if (!right_vnode) {
        write_unlock(left_bloom->version);
        return false;
    }
#endif
    valueList->split(left_vnode, right_vnode);
    if(right_vnode == nullptr) { // this is the case that all the keys in left vnode are the same. we don't split but invalid all the other keys except one key.
        bool ok = left_vnode->insert(key, value, left_bloom);
        write_unlock(left_bloom->version);
        return ok;
    }
    right_bloom = &valueList->bf[right_vnode->getId()];

    // 3) Decide which node should receive (key,value), Read nextVnode’s min under its own version to avoid torn reads.
    Key_t next_right_min = read_consistent(right_bloom->version, [&](){
        return right_bloom->getMinKey();
    });
#if 1
    if (sampler_) {sampler_->Submit(next_right_min);} //sample split
    if (tracker_->LastEpochHistogramValid()) { //TODO: currently one thread speculating per round
        speculator_->TrySubmit();   // TODO: optimize: avoid double atomic ops
    }
#endif

    bool ok = false;
    if (key < next_right_min) {
        ok = left_vnode->insert(key, value, left_bloom);

#if 0
        if(key < left_bloom->getMinKey()) {
            left_bloom->setMinKey(key);
        }
        
        Key_t left_min_key = left_vnode->getMinKey();
        assert(left_min_key== left_bloom->min_key);
        assert(key >= left_bloom->min_key);
#endif
        write_unlock(left_bloom->version);
    } else {
        // Insert into RIGHT (new) vnode
        write_lock(right_bloom->version);
        write_unlock(left_bloom->version);
        ok = right_vnode->insert(key, value, right_bloom);
#if 0
        if(key < right_bloom->getMinKey()) {
            right_bloom->setMinKey(key);
        }
        
        Key_t right_min_key = right_vnode->getMinKey();
        assert(right_min_key== right_bloom->min_key);
        assert(key >= right_bloom->min_key);
#endif
        write_unlock(right_bloom->version);
    }
    if (!ok) return false;
   
#if 0
    Key_t left_min_key = read_consistent(left_bloom->version, [&](){
        return left_vnode->getMinKey();
    });
    if(left_min_key != left_bloom->min_key) {
        cout<< "min key mismatch after split: " << left_min_key << " " << left_bloom->min_key << endl;
    }

    Key_t right_min_key = read_consistent(right_bloom->version, [&](){
        return right_vnode->getMinKey();
    });
    if(right_vnode->getMinKey() != right_bloom->min_key) {
        cout<< "next min key mismatch after split: " << right_min_key << " " << right_bloom->min_key << endl;
    }
#endif
    return true;
}

//parent_inode is the last level inode that contains the targetVnode
//bool updateParentInodeAfterSplit(Inode *parent_inode, Vnode *targetVnode, std::vector<Inode *> &updates, int &last_idx, int &idx_to_next_level)
bool TandemIndex::updateParentInodeAfterSplit(Inode *parent_inode, Vnode *targetVnode,
                                               std::vector<Inode *> &updates,
                                               int &last_idx, int &idx_to_next_level)
{
    write_lock(parent_inode->version);
    
    if(parent_inode->hdr.last_index != last_idx) {
        write_unlock(parent_inode->version);
        return true;
    }

    BloomFilter* target_bloom = &valueList->bf[targetVnode->getId()];
    Key_t targetKey = read_consistent(target_bloom->version, [&](){
        return targetVnode->getMinKey();
    });
    
    if (idx_to_next_level >= 100 && !parent_inode->isSGPUnbalanced(idx_to_next_level - 100)) {
        parent_inode->sgps[idx_to_next_level - 100].covered_nodes++; 
        //TODO [ckpt] : log SGP update
        write_unlock(parent_inode->version);
        return true;
    } else if (!parent_inode->checkForActivateNextGP(idx_to_next_level)) {
        parent_inode->gps[idx_to_next_level].covered_nodes++;
#if ENABLE_DELTA_LOG
        // 单槽 delta 改为批量聚合
        ckptLog->batcher().addDeltaSlot(
            parent_inode->getId(),
            parent_inode->hdr.last_index,
            parent_inode->hdr.next,
            parent_inode->hdr.parent_id,
            static_cast<int16_t>(idx_to_next_level),
            parent_inode->gps[idx_to_next_level].key,
            parent_inode->gps[idx_to_next_level].value,
            parent_inode->gps[idx_to_next_level].covered_nodes
        );
#endif
        // FULL 快照也改为批量累积（如仍需保守冗余）
        dram_log_entry_t *entry = new dram_log_entry_t(parent_inode->getId(),
            parent_inode->hdr.last_index, parent_inode->hdr.next,
            parent_inode->hdr.level, parent_inode->hdr.parent_id);
        for (int i = 0; i <= parent_inode->hdr.last_index; i++) {
            entry->setKeyVal(i, parent_inode->gps[i].key,
                                parent_inode->gps[i].value,
                                parent_inode->gps[i].covered_nodes);
        }
        ckptLog->batcher().addFull(entry);
        write_unlock(parent_inode->version);
        return true;
    }

    int pos = -1;
    if(parent_inode->linkInactiveSGP(targetKey, targetVnode->getId(), pos, 1)) {
        // TODO [ckpt] : log SGP linking
        write_unlock(parent_inode->version);
        return true;
    }

    if (parent_inode->activateGPForVnode(targetKey, targetVnode->getId(), pos, 1)) {
        // 结构变化时仍记录 FULL（批量）
        dram_log_entry_t *entry = new dram_log_entry_t(parent_inode->getId(),
            parent_inode->hdr.last_index, parent_inode->hdr.next,
            parent_inode->hdr.level, parent_inode->hdr.parent_id);
        for (int i = 0; i <= parent_inode->hdr.last_index; i++) {
            entry->setKeyVal(i, parent_inode->gps[i].key,
                                parent_inode->gps[i].value,
                                parent_inode->gps[i].covered_nodes);
        }
        ckptLog->batcher().addFull(entry);
        write_unlock(parent_inode->version);
        return true;
    } else {
        for (size_t i = 1; i < updates.size(); ++i) {
            updates[i]->setParent(updates[i-1]->getId());
        }
        assert(parent_inode->hdr.last_index == fanout / 2 - 1);
        addToRebalanceQueue(parent_inode);
        write_unlock(parent_inode->version);
        return true;
    }
}

Val_t TandemIndex::lookup(Key_t key)
{
    for(;;) {
        int idx = -1;
        int current_level = mainIndex->getLevel();
        if(current_level <= 0) {
            return -1;
        }

        Inode *header = mainIndex->getHeader(current_level - 1);
        if(header == nullptr) {
            return -1;
        }

#if 0
        int vnode_id; const CachedVnodeImage* img = nullptr;
        if(mainIndex->tryGetVnodeCopyForKey(key, vnode_id, img) && img) {
            Val_t v;
            if(mainIndex->probeVnodeCopyValue(img, key, v)) {
                return v;
            }
        }
#endif
        InodeSnapShort snap{};
        Inode *parent_inode = mainIndex->lookup(key, header, current_level - 1, idx, snap);
        if(parent_inode == nullptr) {
            cout << " Failed to lookup the key in the main index." << endl;
            return -1;
        }

        if (!mainIndex->validateSnapShort(parent_inode, snap, key)) { //validate sgp [TODO]
            continue; // 并发修改导致失效，重试
        }

        //mainIndex->populate_cache_shards(key, parent_inode, current_level);
        int vnode_id ;
        if(snap.sgp_key != 0) {
            vnode_id = snap.sgp_value;
        } else {
            vnode_id = snap.gp_value;
        }
        const int start_vnode_id = vnode_id;
        const int current_last_idx = snap.last_index;

        BloomFilter *bloom = &valueList->bf[start_vnode_id];
        if(bloom == nullptr) {
            return -1;
        }
        int current_vnode_id = start_vnode_id;
        for (;;) {
            struct Snap {
                Val_t out{};
                bool can_move{false};
                int  next_id{-1};
            };

            Snap s = read_consistent(bloom->version, [&]() -> Snap {
                Snap res;
                // (b) Decide whether we should move right
                int next = bloom->next_id;
                if(next == -1) {
                    return res;
                }
                BloomFilter* nb = &valueList->bf[next]; // 新增：右节点的版本源
                if(!nb) {
                    return res;
                }

                Key_t next_min = read_consistent(nb->version, [&]() {
                    return nb->getMinKey();
                });
                res.can_move = (key >= next_min);
                res.next_id = next;
                if(!res.can_move) {
                // (c) Try to lookup in the current vnode
                    Val_t tmp;
                    Vnode* vnode = valueList->pmemVnodePool->at(current_vnode_id);
                    if (vnode->lookupWithoutFilter(key, tmp, bloom)) {
                        res.out = tmp;
                        //mainIndex->rememberVnodeCopyAfterLookup(key, vnode);
                    } else {
                        res.out = -1;
                    }
                }
                return res;
            });
            if(s.can_move == false) {
                //mainIndex->rememberVnodeCopyAfterLookup(key, valueList->pmemVnodePool->at(current_vnode_id));
                return s.out;
            }

            BloomFilter* next_bloom = &valueList->bf[s.next_id];
            bloom = next_bloom;
            current_vnode_id = s.next_id;
        } 
    }
}

#if 0
Val_t TandemIndex::lookup(Key_t key)
{
    int idx = -1;

    // 获取起始层级和header节点
    int current_level = mainIndex->getLevel();
    if(current_level <= 0) {
        return -1;
    }
    
    Inode *header = mainIndex->getHeader(current_level - 1);
    if(header == nullptr) {
        return -1;
    }
    
    // lock on the header
    std::shared_lock<std::shared_mutex> header_lock(mainIndex->inode_locks[header->getId()]);
    
    // use mainIndex to lookup the key, header_lock is now locked
    InodeSnapShort snap{};
    Inode *target = mainIndex->lookup(key, header, current_level - 1, idx, snap);
    if(target == nullptr) {
        return -1;
    }
    
    // now header_lock is still held, as lock of target
    int vnode_id = target->gps[idx].value;
    header_lock.unlock();
    BloomFilter *bloom = &valueList->bf[vnode_id];
    if(bloom == nullptr) {
        return -1;
    }
    // ----- 3) Lock-free traversal of vnode chain -----
    int current_vnode_id = vnode_id;
    for (;;) {
        struct Snap {
            Val_t out{};
            bool can_move{false};
            int  next_id{-1};
        };
        

        Snap s = read_consistent(bloom->version, [&]() -> Snap {
            Snap res;

            // (b) Decide whether we should move right
            int next = bloom->next_id;
            if(next == -1) {
                return res;
            }
            BloomFilter* nb = &valueList->bf[next]; // 新增：右节点的版本源
            if(!nb) {
                return res;
            }

            Key_t next_min = read_consistent(nb->version, [&]() {
                return nb->getMinKey();
            });
            res.can_move = (key >= next_min);
            res.next_id = next;
            if(!res.can_move) {
                // (c) Try to lookup in the current vnode
                Val_t tmp;
                Vnode* vnode = valueList->pmemVnodePool->at(current_vnode_id);
                if (vnode->lookupWithoutFilter(key, tmp, bloom)) {
                    res.out = tmp;
                } else {
                    res.out = -1;
                }
            }
            return res;
        });
        if(s.can_move == false) {
            return s.out;
        }

        BloomFilter* next_bloom = &valueList->bf[s.next_id];
        bloom = next_bloom;
        current_vnode_id = s.next_id;
    }
}
#endif

void TandemIndex::createLogFlushThread()
{
    g_spinLock.lock();
    logFlushThread = new std::thread(&TandemIndex::logFlushThreadExec, this, 0);
    wtInitialized = true;
    g_spinLock.unlock();
}

void TandemIndex::logFlushThreadExec(int id)
{
    LogFlushThread lft(id, this->ckptLog, this->pmemRecoveryArray);
    while(true)
    {
        g_spinLock.lock();
        if(!wtInitialized) {
            g_spinLock.unlock();
            usleep(500);
            continue;
        }else {
            g_spinLock.unlock();
            break;
        }
        g_spinLock.unlock();
    }
    while(!g_endTandem.load(std::memory_order_relaxed)) {
        usleep(200);
        lft.LogFlushOperation();
    }
    // 退出前做一次最终 drain，确保无尾批残留
    lft.LogFlushOperation();
}

void TandemIndex::createLogMergeThread()
{
    g_spinLock.lock();
    logMergeThread = new std::thread(&TandemIndex::logMergeThreadExec, this, 0);
    mgInitialized = true;
    g_spinLock.unlock();
}

void TandemIndex::logMergeThreadExec(int id)
{
    LogMergeThread lmt(id, this->ckptLog, this->pmemRecoveryArray);
    while(true)
    {
        g_spinLock.lock();
        if(!mgInitialized) {
            g_spinLock.unlock();
            usleep(500);
            continue;
        }else {
            g_spinLock.unlock();
            break;
        }
        g_spinLock.unlock();
    }
    while(!g_endTandem.load(std::memory_order_relaxed)) {
        usleep(200);
        lmt.logMergeOperation();
    }
    // 退出前做一次最终 drain
    lmt.logMergeOperation();
}

void TandemIndex::createRebalanceThread() 
{
    g_spinLock.lock();
    for(int i = 0; i < MAX_REBALANCE_THREADS; i++) {
        if(rebalanceThread[i] == nullptr) {
            rebalanceThread[i] = new std::thread(&TandemIndex::rebalanceThreadExec, this, i);
        }
    }
    g_spinLock.unlock();
}

void TandemIndex::recover(Key_t key)
{
    //recoveryManager->recoveryOperation(key);
}

void TandemIndex::rebalanceThreadExec(int id)
{
    // wait until mgInitialized is true
    while(true) {
        g_spinLock.lock();
        if(!mgInitialized) {
            g_spinLock.unlock();
            usleep(500);
            continue;
        } else {
            g_spinLock.unlock();
            break;
        }
    }
    
    // main loop: process rebalance queue
    while(!g_endTandem.load(std::memory_order_relaxed)) {
        Inode* inode = nullptr;
        
        // 尝试从队列中获取重平衡任务，并将其标记为正在处理
        if(getFromRebalanceQueue(inode)) {
            
            Inode* parent_inode = mainIndex->getParentInode(inode);
            if(parent_inode != nullptr) {
                assert(parent_inode->hdr.last_index >= 0);
            }
            assert(inode->hdr.last_index >= 0);
            int ret = mainIndex->fastRebalance(inode, parent_inode);
            if(ret == 2) {
                assert(parent_inode->hdr.last_index == fanout / 2 - 1);
#if 0
                cout << " in Rebalance, Parent inode needs rebalancing. id: " << parent_inode->getId()<< endl;
                
                for(int i = 0; i < parent_inode->hdr.last_index; i++) {
                    cout << " gp " << i << " key: " << parent_inode->gps[i].key << " value: " << parent_inode->gps[i].value << " covered_nodes: " << parent_inode->gps[i].covered_nodes << endl;
                }
#endif
                addToRebalanceQueue(parent_inode); // added to rebalance queue
            }
            
            // 处理完成，移除标记
            {
                std::lock_guard<std::mutex> lock(rebalanceQueueMutex);
                nodesInRebalanceProcess.erase(inode);
            }
        } else {
            // 队列为空，休眠一段时间
            usleep(1000); // 1ms
        }
    }
}

// 添加任务到重平衡队列
void TandemIndex::addToRebalanceQueue(Inode *&inode)
{
    std::lock_guard<std::mutex> lock(rebalanceQueueMutex);
    // 检查节点是否已在队列中或正在被处理，防止重复添加
    if (rebalancingInodes.find(inode) == rebalancingInodes.end() &&
        nodesInRebalanceProcess.find(inode) == nodesInRebalanceProcess.end()) {
        rebalanceQueue.push(inode);
        rebalancingInodes.insert(inode);
    }else {
        //cout << "inode " << inode->getId() << " is already in the rebalance queue or being processed." << endl;
    }
}

bool TandemIndex::getFromRebalanceQueue(Inode* &inode)
{
    std::lock_guard<std::mutex> lock(rebalanceQueueMutex);
    if(rebalanceQueue.empty()) {
        return false;
    }
    
    inode = rebalanceQueue.front();
    rebalanceQueue.pop();
    rebalancingInodes.erase(inode); // 从“等待”集合中移除
    nodesInRebalanceProcess.insert(inode); // 添加到“正在处理”集合
    return true;
}

bool TandemIndex::update(Key_t key, Val_t value)
{
    for (;;) { // 重试环
        int idx = -1;
        Vnode *target_vnode = nullptr;
        std::vector<Inode *> updates;
        updates.reserve(MAX_LEVEL);

        int current_level = mainIndex->getLevel();
        if (current_level <= 0) return false;

        Inode *header = mainIndex->getHeader(current_level - 1);
        if (!header) return false;

        // 新：lookup 时当场捕获叶子的短快照
        InodeSnapShort snap{};
        Inode *parent_inode = mainIndex->lookupForInsertWithSnap(key, header, current_level - 1, idx, updates, snap);
        
        // 空结构（只 header）
        if (parent_inode == nullptr) {
            bool ret = insertWithNewInodes(key, value, target_vnode);
            if (!ret) {
                std::cerr << "Failed to insert with new inodes." << std::endl;
                return false;
            }
            if (target_vnode != nullptr) {
                ret = mainIndex->add(target_vnode);
                if (!ret) {
                    std::cout << "There is smaller key already inserted in the index." << std::endl;
                }
            }
            return true;
        }

        // 使用短快照快速验证（优先用 ver_snap）
        if (!mainIndex->validateSnapShort(parent_inode, snap, key)) {
            //cout << "SnapShort validation failed for key: " << key << endl;
            continue; // 并发修改导致失效，整条路径重试
        }
        //mainIndex->populate_cache_shards(key, parent_inode, current_level);

        assert(parent_inode->hdr.level == 0); // 叶子层

        int vnode_id ;
        if(snap.sgp_key != 0) {
            vnode_id = snap.sgp_value;
        } else {
            vnode_id = snap.gp_value;
        }
        const int start_vnode_id = vnode_id;
        const int current_last_idx = snap.last_index; // the last index of the target inode at the time of lookup

        Vnode *start_vnode = valueList->pmemVnodePool->at(start_vnode_id);
        if (!start_vnode) {
            std::cerr << "Failed to get vnode (id=" << start_vnode_id << ")." << std::endl;
            return false;
        }
        target_vnode = start_vnode;
        BloomFilter *target_vnode_bloom = &valueList->bf[target_vnode->getId()];

        //Vnode *start_vnode_replica = new Vnode(*start_vnode); // create a replica of the target vnode for validation

        // 快路径：尝试直接插入
        if (insertInVnodeChain(target_vnode, target_vnode_bloom, key, value)) {
            return true;
        }

        // 慢路径：节点满 -> split
        Vnode* new_vnode = nullptr; //new_vnode and target_vnode share the contents of old target_vnode before split
        BloomFilter* new_bloom = nullptr;
        if (!handleNodeFullAndSplit(target_vnode, target_vnode_bloom, key, value, new_vnode, new_bloom)) {
            std::cerr << "Failed to handle node full and split." << std::endl;
            return false;
        }
        if(new_vnode == nullptr) {// no split happened, all keys are the same.
            return true;
        }

        // 分裂后更新父节点（内部会短写锁再次核对 last_index）
        int last_idx_mut = current_last_idx;
        if (!updateParentInodeAfterSplit(parent_inode, new_vnode, updates, last_idx_mut, idx)) {
            std::cerr << "Failed to update the parent inode after split." << std::endl;
            return false;
        }
        return true;
    } // for retry
}


#if 0
void TandemIndex::update(Key_t key, Val_t value)
{
    int idx = -1;
    Key_t targetKey;
    Vnode *targetVnode = nullptr; // new vnode to be inserted
    Inode *inode = mainIndex->lookup(key, idx);
    if(inode == nullptr) {
        std::cout << "Failed to find the inode for the key: " << key << std::endl;
        return;
    }
    std::shared_lock<std::shared_mutex> inode_lock(mainIndex->inode_locks[inode->getId()]);
    Vnode *vnode = valueList->pmemVnodePool->at(inode->gps[idx].value);
    inode_lock.unlock();
    if(vnode == nullptr) {
        std::cout << "Failed to find the vnode for the key: " << key << std::endl;
        return;
    }
    while(true) {
        //std::unique_lock<std::shared_mutex> lock(vnode->hdr.mtx);
        BloomFilter *bloom = &valueList->bf[vnode->hdr.id];
        std::unique_lock<std::shared_mutex> lock(bloom->vnode_mtx);
        if(key > vnode->getMaxKey() && vnode->hdr.next != -1) {
        //if(vnode->hdr.next != -1 && !valueList->bf[vnode->hdr.id].mightContain(key)) {
            vnode = valueList->pmemVnodePool->at(vnode->hdr.next);
        }else {
            int pos = -1;
            if(vnode->lookup(key,pos, &valueList->bf[vnode->hdr.id])) {
                if(!vnode->isFull()) {
                    vnode->insert(key, value, &valueList->bf[vnode->hdr.id]);
                    vnode->hdr.unsetBit(pos);
                    return;
                }else {
                    targetVnode = valueList->pmemVnodePool->getNextNode();  // get a new vnode;
                    valueList->split(vnode, targetVnode); //redistribute the keys between the two vnodes
                    if(key <= vnode->getMaxKey()) { // key is smaller than the max key of the previous value node after split
                        if(!vnode->lookup(key, pos, &valueList->bf[vnode->hdr.id])) {
                            vnode->insert(key, value, &valueList->bf[vnode->hdr.id]);
                            vnode->hdr.unsetBit(pos);
                        }
                    }else{
                        //std::unique_lock<std::shared_mutex> lock_target(targetVnode->hdr.mtx);
                        BloomFilter *target_bloom = &valueList->bf[targetVnode->hdr.id];
                        std::unique_lock<std::shared_mutex> lock_target(target_bloom->vnode_mtx);
                        if(!targetVnode->lookup(key, pos, target_bloom)) {
                            targetVnode->insert(key, value, &valueList->bf[targetVnode->hdr.id]);
                            targetVnode->hdr.unsetBit(pos);
                        }
                    }
                    break;
                }
            }else {
                std::cout << "Failed to find the key in the vnode: " << vnode->hdr.id << std::endl;
                return;
            }
        }
    }
    {
        
        std::unique_lock<std::shared_mutex> inode_wlock(mainIndex->inode_locks[inode->getId()]);
        //inode->hdr.coveredNodes++;
        if(inode->checkForActivateNextGP(idx)) {
            //activate GP
            int pos = -1;
            {
                //std::shared_lock<std::shared_mutex> lock_target(targetVnode->hdr.mtx);
                BloomFilter *target_bloom = &valueList->bf[targetVnode->getId()];
                std::shared_lock<std::shared_mutex> lock_target(target_bloom->vnode_mtx);
                targetKey = targetVnode->getMinKey();
            }
            if(inode->activateGP(targetKey, targetVnode->getId(), pos, 1)) {
                dram_log_entry_t *entry = new dram_log_entry_t(inode->getId(), inode->hdr.last_index, inode->hdr.next, inode->hdr.level);
                for (int i = 0; i <= inode->hdr.last_index; i++) {
                    entry->setKeyVal(i, inode->gps[i].key, inode->gps[i].value, inode->gps[i].covered_nodes);
                }
                // entry->setCoveredNodes(inode->hdr.coveredNodes); // <-- 移除
                // entry->setLastIndex(inode->hdr.last_index); // <-- 已在构造函数中处理
                ckptLog->enq(entry);
            }else {
                //rebalance the main index, if necessary
                addToRebalanceQueue(inode);
            }
        }else {
            //no needs to activate GP, just return 
            return;
        }
    } 
    // 重平衡现在通过队列异步处理
    
}
#endif

bool TandemIndex::scan(Key_t key, size_t range, std::priority_queue<Key_t, std::vector<Key_t>, std::greater<Key_t>> &result)
{
    for(;;) {
        int idx = -1;
        int current_level = mainIndex->getLevel();
        if(current_level <= 0) {
            return -1;
        }

        Inode *header = mainIndex->getHeader(current_level - 1);
        if(header == nullptr) {
            return -1;
        }
        InodeSnapShort snap{};
        Inode *parent_inode = mainIndex->lookup(key, header, current_level - 1, idx, snap);
        if(parent_inode == nullptr) {
            cout << " Failed to lookup the key in the main index." << endl;
            return -1;
        }

        if (!mainIndex->validateSnapShort(parent_inode, snap, key)) {
            continue; // 并发修改导致失效，重试
        }

        //mainIndex->populate_cache_shards(key, parent_inode, current_level);
        int vnode_id ;
        if(snap.sgp_key != 0) {
            vnode_id = snap.sgp_value;
        } else {
            vnode_id = snap.gp_value;
        }
        const int start_vnode_id = vnode_id;
        const int current_last_idx = snap.last_index;

        BloomFilter *bloom = &valueList->bf[start_vnode_id];
        if(bloom == nullptr) {
            return -1;
        }
        int current_vnode_id = start_vnode_id;
        int remaining_range = range;
        for (;;) {
            struct Snap {
                std::priority_queue<Key_t, std::vector<Key_t>, std::greater<Key_t>> out;
                bool can_move{false};
                int  next_id{-1};
            };

            Snap s = read_consistent(bloom->version, [&]() -> Snap {
                Snap res;
                // (b) Decide whether we should move right
                int next = bloom->next_id;
                if(next == -1) {
                    return res;
                }
                BloomFilter* nb = &valueList->bf[next]; // 新增：右节点的版本源
                if(!nb) {
                    return res;
                }

                Key_t next_min = read_consistent(nb->version, [&]() {
                    return nb->getMinKey();
                });
                res.can_move = (key >= next_min);
                res.next_id = next;
                if(!res.can_move) {
                // (c) Try to lookup in the current vnode
                    Vnode* vnode = valueList->pmemVnodePool->at(current_vnode_id);
                    if (vnode->scan(key, remaining_range, res.out)) {
                        //do nothing
                    }else {
                        res.out.push(-1);
                    }
                }
                return res;
            });
            mergeScanResultsPQ(s.out, result);
            if(remaining_range <= 0 || s.can_move == false) {
                return true;
            }

            BloomFilter* next_bloom = &valueList->bf[s.next_id];
            bloom = next_bloom;
            current_vnode_id = s.next_id;
        } 
    }
}

bool TandemIndex::scan(Key_t key, size_t range,
                         std::vector<Key_t> &result)
{
    for(;;) {
        int idx = -1;
        int current_level = mainIndex->getLevel();
        if(current_level <= 0) {
            return -1;
        }

        Inode *header = mainIndex->getHeader(current_level - 1);
        if(header == nullptr) {
            return -1;
        }
        InodeSnapShort snap{};
        Inode *parent_inode = mainIndex->lookup(key, header, current_level - 1, idx, snap);
        if(parent_inode == nullptr) {
            cout << " Failed to lookup the key in the main index." << endl;
            return -1;
        }

        if (!mainIndex->validateSnapShort(parent_inode, snap, key)) {
            continue; // 并发修改导致失效，重试
        }

        //mainIndex->populate_cache_shards(key, parent_inode, current_level);
        int vnode_id ;
        if(snap.sgp_key != 0) {
            vnode_id = snap.sgp_value;
        } else {
            vnode_id = snap.gp_value;
        }
        const int start_vnode_id = vnode_id;
        const int current_last_idx = snap.last_index;

        BloomFilter *bloom = &valueList->bf[start_vnode_id];
        if(bloom == nullptr) {
            return -1;
        }
        int current_vnode_id = start_vnode_id;
        int remaining_range = range;
        for (;;) {
            struct Snap {
                int ret_id{-1};
                bool can_move{false};
                int  next_id{-1};
            };

            Snap s = read_consistent(bloom->version, [&]() -> Snap {
                Snap res;
                // (b) Decide whether we should move right
                int next_id = bloom->next_id;
                if(next_id == -1) {
                    res.can_move = false;
                    res.ret_id = current_vnode_id;
                    return res;
                }
                BloomFilter* nb = &valueList->bf[next_id]; // 新增：右节点的版本源
                if(!nb) {
                    return res;
                }

                Key_t next_min = read_consistent(nb->version, [&]() {
                    return nb->getMinKey();
                });
                res.can_move = (key >= next_min);
                res.next_id = next_id;
                if(!res.can_move) {
                // (c) Try to lookup in the current vnode
                    res.ret_id = current_vnode_id;
                    if(res.ret_id == -1) {
                        std::cerr << "Failed to get vnode id during scan." << std::endl;
                    }
                }
                if(res.ret_id == -1 && res.can_move == false && res.next_id == -1) {
                    std::cerr << "Both ret_id and can_move are invalid during scan." << std::endl;
                }
                return res;
            });
            if(!s.can_move) {
                Vnode* vnode = valueList->pmemVnodePool->at(s.ret_id);
                if(vnode == nullptr) {
                    std::cerr << "Failed to get vnode during scan. id: " << s.ret_id << std::endl;
                    return false;
                }
                   // 将扫描动作与 BloomFilter 的版本快照绑定，避免 torn-read
                struct ScanPack { int rem; std::vector<Key_t> out; };
                ScanPack pack = read_consistent(bloom->version, [&]() -> ScanPack {
                    ScanPack r;
                    r.out.reserve(remaining_range);
                    int want = remaining_range; // 使用本地副本
                    r.rem = vnode->scan(key, want, r.out);
                    return r;
                });

                // 合并一次快照内的结果，再更新剩余需求
                mergeScanResultsVec(pack.out, result);
                remaining_range = pack.rem;

                if (remaining_range <= 0) return true;   // 已满足
                if (s.next_id == -1)    return true;     // 无右邻居，结束

                BloomFilter* next_bloom = &valueList->bf[s.next_id];
                bloom = next_bloom;
                current_vnode_id = s.next_id;
                continue;
            }

            BloomFilter* next_bloom = &valueList->bf[s.next_id];
            current_vnode_id = s.next_id;
            bloom = next_bloom;
        }
    }
}

void TandemIndex::mergeScanResultsVec(std::vector<Key_t> &src, std::vector<Key_t> &dest)
{
    for(auto key : src) {
        if(key == -1) {
            continue;
        }
        dest.emplace_back(key);
    }
}

void TandemIndex::mergeScanResultsPQ(std::priority_queue<Key_t, std::vector<Key_t>, std::greater<Key_t>> &src,
                                 std::priority_queue<Key_t, std::vector<Key_t>, std::greater<Key_t>> &dest)
{
    while(!src.empty()) {
        Key_t key = src.top();
        src.pop();
        if(key == -1) {
            continue;
        }
        dest.push(key);
    }
}

#if 0
void TandemIndex::scan(Key_t key, size_t range, std::priority_queue<Key_t, std::vector<Key_t>, std::greater<Key_t>> &result)
{
    
    int idx = -1;
    Inode *inode = mainIndex->lookup(key, idx);
    Vnode *vnode = nullptr;
    if(inode == nullptr) {
        std::cout << "Failed to find the inode for the key: " << key << std::endl;
        return;
    }
    {
        std::shared_lock<std::shared_mutex> lock(mainIndex->inode_locks[inode->getId()]);
        vnode = valueList->pmemVnodePool->at(inode->gps[idx].value);
    }
    int remaining_range = range;
    while(true) {
        //std::shared_lock<std::shared_mutex> lock(vnode->hdr.mtx);
        BloomFilter *bloom = &valueList->bf[vnode->hdr.id];
        std::shared_lock<std::shared_mutex> lock(bloom->vnode_mtx);
        remaining_range=vnode->scan(key, remaining_range, result);
        if(remaining_range > 0 && vnode->hdr.next != -1) {
            vnode = valueList->pmemVnodePool->at(vnode->hdr.next);
        }else {
            if(remaining_range > 0) {
                std::cout << "Failed to scan key: " << key << " with range: " << range <<" remaining_range: " << remaining_range << std::endl;
            }
            return;
        }
    }
}
#endif

struct AnchorParams {
  double inserts_per_anchor = 10.0; // how many future inserts justify one SGP
  int    max_per_node       = 8;    // cap [remaining empty slots in SGP array]
  size_t future_epochs      = 1;    // forecast horizon
  size_t window_buckets     = 30;   // hot-region width for GetHottestRegion
};

// tiny helper for pretty-printing vectors
static std::string join_u64(const std::vector<uint64_t>& v) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < v.size(); ++i) {
        oss << v[i];
        if (i + 1 < v.size()) oss << ", ";
    }
    oss << "]";
    return oss.str();
}

void TandemIndex::maybeActivateHotRegion() {
    // Choose a window of buckets to represent the “region” (e.g., 16 buckets)
    
    tl::Region hot{};
    const size_t window = 20;
    if (!tracker_->GetHottestRegion(window, &hot)) {
        //std::cout << "[SGP] no completed epoch yet; skip activation\n";
        return;  // no completed epoch yet
    }
    //std::cout << "[SGP] hottest region = [" << hot.start << ", " << hot.end << ") (window=" << window << ")\n";

    // Forecast one future epoch
    double forecast = 0.0;
    if (!tracker_->GetNumInsertsInKeyRangeForNumFutureEpochs(
        hot.start, hot.end, 1, &forecast)) {
        //std::cout << "[SGP] forecast failed at GetNumInsertsInKeyRangeForNumFutureEpochs; skip\n";
        return;
    }
    //std::cout << std::fixed << std::setprecision(1) << "[SGP] forecast in hot region (next epoch) ≈ " << forecast << "\n";

    // Map to covering nodes at an appropriate level
    int L = 0; // TODO start from lowest level [propagate to parent?]
    auto nodes = mainIndex->nodesCoveringRangeAtLevel(hot.start, hot.end, L);
    if (nodes.empty()) {
        //std::cout << "[SGP] no covering inodes at level " << L << "\n";
        return;
    }
    //std::cout << "[SGP] covering inodes at level " << L << ": " << nodes.size() << "\n";

    //pill the last histogram once
    std::vector<uint64_t> B; // P+1 partition boundaries of last competed epoch
    std::vector<size_t>   C; // insert counts per partition from last epoch
    if (!tracker_->GetLastEpochHistogram(B, C)) {
        //std::cout << "[SGP] no last-epoch histogram; skip\n";
        return;
    }

    // per node intersect, forecast , choose how many SGPs, place anchors, activate sgp
    const AnchorParams P{};
    for (auto* inode : nodes) { 
        if(getFromRebalanceQueue(inode)){
            //std::cout << "  [SGP] inode " << inode->getId() << "queued for rebalance; skip\n";
            continue;
        }

        // no room for speculation
        if (inode->isSGPFull() || inode->isFull()) { //TODO: check if it increase number of splits
            addToRebalanceQueue(inode);
            //std::cout << "  [SGP] inode " << inode->getId() <<"no room for speculation - added to rebanance queue; skip\n"; 
            continue;
        }
        

        //intersection of node and hot region
        const uint64_t nmin = inode->getMinKey();
        const uint64_t nmax = inode->getMaxKey();              // assume inclusive
        const uint64_t S = std::max(hot.start, nmin);
        const uint64_t E = std::min(hot.end, nmax);   // make end exclusive [nmax-1?]
        
        if (S >= E) {
            //std::cout << "  [SGP] inode " << inode->getId() << " range=[" << nmin << "," << nmax << "] no overlap; skip\n";
            continue;
        }

        //forecast how many inserts will hit this node
        double pred = 0.0;
        if (!tracker_->GetNumInsertsInKeyRangeForNumFutureEpochs(S, E, P.future_epochs, &pred) || pred <= 0.0) {
            //std::cout << "  [SGP] inode " << inode->getId() << " slice=[" << S << "," << E << ") forecast≈" << pred << "; skip\n";
            continue;
        }

        //decide how many anchors for this node
        //P.max_per_node = (fanout / 2) - inode->sgp_last_index
        int m = std::clamp<int>(std::lround(pred / P.inserts_per_anchor), 1, P.max_per_node);

        //place anchors by density inside [S,E]
        auto anchors = tracker_->quantileAnchorsInWindow(B, C, S, E, (size_t)m);
        //std::cout << "  [SGP] inode " << inode->getId()
        //          << " node_range=[" << nmin << "," << nmax << "]"
        //          << " slice=[" << S << "," << E << ")"
        //          << " pred≈" << pred << " -> anchors=" << anchors.size()
        //          << " keys=" << join_u64(anchors) << "\n";

        //check rebalance again before activating SGPs [no need activating SGPS in node to be rebalanced]
        if(getFromRebalanceQueue(inode)){
            //std::cout << "  [SGP] inode " << inode->getId() << "queued for rebalance; skip\n";
            continue;
        }
#if 0
        // activate SGPs at those anchor keys
        // TODO - get lock
        for(uint64_t key : anchors){
            inode->activateSGP(key);
            //TODO - checkpoint or flush
        }
#endif        
    } 
    // clear current epoch histogram
    tracker_->DropLastEpochHistogram();
    //std::cout << "[SGP] Speculation completed, dropping last epoch histogram\n";

}

#if 0
void TandemIndex::remove(int key)
{
    mainIndex->remove(key);
}



void TandemIndex::print()
{
    mainIndex->print();
}
#endif
