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
#include <numeric>
#include <sstream>
#include <atomic>
#include <cstdint>
#include <stdexcept>

namespace {
static std::string format_bytes(size_t bytes) {
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::ostringstream oss;
    oss << bytes << " B (" << std::fixed << std::setprecision(2) << mib << " MiB)";
    return oss.str();
}

static constexpr uint64_t kLayoutMetaMagicTag = 0x54414E44454D4C59ull; // "TANDEMLY"
static inline uint64_t current_layout_magic() {
    return kLayoutMetaMagicTag
        ^ (static_cast<uint64_t>(vnode_fanout) << 48)
        ^ (static_cast<uint64_t>(sizeof(Vnode)) << 24)
        ^ (static_cast<uint64_t>(sizeof(BloomFilter)));
}
}

#ifndef ENABLE_THREAD_KEY_CACHE
#define ENABLE_THREAD_KEY_CACHE 1
#endif

#ifndef THREAD_KEY_CACHE_CAP
#define THREAD_KEY_CACHE_CAP 512 
#endif

#ifndef ENABLE_HOTPATH_DEBUG_LOG
#define ENABLE_HOTPATH_DEBUG_LOG 0
#endif

#define THREAD_KEY_CACHE_WAYS 8
#define THREAD_KEY_CACHE_SETS (THREAD_KEY_CACHE_CAP / THREAD_KEY_CACHE_WAYS)

#if ENABLE_THREAD_KEY_CACHE
struct ThreadKeyCacheEntry {
    Key_t key;
    Val_t val;
    uint8_t used;
};

struct ThreadKeyCache {
    ThreadKeyCacheEntry entries[THREAD_KEY_CACHE_SETS][THREAD_KEY_CACHE_WAYS];
    uint8_t hands[THREAD_KEY_CACHE_SETS];

    ThreadKeyCache() {
        for (int i = 0; i < THREAD_KEY_CACHE_SETS; ++i) {
            hands[i] = 0;
            for (int j = 0; j < THREAD_KEY_CACHE_WAYS; ++j) {
                entries[i][j].used = 0;
            }
        }
    }

    inline size_t hash(Key_t k) {
        return (static_cast<uint64_t>(k) * 11400714819323198485llu) % THREAD_KEY_CACHE_SETS;
    }

    inline bool get(Key_t k, Val_t &out) {
        size_t set_idx = hash(k);
        for (int i = 0; i < THREAD_KEY_CACHE_WAYS; ++i) {
            if (entries[set_idx][i].used && entries[set_idx][i].key == k) {
                out = entries[set_idx][i].val;
                return true;
            }
        }
        return false;
    }

    inline void put(Key_t k, Val_t v) {
        size_t set_idx = hash(k);
        
        for (int i = 0; i < THREAD_KEY_CACHE_WAYS; ++i) {
            if (entries[set_idx][i].used && entries[set_idx][i].key == k) {
                entries[set_idx][i].val = v;
                return;
            }
        }

        int way_idx = hands[set_idx];
        entries[set_idx][way_idx].key  = k;
        entries[set_idx][way_idx].val  = v;
        entries[set_idx][way_idx].used = 1;

        hands[set_idx] = (way_idx + 1) % THREAD_KEY_CACHE_WAYS;
    }
};

thread_local ThreadKeyCache g_threadKeyCache;
#endif // ENABLE_THREAD_KEY_CACHE

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
    size_t num_inserts_per_epoch = 1000; // The number of inserts in each InsertTracker epoch; the total elements of the equi-depth histogram used for insert forecasting.
    size_t num_partitions = 100; // The number of bins in the insert forecasitng histogram. TODO: make adaptive with increased node numbers
    size_t sample_size = 100; // The size of the reservoir sample based on which the partition boundaries are set at the beginning of each epoch.
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

