#include <queue>
#include <vector>
#include "tandemIndex.h"
#include "valuelist.h"
#include "spinLock.h"
#include "workerThread.h"
#include "checkpoint.h"
#include "common.h"
#include <sys/syscall.h>

//std::queue<std::vector<wq_entry *>*> g_workQueue[WORKERQUEUE_NUM];
//std::queue <wq_entry_t *> g_workQueue;
//std::vector<int> g_workQueue;
//std::queue<std::vector<ckp_entry *>*> g_checkpointQueue;
//boost::lockfree::spsc_queue<CheckpointVector*, boost::lockfree::capacity<1000000>> g_checkpointQueue;
//boost::lockfree::spsc_queue<ckp_entry *, boost::lockfree::capacity<1000000>> g_checkpointQueue;
std::queue<CheckpointVector *> g_checkpointQueue;
bool wqReady[WORKERQUEUE_NUM] = {false};
volatile bool wtInitialized = false;
volatile bool mgInitialized = false;
std::atomic<bool> g_endTandem;
SpinLock g_spinLock;

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
    dram_log_entry_t *header_entry = new dram_log_entry_t(index_header->getId(), index_header->hdr.last_index,index_header->hdr.next, index_header->hdr.level);
    header_entry->setKeyVal(0, index_header->gps[0].key, index_header->gps[0].value, 1);
    ckptLog->enq(header_entry);
    
    // 初始化rebalanceThread数组
    for(int i = 0; i < MAX_REBALANCE_THREADS; i++) {
        rebalanceThread[i] = nullptr;
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
         superNode->hdr.last_index = dramInodePool->getCurrentIdx();
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
}

