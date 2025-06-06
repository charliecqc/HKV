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
        std::shared_lock<std::shared_mutex> lock_target(bloom->mtx);
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


void TandemIndex::update(Key_t key, Val_t value)
{
    int idx = -1;
    bool needToRebalance = false;
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