TandemIndex::TandemIndex(string storage_path) {
    g_endTandem.store(false,std::memory_order_relaxed);
    storagePath = storage_path;
    pmemBFPool = new PmemBFPool(MAX_VALUE_NODES, storagePath);
    valueList = new ValueList(storagePath, pmemBFPool);

    {
        Vnode *metaVnode = valueList->pmemVnodePool->at(MAX_VALUE_NODES - 1);
        if (metaVnode != nullptr) {
            const bool has_persisted_data = (valueList->pmemVnodePool->getCurrentIdx() > 1);
            const uint64_t expected = current_layout_magic();
            const uint64_t persisted = metaVnode->records[0].key;
            if (!has_persisted_data) {
                metaVnode->records[0].key = expected;
                metaVnode->records[0].value = static_cast<Val_t>(vnode_fanout);
                const unsigned long rec_flush = PmemManager::align_uint_to_cacheline(sizeof(vnode_entry));
                PmemManager::flushToNVM(0, reinterpret_cast<char *>(&metaVnode->records[0]), rec_flush);
            } else if (persisted != expected) {
                std::cerr << "[FATAL] PMEM layout mismatch detected. "
                          << "Current binary expects vnode_fanout=" << vnode_fanout
                          << ", sizeof(Vnode)=" << sizeof(Vnode)
                          << ", sizeof(BloomFilter)=" << sizeof(BloomFilter)
                          << ". Please remove old PMEM files and rerun: "
                          << "rm -rf " << storagePath << "/pmem* " << storagePath << "/ckpt_log"
                          << std::endl;
                throw std::runtime_error("PMEM layout mismatch");
            }
        }
    }

    if(valueList->pmemVnodePool->getCurrentIdx() > 1) {
        if(pmemBFPool->at(0)->next_id == -1) {
           for(size_t i = 0; i <= valueList->pmemVnodePool->getCurrentIdx(); i++) {
               BloomFilter *bloom = valueList->getBloom(i);
               Vnode *vnode = valueList->pmemVnodePool->at(i);
               bloom->clear();
               bloom->next_id  = vnode->hdr.next;
               Key_t min_key = std::numeric_limits<Key_t>::max();
               for(uint64_t bm = vnode->hdr.bitmap; bm; ) {
                   int idx = __builtin_ctzll(bm);
                   bloom->add(vnode->records[idx].key, idx);
                   if(vnode->records[idx].key < min_key) {
                       min_key = vnode->records[idx].key;
                   }
                   bm &= (bm - 1);
               }
                bloom->setMinKey(min_key);
           }
        }
        setDataLoaded(true);
    }else{
        setDataLoaded(false);
    }
    pmemRecoveryArray = new PmemInodePool(sizeof(Inode), MAX_NODES, storagePath);
    recoveryManager = new RecoveryManager(pmemRecoveryArray); 
    int levels = recoveryManager->recoveryOperation();
    dramInodePool = recoveryManager->getDramInodePool();
#if ENABLE_PMEM_STATS
    ckptLog = new CkptLog(LOG_SIZE, levels-1, valueList, storagePath);
#else
    ckptLog = new CkptLog(LOG_SIZE, storagePath);
#endif
    PmemManager::flushToNVM(3, reinterpret_cast<char *>(ckptLog), sizeof(ckptLog));
    mainIndex = new DramSkiplist(ckptLog, dramInodePool, valueList);
    mainIndex->setLevel(levels);
    if(isDataLoaded()) {
        mainIndex->fillInodeCountEachLevel(levels);
        ckptLog->current_highest_level = levels - 1;
        ckptLog->current_inode_idx = dramInodePool->getCurrentIdx();
        memcpy(ckptLog->inode_count_on_each_level.data(),
               mainIndex->inode_count_on_each_level.data(),
               sizeof(int) * levels);
    }
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
    ckptLog->batcher().addFull(header_entry);
    
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
    g_endTandem.store(true, std::memory_order_relaxed);
    
    for(int i = 0; i < MAX_REBALANCE_THREADS; i++) {
        if(rebalanceThread[i] != nullptr) {
            if(rebalanceThread[i]->joinable()) rebalanceThread[i]->join();
            delete rebalanceThread[i];
            rebalanceThread[i] = nullptr;
        }
    }
    ckptLog->drainAndPerssistBatchers();
    if (logFlushThread && logFlushThread->joinable()) {
        logFlushThread->join();
        delete logFlushThread;
        logFlushThread = nullptr;
    }
    if (logMergeThread && logMergeThread->joinable()) {
        logMergeThread->join();
        delete logMergeThread;
        logMergeThread = nullptr;
    }

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

    valueList->syncBloomToPMEM(valueList->pmemVnodePool->getCurrentIdx() + 1);

    PmemManager::flushToNVM(4,
        reinterpret_cast<char *>(pmemBFPool->at(0)),
        sizeof(BloomFilter) * (valueList->pmemVnodePool->getCurrentIdx() + 1));

    //cout << "vnode count: " << valueList->pmemVnodePool->getCurrentIdx() << endl;
    mainIndex->printStats();
    printStatus();

    // 必须先停止所有使用 tracker_ 的线程，再销毁 tracker_
    // 否则线程仍在执行 tracker_->xxx() 时 tracker_ 已被析构 → SIGSEGV
    if (speculator_) {
        speculator_->Stop();
        speculator_.reset();
    }
    if (sampler_) {
        sampler_->Stop();
        sampler_.reset();
    }
    tracker_.reset();   // 所有使用方线程已停止，最后安全销毁

}

