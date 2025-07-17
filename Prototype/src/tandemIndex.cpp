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

#define LOG_SIZE 1UL*1024UL*1024UL*1024UL

TandemIndex::TandemIndex() {
    g_endTandem = false;
    valueList = new ValueList();
    pmemRecoveryArray = new PmemInodePool(sizeof(Inode), MAX_NODES);
    recoveryManager = new RecoveryManager(pmemRecoveryArray); 
    int level = recoveryManager->recoveryOperation();
    dramInodePool = recoveryManager->getDramInodePool();
    ckptLog = new CkptLog(LOG_SIZE);
    PmemManager::flushToNVM(3, reinterpret_cast<char *>(ckptLog), sizeof(ckptLog));
    mainIndex = new DramSkiplist(ckptLog, dramInodePool, valueList);
    mainIndex->setLevel(level);
    createLogMergeThread();
    createRebalanceThread();
    Inode *index_header = mainIndex->getHeader();
    Vnode *value_header = valueList->getHeader();
    index_header->gps[0].value = value_header->getId();
    dram_log_entry_t *header_entry = new dram_log_entry_t(index_header->getId(), index_header->hdr.coveredNodes,index_header->hdr.last_index,index_header->hdr.next, index_header->hdr.level);
    header_entry->setKeyVal(0, index_header->gps[0].key, index_header->gps[0].value);
    ckptLog->enq(header_entry);
    
    // 初始化rebalanceThread数组
    for(int i = 0; i < MAX_REBALANCE_THREADS; i++) {
        rebalanceThread[i] = nullptr;
    }
    
    insert(0,1);
}