bool TandemIndex::insert(Key_t key, Val_t value)
{
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
    
    int vnode_id = target_inode->gps[idx].value;
    target_vnode = valueList->pmemVnodePool->at(vnode_id);
    if(target_vnode == nullptr) {
        std::cout << "Failed to get the vnode from the pmemVnodePool." << std::endl;
        return false;
    }
    BloomFilter *bloom = &valueList->bf[target_vnode->getId()];
    std::unique_lock<std::shared_mutex> vnode_lock(bloom->vnode_mtx);

    int current_last_idx = target_inode->hdr.last_index;
    // **移除：不再需要获取旧的全局 coveredNodes**
    // int coveredNodes = target_inode->hdr.coveredNodes;

    header_lock.unlock();
    //target_vnode: start of the target vnode chain. 
    //vnode_lock: lock for the target vnode's bloom filter, will be changed inside
    ret = insertInVnodeChain(target_vnode, bloom, vnode_lock, key, value);
    if(!ret) {
        // ret == false ==> vnode is full, need to split
        Vnode *newVnode = nullptr;
#ifdef DBG
        {
            std::lock_guard<std::mutex> lock(printMutex);
            cout << "Vnode is full, need to split. Current target_vnode: " << target_vnode->hdr.id << " key: " << key << " tid: " << syscall(SYS_gettid) << endl;
        }
#endif
        ret = handleNodeFullAndSplit(target_vnode, bloom, vnode_lock, key, value, newVnode);
        if(ret) {
            // update the parent inode after split
            ret = updateParentInodeAfterSplit(target_inode, newVnode, updates, current_last_idx, idx);
            if(!ret) {
                std::cout << "Failed to update the parent inode after split." << std::endl;
                return false;
            }
        }else {
            std::cout << "Failed to handle node full and split." << std::endl;
            return false;
        }
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

    // release header vnode lock, because new vnode is ready to be used
    vheader_lock.unlock();
    
    if (!newVnode->insert(key, value, bloom)) {
        std::cerr << "Failed to insert into new vnode" << std::endl;
        ret = false;
        return ret;
    }
    target_vnode = newVnode;    
    vnode_lock.unlock();
    return ret;
}

bool TandemIndex::insertInVnodeChain(Vnode* &vnode, BloomFilter* &bloom, std::unique_lock<std::shared_mutex> &vnode_lock, Key_t key, Val_t value)
{
    // To verify the lock is held, you can use an assertion.
    // This will cause the program to terminate if the lock is not owned
    // by the unique_lock object, which is useful for debugging.
    while(true) {
#ifdef DBG
        {
            std::lock_guard<std::mutex> lock(printMutex);
            cout << " traverse vnode : " << vnode->hdr.id << " vnode->next: " << vnode->hdr.next << " vnode addr: " << vnode << " mutex: " << vnode_lock.mutex() <<" key " << key << " tid " <<syscall(SYS_gettid)<< endl;
        }
#endif
        if(vnode->hdr.next != -1 ) {
            Vnode *next_vnode = valueList->pmemVnodePool->at(vnode->hdr.next);
            BloomFilter *next_bloom = &valueList->bf[next_vnode->getId()];
            std::unique_lock<std::shared_mutex> next_lock(next_bloom->vnode_mtx);
            if(key > next_vnode->getMinKey()) {
                vnode_lock.unlock();
                vnode_lock = std::move(next_lock);
                vnode = next_vnode;
                bloom = next_bloom;
                continue;
            }else {
                break;
            }
        }else {
            break;
        }
    }
    // 尝试插入
    if(vnode->insert(key, value, bloom)) {
        return true; // 插入成功，直接返回
    }else {
#ifdef DBG
        {
            std::lock_guard<std::mutex> lock(printMutex);
            cout << "Vnode is full, need to split. Current vnode: " << vnode->hdr.id << " key: " << key << " tid: " << syscall(SYS_gettid) << endl;
        }
#endif
        return false;
    }
}

bool TandemIndex::handleNodeFullAndSplit(Vnode* &vnode, BloomFilter* &bloom, 
                                         std::unique_lock<std::shared_mutex> &vnode_lock, 
                                         Key_t key, Val_t value, Vnode* &next_node)
{
    //vnode_lock: obtained, lock of vnode.
    //get new vnode from the pool
    Vnode* nextVnode = valueList->pmemVnodePool->getNextNode();
    if (!nextVnode) {
        return false;
    }
    BloomFilter* target_bloom = &valueList->bf[nextVnode->getId()];
    //Key_t targetKey = 0;
    //split vnode, lock of vnode is still held, targetVnode is unseen
    assert(vnode_lock.owns_lock());
#ifdef DBG
    {
        std::lock_guard<std::mutex> lock(printMutex);
        cout << " Splitting vnode: " << vnode->hdr.id << " vnode addr: " << vnode << " mutex: " << vnode_lock.mutex()<< " key " << key << " tid "<< syscall(SYS_gettid) << endl;
    }
#endif
    valueList->split(vnode, nextVnode);
    // 插入操作 - 需要写权限
    bool insert_success = false;
    if (key <= nextVnode->getMinKey()) {
        // 在当前持有的锁保护下修改原vnode
        insert_success = vnode->insert(key, value, bloom);
        if (!insert_success) {
            return false;
        }
    //dont need to lock the targetVnode because its a new node and the predecessor is locked
        //targetKey = nextVnode->getMinKey();
    } else {
    //dont need to lock the targetVnode because its a new node and the predecessor is locked
        insert_success = nextVnode->insert(key, value, target_bloom);
        //targetKey = nextVnode->getMinKey();
        if (!insert_success) {
            return false;
        }
    }
    next_node = nextVnode;
    vnode_lock.unlock(); // 释放当前vnode的锁
    return true;
}

//parent_inode is the last level inode that contains the targetVnode
bool TandemIndex::updateParentInodeAfterSplit(Inode *parent_inode, Vnode *targetVnode, std::vector<Inode *> &updates, int &last_idx, int &idx_to_next_level)
{
    std::unique_lock<std::shared_mutex> inode_lock(mainIndex->inode_locks[parent_inode->getId()]);
    
    // 这个检查仍然非常重要，用于防止在分裂和更新父节点之间发生其他并发修改
    if(parent_inode->hdr.last_index != last_idx) {
#if 0
        std::cout << "Inode last_index mismatch after split, likely a concurrent modification. id: "  << parent_inode->getId() << std::endl;
#endif
        return true; // 操作被抢占，直接返回，让上层逻辑重试
    }

    BloomFilter* target_bloom = &valueList->bf[targetVnode->getId()];
    std::shared_lock<std::shared_mutex> target_lock(target_bloom->vnode_mtx);
    Key_t targetKey = targetVnode->getMinKey();
    
    // check if the gps[idx_to_next_level] 
    if (!parent_inode->checkForActivateNextGP(idx_to_next_level)) {
        // **逻辑正确**: 父节点足够平衡，不需激活新GP。
        parent_inode->gps[idx_to_next_level].covered_nodes++;
#if ENABLE_DELTA_LOG
        mainIndex->ckpt_log_single_slot_delta(ckptLog, parent_inode, static_cast<int16_t>(idx_to_next_level));
#endif
        
        target_lock.unlock(); // 释放targetVnode的锁
        dram_log_entry_t *entry = new dram_log_entry_t(parent_inode->getId(), parent_inode->hdr.last_index, parent_inode->hdr.next, parent_inode->hdr.level);
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
        target_lock.unlock(); // 释放targetVnode的锁
        dram_log_entry_t *entry = new dram_log_entry_t(parent_inode->getId(), parent_inode->hdr.last_index, parent_inode->hdr.next, parent_inode->hdr.level);
        for (int i = 0; i <= parent_inode->hdr.last_index; i++) {
            entry->setKeyVal(i, parent_inode->gps[i].key, parent_inode->gps[i].value, parent_inode->gps[i].covered_nodes);
        }
        ckptLog->enq(entry);
        return true;
    } else {
        // **逻辑正确**: 父节点不平衡，且已经满了，无法激活新GP。
        // 必须对父节点自身进行重平衡。
        
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
    Val_t value;
    
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
    std::shared_lock<std::shared_mutex> vnode_lock(bloom->vnode_mtx);
    // vnode链表遍历逻辑
    while(true) {
       //if bloom filter might contain the key, then lookup in the vnode 
        bool mightContain = bloom->mightContain(key);
        if(mightContain) {
            if(vnode->lookupWithoutFilter(key, value, bloom)) {
                return value;
            }
        }
        //if cant find the key, then decide whether to move to the next vnode
        //1. the next vnode exists
        //2. and key is larger than the max key of the current vnode or the bloom filter decide key not exist in the current vnode
        if(vnode->hdr.next != -1 && (key > vnode->getMaxKey() || !mightContain)) {
            if(!moveToNextVnode(vnode, bloom, vnode_lock)) {
                return -1; // no next vnode to move to
            }
            continue; // continue to check the next vnode
        }
        // if cant move to next vnode, also cant find the key, key does not exist in the index
        return -1;
    }
}

bool TandemIndex::moveToNextVnode(Vnode*& vnode, BloomFilter*& bloom, 
                                 std::shared_lock<std::shared_mutex>& current_lock)
{
    // 检查当前节点是否有下一个节点
    if(vnode->hdr.next == -1) {
        return false;
    }
    
    // 获取下一个节点的ID，避免重复访问
    int next_vnode_id = vnode->hdr.next;
    
    // 从持久化内存池中获取下一个vnode
    Vnode *next = valueList->pmemVnodePool->at(next_vnode_id);
    if(next == nullptr) {
        return false;
    }
    
    // 获取下一个节点的布隆过滤器
    BloomFilter *next_bloom = &valueList->bf[next->hdr.id];
    
    // 尝试获取下一个节点的锁
    std::shared_lock<std::shared_mutex> next_lock(next_bloom->vnode_mtx);
    // 安全地转移到下一个节点
    current_lock.unlock();
    vnode = next;
    bloom = next_bloom;
    current_lock = std::move(next_lock);
    return true;
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