bool TandemIndex::insert(Key_t key, Val_t value)
{
#if ENABLE_PMEM_STATS
    std::shared_lock<std::shared_mutex> lk(pmemRecoveryArray->stats_mtx);
#endif
#if 0
    tracker_->Add(key); //TODO: sampling out of the critical section
    if (tracker_->LastEpochHistogramValid()) {
        SpeculationToken tok(speculation_running_); 
        if (tok.is_leader) {
            maybeActivateHotRegion();
        }
    }
#endif
    for (;;) {
        int idx = -1;
        bool is_sgp = false;
        Vnode *target_vnode = nullptr;
        std::vector<Inode *> updates;
        updates.reserve(MAX_LEVEL);

        int current_level = mainIndex->getLevel();
        if (current_level <= 0) return false;

        Inode *header = mainIndex->getHeader(current_level - 1);
        if (!header) return false;

        InodeSnapShort snap{};
        Inode *parent_inode = mainIndex->lookupForInsertWithSnap(key, header, current_level - 1, idx, is_sgp, updates, snap);
        
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

        if (!mainIndex->validateSnapShort(parent_inode, snap, key)) {
            continue;
        }

        assert(parent_inode->hdr.level == 0);

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
        BloomFilter *target_vnode_bloom = valueList->getBloom(target_vnode->getId());


        //fast path: insert into vnode chain
        if (insertInVnodeChain(target_vnode, target_vnode_bloom, key, value)) {
            return true;
        }

        //slow path: need to split the target vnode
        Vnode* new_vnode = nullptr; //new_vnode and target_vnode share the contents of old target_vnode before split
        BloomFilter* new_bloom = nullptr;
        if (!handleNodeFullAndSplit(target_vnode, target_vnode_bloom, key, value, new_vnode, new_bloom)) {
            std::cerr << "Failed to handle node full and split." << std::endl;
            return false;
        }

        //update parent inode after split
        int last_idx_mut = current_last_idx;
        if (!updateParentInodeAfterSplit(parent_inode, new_vnode, updates, last_idx_mut, idx, is_sgp)) {
            std::cerr << "Failed to update the parent inode after split." << std::endl;
            return false;
        }
        return true;
    } // for retry
}