TandemIndex::~TandemIndex() {
   g_endTandem = true; 
   
   // 等待并清理 rebalanceThread 数组
   for(int i = 0; i < MAX_REBALANCE_THREADS; i++) {
       if(rebalanceThread[i] != nullptr) {
           if(rebalanceThread[i]->joinable()) {
               rebalanceThread[i]->join();
           }
           delete rebalanceThread[i];
           rebalanceThread[i] = nullptr;
       }
   }
   
   if (logMergeThread->joinable()) {
       logMergeThread->join();
       delete logMergeThread;
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
    int vnode_id = target_inode->gps[idx].value;
    target_vnode = valueList->pmemVnodePool->at(vnode_id);
    if(target_vnode == nullptr) {
        std::cout << "Failed to get the vnode from the pmemVnodePool." << std::endl;
        return false;
    }
    BloomFilter *bloom = &valueList->bf[target_vnode->getId()];
    std::unique_lock<std::shared_mutex> vnode_lock(bloom->vnode_mtx);
    header_lock.unlock();

    //target_vnode: start of the target vnode chain. 
    //vnode_lock: lock for the target vnode's bloom filter, will be changed inside
    ret = insertInVnodeChain(target_vnode, bloom, vnode_lock, key, value, target_inode, idx);
    if(!ret) {
        // ret == false ==> vnode is full, need to split
        Vnode *newVnode = nullptr;
#ifdef DBG
        {
            std::lock_guard<std::mutex> lock(printMutex);
            cout << "Vnode is full, need to split. Current target_vnode: " << target_vnode->hdr.id << " key: " << key << " tid: " << syscall(SYS_gettid) << endl;
        }
#endif
        ret = handleNodeFullAndSplit(target_vnode, bloom, vnode_lock, key, value, target_inode, newVnode);
        if(ret) {
            // update the parent inode after split
            ret = updateParentInodeAfterSplit(target_inode, newVnode, updates);
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
        return false;
    }
    target_vnode = newVnode;    
    vnode_lock.unlock();
    return true;
}

bool TandemIndex::insertInVnodeChain(Vnode* &vnode, BloomFilter* &bloom, std::unique_lock<std::shared_mutex> &vnode_lock, Key_t key, Val_t value, Inode* &parent_inode, int idx)
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

bool TandemIndex::moveToNextVnodeForInsert(Vnode *&vnode, BloomFilter *&bloom, std::unique_lock<std::shared_mutex> &vnode_lock)
{
     if (!vnode || vnode->hdr.next == -1) {
        return false;
    }
    
    int next_id = vnode->hdr.next;
    Vnode* next_vnode = valueList->pmemVnodePool->at(next_id);
    if (!next_vnode) {
        return false;
    }
    
    BloomFilter* next_bloom = &valueList->bf[next_id];
    
    try {
        // 插入操作需要获取独占锁
        std::unique_lock<std::shared_mutex> next_lock(next_bloom->vnode_mtx);
        vnode_lock.unlock();
        
        vnode = next_vnode;
        bloom = next_bloom;
        vnode_lock = std::move(next_lock);
        
        return true;
    } catch (...) {
        return false;
    }
}

bool TandemIndex::handleNodeFullAndSplit(Vnode* &vnode, BloomFilter* &bloom, 
                                         std::unique_lock<std::shared_mutex> &vnode_lock, 
                                         Key_t key, Val_t value, Inode* &parent_inode, Vnode* &next_node)
{
    //vnode_lock: obtained, lock of vnode.
     //get new vnode from the pool
    Vnode* nextVnode = valueList->pmemVnodePool->getNextNode();
    if (!nextVnode) {
        return false;
    }
    BloomFilter* target_bloom = &valueList->bf[nextVnode->getId()];
    Key_t targetKey;
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
        targetKey = nextVnode->getMinKey();
    } else {
    //dont need to lock the targetVnode because its a new node and the predecessor is locked
        insert_success = nextVnode->insert(key, value, target_bloom);
        targetKey = nextVnode->getMinKey();
        if (!insert_success) {
            return false;
        }
    }
    next_node = nextVnode;
    vnode_lock.unlock(); // 释放当前vnode的锁
    return true;
}

bool TandemIndex::updateParentInodeAfterSplit(Inode *inode, Vnode *targetVnode, std::vector<Inode *> &updates)
{
    const int inode_id = inode->getId();
    std::unique_lock<std::shared_mutex> inode_wlock(mainIndex->inode_locks[inode_id]);

    BloomFilter* target_bloom = &valueList->bf[targetVnode->getId()];
    std::shared_lock<std::shared_mutex> target_lock(target_bloom->vnode_mtx);

    Key_t targetKey = targetVnode->getMinKey();

    
    // 更新覆盖节点数
    inode->hdr.coveredNodes++;
    
    // 检查是否需要激活GP
    if (!inode->checkForActivateGP()) {
        return true; // 不需要激活，插入完成
    }
    
    int pos = -1;
    if (inode->activateGP(targetKey, pos)) {
        // 成功激活GP
        //bool link_success = mainIndex->linkVnodeToInode(*inode, pos, *targetVnode);
        bool link_success = inode->insertAtPos(targetKey, targetVnode->getId(), pos);
        if (link_success) {
            //inode->gps[pos].key = targetKey;
            target_lock.unlock(); // 释放targetVnode的锁
            dram_log_entry_t *entry = new dram_log_entry_t(inode->getId(), inode->hdr.coveredNodes, inode->hdr.last_index, inode->hdr.next, inode->hdr.level);
            for (int i = 0; i <= inode->hdr.last_index; i++) {
                entry->setKeyVal(i, inode->gps[i].key, inode->gps[i].value);
            }
            entry->setCoveredNodes(inode->hdr.coveredNodes);
            entry->setLastIndex(inode->hdr.last_index);
            ckptLog->enq(entry);
        }
        return link_success;
    } else {
        // 获取target_inode的父节点
        // 记录从根到目标节点的路径上所有父子关系
        for (size_t i = 1; i < updates.size(); ++i) {
            mainIndex->recordInodeRelation(updates[i], updates[i-1]);
        }
        // 将需要重平衡的节点及其父节点信息添加到重平衡任务中
        assert(inode->hdr.last_index ==13);
        addToRebalanceQueue(inode);
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
    while(!g_endTandem) {
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
    while(!g_endTandem) {
        Inode* inode = nullptr;
        
        // 尝试从队列中获取重平衡任务，并将其标记为正在处理
        if(getFromRebalanceQueue(inode)) {
            // 获取重平衡锁
            std::unique_lock<std::shared_mutex> rebalance_lock(mainIndex->rebalance_lock);
            
            Inode* parent_inode = mainIndex->getParentInode(inode);
            if(parent_inode != nullptr) {
                assert(parent_inode->hdr.last_index >= 0);
            }
            assert(inode->hdr.last_index >= 0);
            int ret = mainIndex->fastRebalance(inode, parent_inode);
            if(ret == 2) {
                assert(parent_inode->hdr.last_index == 13);
                addToRebalanceQueue(parent_inode); // 如果需要重平衡，重新加入队列
            }
            
            // TODO: 实现具体的 Inode 重平衡逻辑
            // int ret = mainIndex->rebalanceInode(*inode);
            rebalance_lock.unlock();

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
        inode->hdr.coveredNodes++;
        if(inode->checkForActivateGP()) {
            //activate GP
            int pos = -1;
            {
                //std::shared_lock<std::shared_mutex> lock_target(targetVnode->hdr.mtx);
                BloomFilter *target_bloom = &valueList->bf[targetVnode->getId()];
                std::shared_lock<std::shared_mutex> lock_target(target_bloom->vnode_mtx);
                targetKey = targetVnode->getMinKey();
            }
            if(inode->activateGP(targetKey, pos)) {
                //bool ret = mainIndex->linkVnodeToInode(*inode, pos, *targetVnode);
                bool ret = inode->insertAtPos(targetKey, targetVnode->getId(), pos);
                if(!ret) {
                    std::cout << "Failed to link the vnode to the inode." << std::endl;
                    return;
                }
                //inode->gps[pos].key = targetKey;
                dram_log_entry_t *entry = new dram_log_entry_t(inode->getId(),inode->hdr.coveredNodes, inode->hdr.last_index, inode->hdr.next, inode->hdr.level);
                for(int i = 0; i <= inode->hdr.last_index; i++) {
                    entry->setKeyVal(i, inode->gps[i].key, inode->gps[i].value);
                }
                entry->setCoveredNodes(inode->hdr.coveredNodes);
                entry->setLastIndex(inode->hdr.last_index);
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



#if 0
bool TandemIndex::insertWithoutIndex(Key_t key, Val_t value)
{
    // 与lookup中处理null情况的逻辑保持一致
    Vnode *headVnode = valueList->getHeader();
    if (!headVnode) {
        return false;
    }
    
    BloomFilter *head_bloom = &valueList->bf[headVnode->getId()];
    std::shared_lock<std::shared_mutex> head_lock(head_bloom->vnode_mtx);
    
    Vnode *targetVnode = nullptr;
    
    // 检查第一个实际节点
    if (headVnode->hdr.next != -1) {
        targetVnode = valueList->pmemVnodePool->at(headVnode->hdr.next);
        if (targetVnode && !targetVnode->isFull()) {
            BloomFilter* target_bloom = &valueList->bf[targetVnode->getId()];
            std::unique_lock<std::shared_mutex> target_lock(target_bloom->vnode_mtx);
            
            Key_t old_min_key = targetVnode->getMinKey();
            
            if (targetVnode->insert(key, value, target_bloom)) {
                // 如果改变了最小键，更新主索引
                Key_t new_min_key = targetVnode->getMinKey();
                if (new_min_key != old_min_key) {
                    return mainIndex->update(old_min_key, new_min_key, value);
                }
                return true;
            }
        }
    }
}



void TandemIndex::remove(int key)
{
    mainIndex->remove(key);
}



void TandemIndex::print()
{
    mainIndex->print();
}
#endif

