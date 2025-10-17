#include <queue>
#include <vector>
#include "tandemIndex.h"
#include "valuelist.h"
#include "spinLock.h"
#include "workerThread.h"
#include "checkpoint.h"
#include "common.h"
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

std::shared_ptr<tl::InsertTracker> tracker_;
struct InsertForecastingOptions {
    bool use_insert_forecasting = true;
    size_t num_inserts_per_epoch = 10000; // The number of inserts in each InsertTracker epoch; the total elements of the equi-depth histogram used for insert forecasting.
    size_t num_partitions = 10; // The number of bins in the insert forecasitng histogram.
    size_t sample_size = 1000; // The size of the reservoir sample based on which the partition boundaries are set at the beginning of each epoch.
    size_t random_seed = 42; // The random seed to be used by the insert tracker.
    double overestimation_factor = 1.5; // Estimated ratio of (number of records in reorg range) / (number of records that fit in base pages in reorg range).
    size_t num_future_epochs = 1; // During reorganization, the system will leave sufficient space to accommodate forecasted inserts for the next `num_future_epochs` epochs.
};

#define LOG_SIZE 3UL*1024UL*1024UL*1024UL

TandemIndex::TandemIndex() {
    g_endTandem.store(false,std::memory_order_relaxed);
    valueList = new ValueList();
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
    dram_log_entry_t *header_entry = new dram_log_entry_t(index_header->getId(), index_header->hdr.last_index,index_header->hdr.next, index_header->hdr.level, index_header->hdr.parent_id);
    header_entry->setKeyVal(0, index_header->gps[0].key, index_header->gps[0].value, 1);
    ckptLog->enq(header_entry);
    
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
    } else {
        tracker_.reset(); // or leave null
    }

    insert(0,1); 
}

TandemIndex::~TandemIndex() {
    // 1. 首先设置结束标志
    g_endTandem.store(true, std::memory_order_relaxed);
    
    // 2. 等待所有重平衡线程完全结束
    for(int i = 0; i < MAX_REBALANCE_THREADS; i++) {
        if(rebalanceThread[i] != nullptr) {
            if(rebalanceThread[i]->joinable()) {
                rebalanceThread[i]->join();
            }
            delete rebalanceThread[i];
            rebalanceThread[i] = nullptr;
        }
    }
    
    // 3. 在所有线程结束后，显式清理共享资源
    {
        std::lock_guard<std::mutex> lock(rebalanceQueueMutex);
        rebalanceQueue = std::queue<Inode *>(); // 清空队列
        rebalancingInodes.clear();              // 清空集合
        nodesInRebalanceProcess.clear();        // 清空集合
    }

    if(logFlushThread && logFlushThread->joinable()) {
        logFlushThread->join();
        delete logFlushThread;
        logFlushThread = nullptr;
    }
    
    if (logMergeThread && logMergeThread->joinable()) {
        logMergeThread->join();
        delete logMergeThread;
        logMergeThread = nullptr;
    }
    
    Inode *superNode = pmemRecoveryArray->at(MAX_NODES - 1);
    if(superNode != nullptr) {
         superNode->hdr.next = dramInodePool->getCurrentIdx();
         superNode->hdr.level = mainIndex->getLevel();
         PmemManager::flushToNVM(1, reinterpret_cast<char *>(superNode), sizeof(Inode));
    } 

    Vnode *metaVnode = valueList->pmemVnodePool->at(MAX_VALUE_NODES - 1);
    if(metaVnode != nullptr) {
        metaVnode->hdr.next = valueList->pmemVnodePool->getCurrentIdx();
        PmemManager::flushToNVM(0, reinterpret_cast<char *>(metaVnode), sizeof(Vnode));
    }
    cout << "vnode count: " << valueList->pmemVnodePool->getCurrentIdx() << endl;
    mainIndex->printStats();

    tracker_.reset();
}