bool TandemIndex::insertWithNewInodes(Key_t key, Val_t value, Vnode *&target_vnode)
{
    Vnode* headerVnode = valueList->getHeader();
    BloomFilter* headerbloom = valueList->getBloom(headerVnode->getId());
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

    BloomFilter* bloom = valueList->getBloom(newVnode->getId());
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
                int nid = current_bloom->next_id;               // get right node id 
                if (nid == -1) {
                    return r;
                }

                BloomFilter* next_bloom = valueList->getBloom(nid); // get right node's bloom
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

            BloomFilter* next_bloom = valueList->getBloom(s.next_id);
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
                BloomFilter* next_bloom= valueList->getBloom(nid);
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
    valueList->split(left_vnode, right_vnode);
    if(right_vnode == nullptr) { // this is the case that all the keys in left vnode are the same. we don't split but invalid all the other keys except one key.
        bool ok = left_vnode->insert(key, value, left_bloom);
        write_unlock(left_bloom->version);
        return ok;
    }
    right_bloom = valueList->getBloom(right_vnode->getId());

    // 3) Decide which node should receive (key,value), Read nextVnode’s min under its own version to avoid torn reads.
    Key_t next_right_min = read_consistent(right_bloom->version, [&](){
        return right_bloom->getMinKey();
    });

    if (sampler_) {sampler_->Submit(next_right_min);} //sample split
    if (tracker_->LastEpochHistogramValid()) { //TODO: currently one thread speculating per round
        speculator_->TrySubmit();   // TODO: optimize: avoid double atomic ops
    }

    bool ok = false;
    if (key < next_right_min) {
        ok = left_vnode->insert(key, value, left_bloom);
        write_unlock(left_bloom->version);
    } else {
        // Insert into RIGHT (new) vnode
        write_lock(right_bloom->version);
        write_unlock(left_bloom->version);
        ok = right_vnode->insert(key, value, right_bloom);
        write_unlock(right_bloom->version);
    }
    if (!ok) return false;
    return true;
}

