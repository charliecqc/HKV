#include <queue>
#include <vector>
#include "tandemIndex.h"
#include "valuelist.h"
#include "spinLock.h"
#include "workerThread.h"
#include "checkpoint.h"
#include "common.h"

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
    mainIndex = new DramSkiplist(ckptLog, dramInodePool,valueList);
    mainIndex->setLevel(level);
    createLogMergeThread();
    Inode *index_header = mainIndex->getHeader();
    Vnode *value_header = valueList->getHeader();
    index_header->gps[0].value = value_header->getId();
    dram_log_entry_t *header_entry = new dram_log_entry_t(index_header->getId(), index_header->hdr.coveredNodes,index_header->hdr.last_index,index_header->hdr.next);
    header_entry->setKeyVal(0, index_header->gps[0].key, index_header->gps[0].value);
    ckptLog->enq(header_entry);
}

TandemIndex::~TandemIndex() {
   g_endTandem = true; 
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
// handle the case when the inode already exists
bool TandemIndex::handleExistingInodeInsert(Inode *inode, Key_t key, Val_t value, 
                                          int idx, bool &needToRebalance, Vnode* &targetVnode)
{
    // initialize variables
    needToRebalance = false;
    targetVnode = nullptr;
    
    // 1. get the value node from the inode
    Vnode *valueNode = nullptr;
    {
        std::shared_lock<std::shared_mutex> inode_lock(mainIndex->inode_locks[inode->getId()]);
        valueNode = valueList->pmemVnodePool->at(inode->gps[idx].value);
    } // release the lock after getting the value node
    
    // 2. traverse the value node linked list
    while(true) {
        // get bloom filter for the current value node
        BloomFilter *bloom = &valueList->bf[valueNode->hdr.id];
        
        // use a shared lock for reading the bloom filter
        {
            std::unique_lock<std::shared_mutex> lock_value(bloom->mtx);
            
            // cache maxKey to avoid repeated calculations
            Key_t maxKey = valueNode->getMaxKey(bloom);
            
            if(key > maxKey && valueNode->hdr.next != -1) {
                //move to the next vnode
                Vnode *nextNode = valueList->pmemVnodePool->at(valueNode->hdr.next);
                lock_value.unlock(); // manually unlock before continuing
                valueNode = nextNode;
                continue;
            }
            
            // 尝试直接插入
            if(valueNode->insert(key, value, &valueList->bf[valueNode->getId()])) {
                return true; // return true if insert is successful
            }
            
            // node is full, need to split
            targetVnode = valueList->pmemVnodePool->getNextNode();
            if(!targetVnode) {
                return false;
            }
            
            // split operation
            valueList->split(valueNode, targetVnode);
            
            // decide where to insert the new key
            Key_t splitMaxKey = valueNode->getMaxKey(bloom);
            if(key <= splitMaxKey) {
                // insert into the original node
                if(!valueNode->insert(key, value, &valueList->bf[valueNode->getId()])) {
                    std::cout << "Failed to insert after split. key: " << key << std::endl;
                    return false;
                }
            } else {
                // insert into the new vnode, need to lock it
                BloomFilter *target_bloom = &valueList->bf[targetVnode->getId()];
                std::unique_lock<std::shared_mutex> lock_target(target_bloom->mtx);
                if(!targetVnode->insert(key, value, target_bloom)) {
                    std::cout << "Failed to insert into new vnode." << std::endl;
                    return false;
                }
            }
            break; // quit the loop after handling the split
        }
    }
    
    // 3. update the inode after split
    return updateInodeAfterSplit(inode, targetVnode, needToRebalance);
}

bool TandemIndex::updateInodeAfterSplit(Inode *inode, Vnode *targetVnode, bool &needToRebalance)
{
    std::unique_lock<std::shared_mutex> inode_wlock(mainIndex->inode_locks[inode->getId()]);
    inode->hdr.coveredNodes++;
    
    if(!inode->checkForActivateGP()) {
        return true; // no need to activate new GP
    }
    
    // obtain the minimum key from the target vnode
    Key_t targetKey;
    {
        BloomFilter *bloom = &valueList->bf[targetVnode->hdr.id];
        std::unique_lock<std::shared_mutex> lock_target(bloom->mtx);
        targetKey = targetVnode->getMinKey(bloom);
    } // release target vnode lock
    
    int pos = -1;
    if(inode->activateGP(targetKey, pos)) {
        // link the new vnode to the inode
        bool ret = mainIndex->linkVnodeToInode(*inode, pos, *targetVnode);
        if(!ret) {
            std::cout << "Failed to link vnode to inode." << std::endl;
            return false;
        }
        
        inode->gps[pos].key = targetKey;
        
        // create a new log entry for the checkpoint
        auto entry = std::make_unique<dram_log_entry_t>(
            inode->getId(), inode->hdr.coveredNodes, 
            inode->hdr.last_index, inode->hdr.next);
            
        for(int i = 0; i <= inode->hdr.last_index; i++) {
            entry->setKeyVal(i, inode->gps[i].key, inode->gps[i].value);
        }
        entry->setCoveredNodes(inode->hdr.coveredNodes);
        entry->setLastIndex(inode->hdr.last_index);
        
        ckptLog->enq(entry.release()); // move ownership to the log
        return true;
    } else {
        needToRebalance = true;
        return true;
    }
}

// process the case when a new inode is inserted
bool TandemIndex::handleNewInodeInsert(Key_t key, Val_t value)
{
    Vnode *headVnode = valueList->getHeader();
    Vnode *targetVnode = nullptr;
    
    BloomFilter *head_bloom = &valueList->bf[headVnode->getId()];
    std::unique_lock<std::shared_mutex> head_lock(head_bloom->mtx);
    
    // 1. try to insert into the first vnode
    if(headVnode->hdr.next != -1) {
        targetVnode = valueList->pmemVnodePool->at(headVnode->hdr.next);
        
        
        BloomFilter *bloom = &valueList->bf[targetVnode->getId()];
        std::unique_lock<std::shared_mutex> lock_target(bloom->mtx);
        
        if(!targetVnode->isFull()) {
            Key_t old_min_key = targetVnode->getMinKey(bloom);
            if(targetVnode->insert(key, value, bloom)) {
                // propgate the change to the main index
                return mainIndex->update(old_min_key, key, value);
            }
        }
    }
    
    // 2. create a new vnode if the first one is full
    targetVnode = valueList->pmemVnodePool->getNextNode();
    if(!targetVnode) {
        std::cout << "Failed to get new node from pool." << std::endl;
        return false;
    }
    
    //insert the key value into the new vnode
    bool ret = targetVnode->insert(key, value, &valueList->bf[targetVnode->getId()]);
    if(!ret) {
        std::cout << "Failed to insert into new vnode. Key: " << key << std::endl;
        return false;
    }
    
    // 3. insert the new vnode into the value list
    ret = valueList->append(headVnode, targetVnode);
    if(!ret) {
        std::cout << "Failed to append to value list." << std::endl;
        return false;
    }
    
    head_lock.unlock(); // release the lock on the head vnode after appending
    
    // 4. create a new inode and link it to the main index
    ret = mainIndex->insert(targetVnode);
    if(!ret) {
        std::cout << "Failed to insert into main index." << std::endl;
    }
    
    return ret;
}

bool TandemIndex::insert(Key_t key, Val_t value)
{
//0. find the start of the vnode chain in the value list
    int idx = -1;
    bool ret = false;
    bool needToRebalance = false;
    Inode *inode = mainIndex->lookup(key, idx);
    Vnode *targetVnode = nullptr; // new vnode to be inserted
    if(inode != nullptr) {
        ret = handleExistingInodeInsert(inode, key, value, idx, needToRebalance, targetVnode);
    }else {
        ret = handleNewInodeInsert(key, value);
    }

     if(needToRebalance && inode && targetVnode) {
        std::unique_lock<std::shared_mutex> rebalance_lock(mainIndex->rebalance_lock);
        ret = mainIndex->rebalanceInode(*inode, *targetVnode);
     }
    return ret;
}

Val_t TandemIndex::lookup(Key_t key)
{
    int idx = -1;
    Vnode *vnode = nullptr;
    Val_t value;
    Inode *inode = mainIndex->lookup(key, idx);
    if(inode == nullptr) {
        return -1;
    }
    
    // get the start of the vnode chain
    {
        std::shared_lock<std::shared_mutex> lock(mainIndex->inode_locks[inode->getId()]);
        vnode = valueList->pmemVnodePool->at(inode->gps[idx].value);
    }
    
    // get the bloom filter for the vnode
    BloomFilter *bloom = &valueList->bf[vnode->hdr.id];
    std::unique_lock<std::shared_mutex> current_lock(bloom->mtx);
    
    while(true) {
        bool mightContain = bloom->mightContain(key);
        
        if(vnode->hdr.next != -1 && !mightContain) {
            // hand over lock to the next vnode
            Vnode *next = valueList->pmemVnodePool->at(vnode->hdr.next);
            BloomFilter *next_bloom = &valueList->bf[next->hdr.id];
            std::unique_lock<std::shared_mutex> next_lock(next_bloom->mtx);
            
            // unlock the current vnode
            current_lock.unlock();
            vnode = next;
            bloom = next_bloom;
            current_lock = std::move(next_lock);
            continue;
        }
        
        if(mightContain) {
            if(vnode->lookupWithoutFilter(key, value, bloom)) {
                return value;
            } else {
                // false positive, check next vnode
                if (key > vnode->getMaxKey(bloom) && vnode->hdr.next != -1) {
                    Vnode *next = valueList->pmemVnodePool->at(vnode->hdr.next);
                    BloomFilter *next_bloom = &valueList->bf[next->hdr.id];
                    std::unique_lock<std::shared_mutex> next_lock(next_bloom->mtx);
                    
                    current_lock.unlock();
                    vnode = next;
                    bloom = next_bloom;
                    current_lock = std::move(next_lock);
                    continue;
                } else {
                    cout << "1 Key not found: " << key << std::endl;
                    return -1;
                }
            }
        } else {
            cout << "Key not found: " << key << std::endl;
            return -1;
        }
    }
}

#if 0
Val_t TandemIndex::lookup(Key_t key)
{
    int idx = -1;
    Vnode *vnode = nullptr;
    
    // 1. 快速获取起始节点指针
    {
        Inode *inode = mainIndex->lookup(key, idx);
        if(inode == nullptr) return -1;
        
        std::shared_lock<std::shared_mutex> lock(mainIndex->inode_locks[inode->getId()]);
        vnode = valueList->pmemVnodePool->at(inode->gps[idx].value);
    } // 尽早释放 inode 锁
    
    // 2. 无锁遍历优化
    return lookupInVnodeChain(vnode, key);
}

Val_t TandemIndex::lookupInVnodeChain(Vnode *vnode, Key_t key) 
{
    Val_t value;
    BloomFilter *bloom = &valueList->bf[vnode->hdr.id];
    std::shared_lock<std::shared_mutex> current_lock(bloom->mtx);
    
    while(true) {
        bool mightContain = bloom->mightContain(key);
        
        if(vnode->hdr.next != -1 && !mightContain) {
            // hand over lock to the next vnode
            Vnode *next = valueList->pmemVnodePool->at(vnode->hdr.next);
            BloomFilter *next_bloom = &valueList->bf[next->hdr.id];
            std::shared_lock<std::shared_mutex> next_lock(next_bloom->mtx);
            
            // unlock the current vnode
            current_lock.unlock();
            vnode = next;
            bloom = next_bloom;
            current_lock = std::move(next_lock);
            continue;
        }
        
        if(mightContain) {
            if(vnode->lookupWithoutFilter(key, value, bloom)) {
                return value;
            } else {
                // false positive, check next vnode
                if (key > vnode->getMaxKey(bloom) && vnode->hdr.next != -1) {
                    Vnode *next = valueList->pmemVnodePool->at(vnode->hdr.next);
                    BloomFilter *next_bloom = &valueList->bf[next->hdr.id];
                    std::shared_lock<std::shared_mutex> next_lock(next_bloom->mtx);
                    
                    current_lock.unlock();
                    vnode = next;
                    bloom = next_bloom;
                    current_lock = std::move(next_lock);
                    continue;
                } else {
                    return -1;
                }
            }
        } else {
            return -1;
        }
    }
}
#endif

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

void TandemIndex::recover(Key_t key)
{
    //recoveryManager->recoveryOperation(key);
}

#if 0
void TandemIndex::update(Key_t key, Val_t value)
{
    int idx = -1;
    bool needToRebalance = false;
    Vnode *targetVnode = nullptr; // new vnode to be inserted

    //1. find the start of the vnode chain in the value list
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
        BloomFilter *bloom = &valueList->bf[vnode->hdr.id];
        std::unique_lock<std::shared_mutex> lock(bloom->mtx);
        if(key > vnode->getMaxKey(bloom) && vnode->hdr.next != -1) {
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
                    valueList->split(vnode, targetVnode); //redisribute the keys between the two vnodes
                    if(key <= vnode->getMaxKey(&valueList->bf[vnode->hdr.id])) { // key is smaller than the max key of the previous value node after split
                        if(!vnode->lookup(key, pos, &valueList->bf[vnode->hdr.id])) {
                            vnode->insert(key, value, &valueList->bf[vnode->hdr.id]);
                            vnode->hdr.unsetBit(pos);
                        }
                    }else{
                        BloomFilter *target_bloom = &valueList->bf[targetVnode->hdr.id];
                        std::unique_lock<std::shared_mutex> lock_target(target_bloom->mtx);
                        if(!targetVnode->lookup(key, pos, target_bloom)) {
                            targetVnode->insert(key, value, target_bloom);
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
            Key_t targetKey;
            int pos = -1;
            {
                BloomFilter *bloom = &valueList->bf[targetVnode->hdr.id];
                std::shared_lock<std::shared_mutex> lock_target(bloom->mtx);
                targetKey = targetVnode->getMinKey(bloom);
            }
            if(inode->activateGP(targetKey, pos)) {
                bool ret = mainIndex->linkVnodeToInode(*inode, pos, *targetVnode);
                if(!ret) {
                    std::cout << "Failed to link the vnode to the inode." << std::endl;
                    return;
                }
                inode->gps[pos].key = targetKey;
                dram_log_entry_t *entry = new dram_log_entry_t(inode->getId(),inode->hdr.coveredNodes, inode->hdr.last_index, inode->hdr.next);
                for(int i = 0; i <= inode->hdr.last_index; i++) {
                    entry->setKeyVal(i, inode->gps[i].key, inode->gps[i].value);
                }
                entry->setCoveredNodes(inode->hdr.coveredNodes);
                entry->setLastIndex(inode->hdr.last_index);
                ckptLog->enq(entry);
            }else {
                //rebalance the main index, if necessary
                needToRebalance = true;
            }
        }else {
            //no needs to activate GP, just return 
            return;
        }
    } 
    if(needToRebalance && inode){
        std::unique_lock<std::shared_mutex> rebalance_lock(mainIndex->rebalance_lock);
        bool ret = mainIndex->rebalanceInode(*inode, *targetVnode);
        return;
    }
}
#endif

void TandemIndex::update(Key_t key, Val_t value)
{
     int idx = -1;
    bool needToRebalance = false;
    Vnode *targetVnode = nullptr;
    
    // 1. 查找索引节点
    Inode *inode = mainIndex->lookup(key, idx);
    if (inode == nullptr) {
        std::cout << "Failed to find the inode for the key: " << key << std::endl;
        return;
    }
    
    // 2. 快速获取起始值节点
    Vnode *vnode = nullptr;
    {
        std::shared_lock<std::shared_mutex> inode_lock(mainIndex->inode_locks[inode->getId()]);
        vnode = valueList->pmemVnodePool->at(inode->gps[idx].value);
    } // 立即释放 inode 锁
    
    if (vnode == nullptr) {
        std::cout << "Failed to find the vnode for the key: " << key << std::endl;
        return;
    }
    
    // 3. 在链表中查找并更新
    if (!findAndUpdateInVnodeChain(key, value, vnode, targetVnode)) {
        std::cout << "Failed to find the key in the vnode chain for update: " << key << std::endl;
        return;
    }
    
    // 4. 处理分裂后的索引更新
    if (targetVnode != nullptr) {
        handleIndexUpdateAfterSplit(inode, targetVnode, needToRebalance);
    }
    
    // 5. 处理重平衡
    if (needToRebalance && inode && targetVnode) {
        std::unique_lock<std::shared_mutex> rebalance_lock(mainIndex->rebalance_lock);
        mainIndex->rebalanceInode(*inode, *targetVnode);
    }
}

bool TandemIndex::findAndUpdateInVnodeChain(Key_t key, Val_t value, Vnode *startVnode, Vnode *&targetVnode)
{
    Vnode *current = startVnode;
    targetVnode = nullptr;
   
   BloomFilter *bloom = &valueList->bf[current->hdr.id]; 
   std::unique_lock<std::shared_mutex> current_lock(bloom->mtx);
   int pos = -1;
    while (true) {
        bool mightContain = bloom->mightContain(key); 
        if(current->hdr.next != -1 && !mightContain) {
            // 如果当前节点不包含键且有下一个节点，移动到下一个节点
            Vnode *next = valueList->pmemVnodePool->at(current->hdr.next);
            BloomFilter *next_bloom = &valueList->bf[next->hdr.id];
            std::unique_lock<std::shared_mutex> next_lock(next_bloom->mtx);

            current_lock.unlock(); // 手动释放当前锁
            current = next;
            bloom = next_bloom;
            current_lock = std::move(next_lock);
            continue;
        }
        if(mightContain) {
            if(current->lookupWithoutFilter(key,pos,bloom)) {
                return performUpdateInVnode(current, key, value, pos, bloom, targetVnode);
            } else {
                //false positive, check next vnode
                if (key > current->getMaxKey(bloom) && current->hdr.next != -1) {
                    Vnode *next = valueList->pmemVnodePool->at(current->hdr.next);
                    BloomFilter *next_bloom = &valueList->bf[next->hdr.id];
                    std::unique_lock<std::shared_mutex> next_lock(next_bloom->mtx);
                
                    current_lock.unlock(); // 手动释放当前锁
                    current = next;
                    bloom = next_bloom;
                    current_lock = std::move(next_lock);
                    continue;
                } else {
                    cout << "Key not found in current or subsequent nodes: " << key << std::endl;
                    //current->dump();
                    return false; // 键不在当前节点或后续节点
                } 
            }
        } else {
            cout << "Bloom filter indicates key is not present: " << key << std::endl;
            return false;
        }
    }
    cout << "Key not found in any vnode: " << key << std::endl;
    return false;
        // 找到键，执行更新操作
    //BloomFilter *target_bloom = &valueList->bf[current->hdr.id];
    //return performUpdateInVnode(current, key, value, pos, target_bloom, targetVnode);
}

// 在具体的 vnode 中执行更新操作
bool TandemIndex::performUpdateInVnode(Vnode *vnode, Key_t key, Val_t value, int pos, 
                                     BloomFilter *bloom, Vnode *&targetVnode)
{
    if (!vnode->isFull()) {
        // 简单情况：节点有空间，直接更新
        return performSimpleUpdate(vnode, key, value, pos, bloom);
    } else {
        // 复杂情况：节点已满，需要分裂
        return performUpdateWithSplit(vnode, key, value, pos, bloom, targetVnode);
    }
}

// 简单更新（节点未满）
bool TandemIndex::performSimpleUpdate(Vnode *vnode, Key_t key, Val_t value, int pos, BloomFilter *bloom)
{
    // 方法2：插入新值并清除旧标记
    if (vnode->insert(key, value, bloom)) {
        vnode->hdr.unsetBit(pos);
        return true;
    }
    
    return false;
}

// 带分裂的更新（节点已满）
bool TandemIndex::performUpdateWithSplit(Vnode *vnode, Key_t key, Val_t value, int pos, 
                                       BloomFilter *bloom, Vnode *&targetVnode)
{
    // 1. 获取新节点
    targetVnode = valueList->pmemVnodePool->getNextNode();
    if (!targetVnode) {
        std::cout << "Failed to get new vnode from pool" << std::endl;
        return false;
    }
    
    // 2. 执行分裂
    if (!valueList->split(vnode, targetVnode)) {
        std::cout << "Failed to split vnode" << std::endl;
        return false;
    }
    
    // 3. 重新定位键并更新
    return relocateAndUpdateAfterSplit(vnode, targetVnode, key, value, bloom);
}

// 分裂后重新定位并更新
bool TandemIndex::relocateAndUpdateAfterSplit(Vnode *originalVnode, Vnode *newVnode, 
                                            Key_t key, Val_t value, BloomFilter *originalBloom)
{
    Key_t splitMaxKey = originalVnode->getMaxKey(originalBloom);
    
    if (key <= splitMaxKey) {
        // 键在原节点中
        return updateAfterSplitInOriginal(originalVnode, key, value, originalBloom);
    } else {
        // 键在新节点中
        return updateAfterSplitInNew(newVnode, key, value);
    }
}

// 在原节点中更新（分裂后）
bool TandemIndex::updateAfterSplitInOriginal(Vnode *vnode, Key_t key, Val_t value, BloomFilter *bloom)
{
    int pos = -1;
    if (!vnode->lookup(key, pos, bloom)) {
        // 重新插入（分裂可能改变了键的位置）
        return vnode->insert(key, value, bloom);
    }
    
    // 更新现有位置
    // 插入新值并清除旧标记
    if (vnode->insert(key, value, bloom)) {
        vnode->hdr.unsetBit(pos);
        return true;
    }
    
    return false;
}

// 在新节点中更新（分裂后）
bool TandemIndex::updateAfterSplitInNew(Vnode *newVnode, Key_t key, Val_t value)
{
    BloomFilter *target_bloom = &valueList->bf[newVnode->hdr.id];
    std::unique_lock<std::shared_mutex> lock_target(target_bloom->mtx);
    
    int pos = -1;
    if (!newVnode->lookup(key, pos, target_bloom)) {
        // 重新插入
        return newVnode->insert(key, value, target_bloom);
    }
    
    // 更新现有位置
        // 插入新值并清除旧标记
    if (newVnode->insert(key, value, target_bloom)) {
        newVnode->hdr.unsetBit(pos);
        return true;
    }
    
    return false;
}

// 处理分裂后的索引更新
void TandemIndex::handleIndexUpdateAfterSplit(Inode *inode, Vnode *targetVnode, bool &needToRebalance)
{
    std::unique_lock<std::shared_mutex> inode_wlock(mainIndex->inode_locks[inode->getId()]);
    inode->hdr.coveredNodes++;
    
    if (!inode->checkForActivateGP()) {
        return; // 不需要激活新的 GP
    }
    
    // 获取目标键
    Key_t targetKey = getMinKeyFromVnode(targetVnode);
    
    int pos = -1;
    if (inode->activateGP(targetKey, pos)) {
        // 成功激活 GP，链接节点并记录日志
        linkVnodeAndCreateLogEntry(inode, targetVnode, pos, targetKey);
    } else {
        // 需要重平衡
        needToRebalance = true;
    }
}

// 获取 vnode 的最小键
Key_t TandemIndex::getMinKeyFromVnode(Vnode *vnode)
{
    BloomFilter *bloom = &valueList->bf[vnode->hdr.id];
    std::unique_lock<std::shared_mutex> lock_target(bloom->mtx);
    return vnode->getMinKey(bloom);
}

// 链接 vnode 并创建日志条目
void TandemIndex::linkVnodeAndCreateLogEntry(Inode *inode, Vnode *targetVnode, int pos, Key_t targetKey)
{
    // 1. 链接节点
    bool ret = mainIndex->linkVnodeToInode(*inode, pos, *targetVnode);
    if (!ret) {
        std::cout << "Failed to link the vnode to the inode." << std::endl;
        return;
    }
    
    // 2. 设置键
    inode->gps[pos].key = targetKey;
    
    // 3. 创建日志条目
    dram_log_entry_t *entry = new dram_log_entry_t(
        inode->getId(), 
        inode->hdr.coveredNodes, 
        inode->hdr.last_index, 
        inode->hdr.next
    );
    
    // 4. 填充日志数据
    for (int i = 0; i <= inode->hdr.last_index; i++) {
        entry->setKeyVal(i, inode->gps[i].key, inode->gps[i].value);
    }
    entry->setCoveredNodes(inode->hdr.coveredNodes);
    entry->setLastIndex(inode->hdr.last_index);
    
    // 5. 提交日志
    ckptLog->enq(entry);
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
        BloomFilter *bloom = &valueList->bf[vnode->hdr.id];
        std::shared_lock<std::shared_mutex> lock(bloom->mtx);
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

void TandemIndex::remove(int key)
{
    mainIndex->remove(key);
}



void TandemIndex::print()
{
    mainIndex->print();
}
#endif