bool TandemIndex::insert(Key_t key, Val_t value)
{
    tracker_->Add(key); //sampling
    maybeActivateHotRegion(); //TODO [discard the histogram after this]
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
    header_lock.unlock();
    
    int vnode_id = target_inode->gps[idx].value;
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

bool TandemIndex::insertWithNewInodes(Key_t key, Val_t value, Vnode *&target_vnode)
{
    Vnode* headerVnode = valueList->getHeader();
    BloomFilter* headerbloom = &valueList->bf[headerVnode->getId()];
    std::unique_lock<std::shared_mutex> vheader_lock(headerbloom->vnode_mtx);
      // create new vnode
    Vnode* newVnode = valueList->pmemVnodePool->getNextNode();
    if (!newVnode) {
        std::cerr << "Failed to get new vnode from pool" << std::endl;
        return false;
    }
    //lock state: headerVnode: yes, newVnode: no
    bool ret = valueList->append(headerVnode, newVnode);

    // 在新vnode中插入键值对
    BloomFilter* bloom = &valueList->bf[newVnode->getId()];
    std::unique_lock<std::shared_mutex> vnode_lock(bloom->vnode_mtx);
    write_start(bloom->version);

    // release header vnode lock, because new vnode is ready to be used
    vheader_lock.unlock();
    
    if (!newVnode->insert(key, value, bloom)) {
        std::cerr << "Failed to insert into new vnode" << std::endl;
        write_end(bloom->version);
        return false;
    }
    write_end(bloom->version);
    target_vnode = newVnode;
    return true;    
}

bool TandemIndex::insertInVnodeChain(Vnode* &vnode, BloomFilter* &bloom, Key_t key, Val_t value)
{
   for (;;) {
        // ------------ 1) lock-free forward traversal ------------
        for (;;) {
            struct Step {
                bool can_move{false};
                int  next_id{-1};
            };

            Step s = read_consistent(bloom->version, [&]() -> Step {
                Step r;
                int nid = vnode->hdr.next;            // read under vnode's version
                if (nid == -1) return r;

                Vnode* n = valueList->pmemVnodePool->at(nid);
                if (!n) return r;
                BloomFilter* nb = &valueList->bf[n->hdr.id]; // 新增：右节点的版本源
                // read next's min under next's version
                Key_t next_min = read_consistent(nb->version, [&]() {
                    return n->getMinKey();
                });

                r.can_move = (key >= next_min);
                r.next_id  = nid;
                return r;
            });

            if (!s.can_move) break;

            Vnode* next = valueList->pmemVnodePool->at(s.next_id);
            if (!next) break; // defensive
            __builtin_prefetch(&next->hdr, 0, 1);
            __builtin_prefetch(next->records, 0, 1);
            vnode = next;
            bloom = &valueList->bf[next->hdr.id];
        }

        // ------------ 2) lock & re-validate ------------
        // A concurrent split may have made our key belong to the next vnode.
        {
            std::unique_lock<std::shared_mutex> lock(bloom->vnode_mtx);
            // Re-check routing decision while holding this vnode's writer lock.
            int nid = vnode->hdr.next;
            if (nid != -1) {
                Vnode* n = valueList->pmemVnodePool->at(nid);
                if (n) {
                    BloomFilter* nb = &valueList->bf[n->hdr.id]; // 新增：右节点的版本源
                    Key_t next_min = read_consistent(nb->version, [&]() {
                        return n->getMinKey();
                    });

                    if (key >= next_min) {
                        // We should move right; drop the lock and loop.
                        vnode = n;
                        bloom = &valueList->bf[n->hdr.id];
                        continue; // go back to (1)
                    }
                }
            }

            // ------------ 3) do the insert under versioned write ------------
            write_start(bloom->version);
            bool ok = vnode->insert(key, value, bloom);
            write_end(bloom->version);

            return ok; // if false, caller will do split path
        }
    }
}

bool TandemIndex::handleNodeFullAndSplit(Vnode* &vnode, BloomFilter* &bloom,  
                                         Key_t key, Val_t value, Vnode* &next_node)
{
    std::unique_lock<std::shared_mutex> left_lock(bloom->vnode_mtx);
    Vnode* nextVnode = valueList->pmemVnodePool->getNextNode();
    if (!nextVnode) {
        return false;
    }

    valueList->split(vnode, nextVnode);
    // 3) Decide which node should receive (key,value)
    // Read nextVnode’s min under its own version to avoid torn reads.
    BloomFilter* next_bloom = &valueList->bf[nextVnode->getId()];
    Key_t next_min = read_consistent(next_bloom->version, [&](){
        return nextVnode->getMinKey();
    });

    bool ok = false;
    if (key < next_min) {
        write_start(bloom->version);
        ok = vnode->insert(key, value, bloom);
        write_end(bloom->version);
    } else {
        // Insert into RIGHT (new) vnode
        left_lock.unlock();
        BloomFilter* right_bloom = &valueList->bf[nextVnode->getId()];
        std::unique_lock<std::shared_mutex> right_lock(right_bloom->vnode_mtx);
        write_start(right_bloom->version);
        ok = nextVnode->insert(key, value, right_bloom);
        write_end(right_bloom->version);
        bloom = right_bloom;
    }
    if (!ok) return false;

    // 4) Publish the new node to the caller (parent update will happen outside)
    next_node = nextVnode;
    return true;
}

//parent_inode is the last level inode that contains the targetVnode
//bool updateParentInodeAfterSplit(Inode *parent_inode, Vnode *targetVnode, std::vector<Inode *> &updates, int &last_idx, int &idx_to_next_level)
bool TandemIndex::updateParentInodeAfterSplit(Inode *parent_inode, Vnode *targetVnode,
                                               std::vector<Inode *> &updates,
                                               int &last_idx, int &idx_to_next_level)
{
    std::unique_lock<std::shared_mutex> inode_lock(mainIndex->inode_locks[parent_inode->getId()]);
    
    if(parent_inode->hdr.last_index != last_idx) {
#if 0
        std::cout << "Inode last_index mismatch after split, likely a concurrent modification. id: "  << parent_inode->getId() << std::endl;
#endif
        return true; // 操作被抢占，直接返回，让上层逻辑重试
    }

    BloomFilter* target_bloom = &valueList->bf[targetVnode->getId()];
    //std::shared_lock<std::shared_mutex> target_lock(target_bloom->vnode_mtx);
    //Key_t targetKey = targetVnode->getMinKey();
    Key_t targetKey = read_consistent(target_bloom->version, [&](){
        return targetVnode->getMinKey();
    });

    // check if the gps[idx_to_next_level] 
    if (!parent_inode->checkForActivateNextGP(idx_to_next_level)) {
        // **逻辑正确**: 父节点足够平衡，不需激活新GP。
        parent_inode->gps[idx_to_next_level].covered_nodes++;
#if ENABLE_DELTA_LOG
        mainIndex->ckpt_log_single_slot_delta(ckptLog, parent_inode, static_cast<int16_t>(idx_to_next_level));
#endif
        
        //target_lock.unlock(); // 释放targetVnode的锁
        dram_log_entry_t *entry = new dram_log_entry_t(parent_inode->getId(), parent_inode->hdr.last_index, parent_inode->hdr.next, parent_inode->hdr.level, parent_inode->hdr.parent_id);
        for (int i = 0; i <= parent_inode->hdr.last_index; i++) {
            entry->setKeyVal(i, parent_inode->gps[i].key, parent_inode->gps[i].value, parent_inode->gps[i].covered_nodes);
        }
        ckptLog->enq(entry);
        return true;
    }
    
    int pos = -1;
    // **逻辑正确**: 父节点不平衡，需要激活一个新GP来指向新分裂出的Vnode。
    // 新GP只覆盖这一个Vnode，所以初始覆盖数是1。
    if (parent_inode->activateGPForVnode(targetKey, targetVnode->getId(), pos, 1)) {
        //target_lock.unlock(); // 释放targetVnode的锁
        dram_log_entry_t *entry = new dram_log_entry_t(parent_inode->getId(), parent_inode->hdr.last_index, parent_inode->hdr.next, parent_inode->hdr.level, parent_inode->hdr.parent_id);
        for (int i = 0; i <= parent_inode->hdr.last_index; i++) {
            entry->setKeyVal(i, parent_inode->gps[i].key, parent_inode->gps[i].value, parent_inode->gps[i].covered_nodes);
        }
        ckptLog->enq(entry);
        return true;
    } else {
        // **逻辑正确**: 父节点不平衡，且已经满了，无法激活新GP。
        // 必须对父节点自身进行重平衡。
        //[TODO] write barrier + lock?
        // 记录从根到目标节点的路径上所有父子关系，为重平衡提供父节点指针
        for (size_t i = 1; i < updates.size(); ++i) {
            //mainIndex->recordInodeRelation(updates[i], updates[i-1]);
            updates[i]->setParent(updates[i-1]->getId());

        }
        // 将需要重平衡的节点及其父节点信息添加到重平衡任务中
        assert(parent_inode->hdr.last_index == fanout / 2 - 1);
#if 0
        cout << " Parent inode needs rebalancing. id: " << parent_inode->getId()<< endl;
        
        for(int i = 0; i < parent_inode->hdr.last_index; i++) {
            cout << " gp " << i << " key: " << parent_inode->gps[i].key << " value: " << parent_inode->gps[i].value << " covered_nodes: " << parent_inode->gps[i].covered_nodes << endl;
        }
#endif
        addToRebalanceQueue(parent_inode);
        return true;
    }
}

Val_t TandemIndex::lookup(Key_t key)
{
    int idx = -1;
    Vnode *vnode = nullptr;

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
    Inode *target = mainIndex->lookup(key, header, current_level - 1, header_lock, idx);
    if(target == nullptr) {
        return -1;
    }
    
    // now header_lock is still held, as lock of target
    int vnode_id = target->gps[idx].value;
    header_lock.unlock();

    vnode = valueList->pmemVnodePool->at(vnode_id);
    if(vnode == nullptr) {
        return -1;
    }
    
    BloomFilter *bloom = &valueList->bf[vnode_id];
    // ----- 3) Lock-free traversal of vnode chain -----
    for (;;) {
        struct Snap {
            bool found{false};
            Val_t out{};
            bool can_move{false};
            int  next_id{-1};
        };

        Snap s = read_consistent(bloom->version, [&]() -> Snap {
            Snap res;

            // (a) If bloom says 'maybe', do an exact match under the same version snapshot
            bool might = bloom->mightContain(key);
            if (might) {
                Val_t tmp;
                if (vnode->lookupWithoutFilter(key, tmp, bloom)) {
                    res.found = true;
                    res.out = tmp;
                    return res;
                }
            }

            // (b) Decide whether we should move right
            int next = vnode->hdr.next;
            if (next != -1) {
                Key_t vmax = vnode->getMaxKey();         // safe under version snapshot
                res.can_move = (key > vmax) || !might;   // classic routing rule
                if (res.can_move) res.next_id = next;
            }
            return res;
        });

        if (s.found) return s.out;
        if (!s.can_move) return -1;

        // Hop to the next vnode (still no locks). We’ll re-evaluate under the next vnode’s version.
        Vnode *next = valueList->pmemVnodePool->at(s.next_id);
        if (!next) return -1;
        __builtin_prefetch(&next->hdr, 0, 1);
        __builtin_prefetch(next->records, 0, 1);
        vnode = next;
        bloom = &valueList->bf[next->hdr.id];
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


void TandemIndex::update(Key_t key, Val_t value)
{
#if 0
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
#endif
}

void TandemIndex::scan(Key_t key, size_t range, std::priority_queue<Key_t, std::vector<Key_t>, std::greater<Key_t>> &result)
{
#if 0
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
#endif
}

struct AnchorParams {
  double inserts_per_anchor = 64.0; // how many future inserts justify one SGP
  int    max_per_node       = 8;    // cap [remaining empty slots in SGP array]
  size_t future_epochs      = 1;    // forecast horizon
  size_t window_buckets     = 16;   // hot-region width for GetHottestRegion
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
    if (!tracker_) return;

    // Choose a window of buckets to represent the “region” (e.g., 16 buckets)
    tl::Region hot{};
    const size_t window = 16;
    if (!tracker_->GetHottestRegion(window, &hot)) {
        //std::cout << "[SGP] no completed epoch yet; skip activation\n";
        return;  // no completed epoch yet
    }
    //std::cout << "[SGP] hottest region = [" << hot.start << ", " << hot.end << ") (window=" << window << ")\n";

    // Forecast one future epoch (tweak if you want >1)
    double forecast = 0.0;
    if (!tracker_->GetNumInsertsInKeyRangeForNumFutureEpochs(
        hot.start, hot.end, /*num_future_epochs=*/1, &forecast)) {
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
        // TODO: check if no SGP slots - queue for rebalnce 
        // TODO: if node queued for rebalance - continue

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

        // activate SGPs at those anchor keys
        for(uint64_t key : anchors){
            // get lock 
            // node->activateSGP(n);  //TODO implement
        }
        
    } 
    // TODO - clear current epoch
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