bool TandemIndex::updateParentInodeAfterSplit(Inode *parent_inode, Vnode *targetVnode,
                                               std::vector<Inode *> &updates,
                                               int &last_idx, int &idx_to_next_level, bool &is_sgp)
{
    write_lock(parent_inode->version);
    
    if(parent_inode->hdr.last_index != last_idx) {
        write_unlock(parent_inode->version);
        return true;
    }

    BloomFilter* target_bloom = valueList->getBloom(targetVnode->getId());
    Key_t targetKey = read_consistent(target_bloom->version, [&](){
        return targetVnode->getMinKey();
    });
    
    if (is_sgp && !parent_inode->isSGPUnbalanced(idx_to_next_level)) {
        parent_inode->sgps[idx_to_next_level].covered_nodes++; 
        //TODO [ckpt] : log SGP update
        write_unlock(parent_inode->version);
        return true;
    } else if (!parent_inode->checkForActivateNextGP(idx_to_next_level)) {
        parent_inode->gps[idx_to_next_level].covered_nodes++;
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
#if ENABLE_HOTPATH_DEBUG_LOG
        std::cout << "Linking SGP at pos " << pos
              << " key=" << targetKey
              << " -> vnode " << targetVnode->getId()
              << " covering " << parent_inode->sgps[pos].covered_nodes << "\n";
#endif
        write_unlock(parent_inode->version);
        return true;
    }

    if (parent_inode->activateGPForVnode(targetKey, targetVnode->getId(), pos, 1)) {
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
#if ENABLE_THREAD_KEY_CACHE
    {
        Val_t cached;
        if (g_threadKeyCache.get(key, cached)) {
            return cached;
        }
    }
#endif
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

        if (!mainIndex->validateSnapShort(parent_inode, snap, key)) { //validate sgp [TODO]
            continue; // 并发修改导致失效，重试
        }
#if ENABLE_L2_SHARD_CACHE
        //mainIndex->populate_cache_shards(key, parent_inode, current_level);
#endif
        int vnode_id ;
        if(snap.sgp_key != 0) {
            vnode_id = snap.sgp_value;
        } else {
            vnode_id = snap.gp_value;
        }
        const int start_vnode_id = vnode_id;

        BloomFilter *bloom = valueList->getBloom(start_vnode_id);
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
                int next = bloom->next_id;
                if(next == -1) {
                    return res;
                }
                BloomFilter* nb = valueList->getBloom(next);
                if(!nb) {
                    return res;
                }
                // 缩减闭包：只做路由判断
                Key_t next_min = read_consistent(nb->version, [&]() {
                    return nb->getMinKey();
                });
                res.can_move = (key >= next_min);
                res.next_id = next;
                if(!res.can_move) {
                    // 不在闭包里做 TLS 与 vnode 查找
                    res.out = -1;
                }
                return res;
            });

            if(!s.can_move) {
                if(s.out != (Val_t)-1) return s.out; // 早期保守
                // 闭包未执行 vnode 查找，这里补做
                Vnode* vnode = valueList->pmemVnodePool->at(current_vnode_id);
                if(!vnode) return -1;
                Val_t tmp;
                if(vnode->lookupWithoutFilter(key, tmp, bloom)) {
#if ENABLE_THREAD_KEY_CACHE
                    g_threadKeyCache.put(key, tmp);
#endif
                    return tmp;
                }
                return -1;
            }

            BloomFilter* next_bloom = valueList->getBloom(s.next_id);
            bloom = next_bloom;
            current_vnode_id = s.next_id;
        } 
    }
}

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
        long vnode_count = valueList->pmemVnodePool->getCurrentIdx() + 1;
        double dram_search_efficiency = mainIndex->calculateSearchEfficiency(vnode_count);
        lmt.logMergeOperation(dram_search_efficiency, vnode_count);
    }
    // 退出前做一次最终 drain
    long vnode_count = valueList->pmemVnodePool->getCurrentIdx() + 1;
    double dram_search_efficiency = mainIndex->calculateSearchEfficiency(vnode_count);
    lmt.logMergeOperation(dram_search_efficiency, vnode_count);
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
        inode->onRebalanceComplete();
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
        bool is_sgp = false;
        Vnode *target_vnode = nullptr;
        std::vector<Inode *> updates;
        updates.reserve(MAX_LEVEL);

        int current_level = mainIndex->getLevel();
        if (current_level <= 0) return false;

        Inode *header = mainIndex->getHeader(current_level - 1);
        if (!header) return false;

        // 新：lookup 时当场捕获叶子的短快照
        InodeSnapShort snap{};
        Inode *parent_inode = mainIndex->lookupForInsertWithSnap(key, header, current_level - 1, idx, is_sgp, updates, snap);
        
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
        BloomFilter *target_vnode_bloom = valueList->getBloom(target_vnode->getId());

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
        if (!updateParentInodeAfterSplit(parent_inode, new_vnode, updates, last_idx_mut, idx, is_sgp)) {
            std::cerr << "Failed to update the parent inode after split." << std::endl;
            return false;
        }
        return true;
    } // for retry
}

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

        int vnode_id ;
        if(snap.sgp_key != 0) {
            vnode_id = snap.sgp_value;
        } else {
            vnode_id = snap.gp_value;
        }
        const int start_vnode_id = vnode_id;

        BloomFilter *bloom = valueList->getBloom(start_vnode_id);
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
                BloomFilter* nb = valueList->getBloom(next); // 新增：右节点的版本源
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

            BloomFilter* next_bloom = valueList->getBloom(s.next_id);
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

        int vnode_id ;
        if(snap.sgp_key != 0) {
            vnode_id = snap.sgp_value;
        } else {
            vnode_id = snap.gp_value;
        }
        const int start_vnode_id = vnode_id;

        BloomFilter *bloom = valueList->getBloom(start_vnode_id);
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
                BloomFilter* nb = valueList->getBloom(next_id); // 新增：右节点的版本源
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

                BloomFilter* next_bloom = valueList->getBloom(s.next_id);
                bloom = next_bloom;
                current_vnode_id = s.next_id;
                continue;
            }

            BloomFilter* next_bloom = valueList->getBloom(s.next_id);
            current_vnode_id = s.next_id;
            bloom = next_bloom;
        }
    }
}

void TandemIndex::mergeScanResultsVec(std::vector<Key_t> &src, std::vector<Key_t> &dest)
{
    for(auto key : src) {
        if(key == (Key_t)-1) {
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
        if(key == (Key_t)-1) {
            continue;
        }
        dest.push(key);
    }
}

struct AnchorParams {
  double inserts_per_anchor = 10.0; // how many future inserts justify one SGP
  int    max_per_node       = 8;    // cap [remaining empty slots in SGP array]
  size_t future_epochs      = 1;    // forecast horizon
  size_t window_buckets     = 30;   // hot-region width for GetHottestRegion
};

// tiny helper for pretty-printing vectors
/*
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
*/
/*
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
    std::cout << "[SGP] covering inodes at level " << L << ": " << nodes.size() << "\n";

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
        int m = std::lround(pred / P.inserts_per_anchor);
        if (m <= 0) continue;
        m = std::min(m, P.max_per_node);


        //place anchors by density inside [S,E]
        auto anchors = tracker_->quantileAnchorsInWindow(B, C, S, E, (size_t)m);
        anchors.erase( std::remove_if(anchors.begin(), anchors.end(),
            [&](uint64_t k){
            return k <= S || k >= E;}),
            anchors.end());
        std::cout << "  [SGP] inode " << inode->getId()
                  << " node_range=[" << nmin << "," << nmax << "]"
                  << " slice=[" << S << "," << E << ")"
                  << " pred≈" << pred << " -> anchors=" << anchors.size()
                  << " keys=" << join_u64(anchors) << "\n";

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
}*/

void TandemIndex::printStatus()
{
#if ENABLE_PMEM_STATS
    std::unique_lock<std::shared_mutex> lock(pmemRecoveryArray->stats_mtx);
#endif
    long vnode_count = valueList->pmemVnodePool->getCurrentIdx() + 1;
    mainIndex->calculateSearchEfficiency(vnode_count);

    const size_t inode_size = sizeof(Inode);
    const size_t bloom_size = sizeof(BloomFilter);

    const size_t dram_inode_used_nodes = static_cast<size_t>(dramInodePool->getCurrentIdx());
    const size_t dram_inode_alloc_nodes = static_cast<size_t>(dramInodePool->getPoolSize());
    const size_t pmem_inode_used_nodes = static_cast<size_t>(pmemRecoveryArray->getCurrentIdx());
    const size_t pmem_inode_alloc_nodes = static_cast<size_t>(MAX_NODES);

    const size_t bloom_used_nodes = static_cast<size_t>(vnode_count);
    // const size_t bloom_dram_alloc_nodes = valueList->getAllocatedBloomCount();
    const size_t bloom_pmem_alloc_nodes = static_cast<size_t>(MAX_VALUE_NODES);

    std::cout << "[MemoryUsage] inode.dram.used=" << format_bytes(dram_inode_used_nodes * inode_size)
              << ", inode.dram.alloc=" << format_bytes(dram_inode_alloc_nodes * inode_size)
              << ", inode.pmem.used=" << format_bytes(pmem_inode_used_nodes * inode_size)
              << ", inode.pmem.alloc=" << format_bytes(pmem_inode_alloc_nodes * inode_size)
              << std::endl;

    std::cout << "[MemoryUsage] bloom.used=" << format_bytes(bloom_used_nodes * bloom_size)
              << ", bloom.dram.alloc=" << format_bytes(valueList->getAllocatedBloomBytes())
              << ", bloom.pmem.alloc=" << format_bytes(bloom_pmem_alloc_nodes * bloom_size)
              << std::endl;
}

#if 0
void TandemIndex::remove(int key)
{
    mainIndex->remove(key);
}
#endif

/*void TandemIndex::maybeActivateHotRegionOld() {
    constexpr size_t WINDOW = 20;
    constexpr size_t FUTURE_EPOCHS = 1;

    tl::Region hot{};
    if (!tracker_->GetHottestRegion(WINDOW, &hot))
        return;

    std::vector<uint64_t> B;
    std::vector<size_t>   C;
    if (!tracker_->GetLastEpochHistogram(B, C))
        return;

    int L = 0;
    auto inodes = mainIndex->nodesCoveringRangeAtLevel(hot.start, hot.end, L);
    if (inodes.empty()) {
        tracker_->DropLastEpochHistogram();
        return;
    }

    struct IntervalCandidate {
        int gp;
        uint64_t lo;
        uint64_t hi;
        double pred;
    };

    for (Inode* inode : inodes) {
        //if(getFromRebalanceQueue(inode)){
        //    //std::cout << "  [SGP] inode " << inode->getId() << "queued for rebalance; skip\n";
        //    continue;
        //}

        if (!inode || inode->isFull() || inode->isSGPFull()){
            addToRebalanceQueue(inode);
            continue;
        }
            
        int last_gp = inode->hdr.last_index;
        if (last_gp < 0) continue;

        double inode_pressure = 0.0;
        std::vector<IntervalCandidate> hot_intervals;

        for (int i = 0; i < last_gp; ++i) {
            std::vector<IntervalCandidate> hot_intervals;
            double inode_pressure = 0.0;
            uint64_t lo = inode->gps[i].key;
            uint64_t hi = inode->gps[i + 1].key;

            // intersect with hot region
            uint64_t S = std::max(lo, hot.start);
            uint64_t E = std::min(hi, hot.end);
            if (S >= E) continue;

            double pred = 0.0;
            if (!tracker_->GetNumInsertsInKeyRangeForNumFutureEpochs(
                    S, E, FUTURE_EPOCHS, &pred))
                continue;

            // predict stability violation
            double coeff = SEARCH_STABILITY_COEFFICIENT_BY_LEVEL[inode->hdr.level];
            double future_nodes = inode->gps[i].covered_nodes + pred;

            if (future_nodes <= coeff)
                continue;

            int m = std::min(
                int(std::ceil(pred / AnchorParams{}.inserts_per_anchor)),
                AnchorParams{}.max_per_node
            );
            if (m <= 0) continue;

            auto anchors =
                tracker_->placeAnchorsInsideInterval(B, C, lo, hi, m);

            if (anchors.empty()) continue;
#if 0
            std::cout << "  [SGP] HOT inode " << inode->getId()
                      << " GP[" << i << "]"
                      << " interval=[" << lo << "," << hi << ")"
                      << " pred≈" << pred
                      << " anchors=" << anchors.size()
                      << " keys=" << join_u64(anchors) << "\n";
#endif
            //if(getFromRebalanceQueue(inode)){
            //std::cout << "  [SGP] inode " << inode->getId() << "queued for rebalance; skip\n";
            //continue;}
#if 0
            for (uint64_t k : anchors) {
                if (!inode->isSGPFull()){ 
                    inode->activateSGP(k);
                } else {
                    break;
                }
            }
#endif
        }
    }

    tracker_->DropLastEpochHistogram();
}*/

void TandemIndex::maybeActivateHotRegion() {
    constexpr size_t WINDOW        = 20;
    constexpr size_t FUTURE_EPOCHS = 1;

    /*
    auto dump_inode_sgp_state = [&](Inode* inode) {
    std::ostringstream oss;
    oss << "  [SGP-STATE] inode " << inode->getId()
        << " L=" << inode->hdr.level
        << " GP=" << (inode->hdr.last_index + 1)
        << " SGP=" << inode->hdr.last_sgp
        << " | GPs=[";

    for (int i = 0; i <= inode->hdr.last_index; ++i) {
        oss << inode->gps[i].key;
        if (i < inode->hdr.last_index) oss << ",";
    }

    oss << "] SGPs=[";

    for (int i = 0; i < inode->hdr.last_sgp; ++i) {
        oss << inode->sgps[i].key;
        if (i + 1 < inode->hdr.last_sgp) oss << ",";
    }

    oss << "]";
    std::cout << oss.str() << "\n";
};
*/

    // local guards (no AnchorParams changes)
    constexpr double MIN_INODE_PRED = 0.05;  // suppress tiny Zipf noise
    constexpr int    MAX_ANCHORS_PER_INODE = 3;

    tl::Region hot{};
    if (!tracker_->GetHottestRegion(WINDOW, &hot))
        return;

    std::vector<uint64_t> B;
    std::vector<size_t>   C;
    if (!tracker_->GetLastEpochHistogram(B, C))
        return;

    int L = 0;
    auto inodes = mainIndex->nodesCoveringRangeAtLevel(hot.start, hot.end, L);
    if (inodes.empty()) {
        tracker_->DropLastEpochHistogram();
        return;
    }

    for (Inode* inode : inodes) {
        if (!inode)
            continue;

        if (inode->isFull() || inode->isSGPFull()) {
            addToRebalanceQueue(inode);
            continue;
        }

        uint64_t version_snapshot = inode->structure_version.load(std::memory_order_acquire);

        int last_gp = inode->hdr.last_index;
        if (last_gp < 0)
            continue;

        double inode_pred = 0.0;
        std::vector<uint64_t> inode_anchors;

        // ---- PHASE 1: scan GP intervals & COLLECT anchors ----
        for (int i = 0; i < last_gp; ++i) {
            uint64_t lo = inode->gps[i].key;
            uint64_t hi = inode->gps[i + 1].key;

            uint64_t S = std::max(lo, hot.start);
            uint64_t E = std::min(hi, hot.end);
            if (S >= E)
                continue;

            double pred = 0.0;
            if (!tracker_->GetNumInsertsInKeyRangeForNumFutureEpochs(
                    S, E, FUTURE_EPOCHS, &pred))
                continue;

            double coeff =
                SEARCH_STABILITY_COEFFICIENT_BY_LEVEL[inode->hdr.level];
            double future_nodes = inode->gps[i].covered_nodes + pred;

            if (future_nodes <= coeff)
                continue;

            inode_pred += pred;

            int m = std::min(
                int(std::ceil(pred / AnchorParams{}.inserts_per_anchor)),
                AnchorParams{}.max_per_node
            );
            if (m <= 0)
                continue;

            auto anchors =
                tracker_->placeAnchorsInsideInterval(B, C, lo, hi, m);

            for (uint64_t k : anchors) {
                if ((int)inode_anchors.size() >= MAX_ANCHORS_PER_INODE)
                    break;
                inode_anchors.push_back(k);
            }
        }

        // ---- PHASE 2: inode-level decision ----
        if (inode_pred < MIN_INODE_PRED)
            continue;

        if (inode_anchors.empty())
            continue;

#if 0
        std::cout << "  [SGP] HOT inode " << inode->getId()
                  << " anchors=" << inode_anchors.size()
                  << " keys=" << join_u64(inode_anchors)
                  << " pred≈" << inode_pred << "\n";
#endif

        // ---- PHASE 3: activate SGPs ONCE ----
        if (inode->structure_version.load(std::memory_order_acquire) != version_snapshot) {
        std::cout << "  [SGP] skipped \n";
            continue;}

        for (uint64_t k : inode_anchors) {
            if (inode->isSGPFull())
                break;
            //std::cout << "  [SGP] activating" << k << "\n";
            inode->activateSGP(k);

        }
        //dump_inode_sgp_state(inode);
    }

    tracker_->DropLastEpochHistogram();
}