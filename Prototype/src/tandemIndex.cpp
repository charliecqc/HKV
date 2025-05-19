#include <queue>
#include <vector>
#include "tandemIndex.h"
#include "valuelist.h"
#include "spinLock.h"
#include "workerThread.h"
#include "checkpoint.h"
#include "common.h"

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
    //dramInodePool = new DramInodePool(sizeof(Inode), MAX_NODES);
    valueList = new ValueList();
    pmemRecoveryArray = new PmemInodePool(sizeof(Inode), MAX_NODES);
    recoveryManager = new RecoveryManager(pmemRecoveryArray); 
    int level = recoveryManager->recoveryOperation();
    dramInodePool = recoveryManager->getDramInodePool();
    ckptLog = new CkptLog(LOG_SIZE);
    PmemManager::flushToNVM(3, reinterpret_cast<char *>(ckptLog), sizeof(ckptLog));
    //ckpq = new CheckpointQueue();
    //assert(ckpq->isEmpty());
    mainIndex = new DramSkiplist(ckptLog, dramInodePool);
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

bool TandemIndex::insert(Key_t key, Val_t value)
{
//0. find the start of the vnode chain in the value list
    int idx = -1;
    bool ret = false;
    bool needToRebalance = false;
    Inode *inode = mainIndex->lookup(key, idx);
    Vnode *targetVnode = nullptr; // new vnode to be inserted
    if(inode != nullptr) {
        std::shared_lock<std::shared_mutex> inode_lock(mainIndex->inode_locks[inode->getId()]);
        Vnode *valueNode = valueList->pmemVnodePool->at(inode->gps[idx].value);
        inode_lock.unlock();
        while(true) {
            std::unique_lock<std::shared_mutex> lock_value(valueNode->hdr.mtx);
            if(key > valueNode->getMaxKey() && valueNode->hdr.next != -1) {
                valueNode = valueList->pmemVnodePool->at(valueNode->hdr.next);
            }else {
                if(valueNode->insert(key, value)) {
                    return true;
                }else {
                    //value node is full
                    targetVnode = valueList->pmemVnodePool->getNextNode();  // get a new vnode;
                    valueList->split(valueNode, targetVnode); //redisribute the keys between the two vnodes
                    if(key <= valueNode->getMaxKey()) { // key is smaller than the max key of the previous value node after split
                        if(!valueNode->insert(key, value)) {
                            std::cout << "Failed to insert the key and value into the vnode after split. key: " << key << std::endl;
                            return false;
                        }
                    }else{
                        std::unique_lock<std::shared_mutex> lock_target(targetVnode->hdr.mtx);
                        if(!targetVnode->insert(key, value)) {
                            std::cout << "Failed to insert the key and value into the vnode." << std::endl;
                            return false;
                        }
                    }
                    break; // going to the next step where increase inode's covered nodes and active GP if its necessary
                }
            }
        }
        //incease the covered nodes of the inode due to newly added vnodes
        {
            std::unique_lock<std::shared_mutex> inode_wlock(mainIndex->inode_locks[inode->getId()]);
            inode->hdr.coveredNodes++;
            if(inode->checkForActivateGP()) {
                //activate GP
                Key_t targetKey;
                int pos = -1;
                {
                    std::shared_lock<std::shared_mutex> lock_target(targetVnode->hdr.mtx);
                    targetKey = targetVnode->getMinKey();
                }
                if(inode->activateGP(targetKey, pos)) {
                    ret = mainIndex->linkVnodeToInode(*inode, pos, *targetVnode);
                    if(!ret) {
                        std::cout << "Failed to link the vnode to the inode." << std::endl;
                        return ret;
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
                return true;
            }
        }
    }else {
        // case of no inode is found
        //in the case that either the key is smaller than the min key of first inode or current value node is full
        Vnode *headVnode = valueList->getHeader();
        {
            std::shared_lock<std::shared_mutex> lock(headVnode->hdr.mtx);
            targetVnode = valueList->pmemVnodePool->at(headVnode->hdr.next);
            if(targetVnode != nullptr) {
                std::unique_lock<std::shared_mutex> lock_target(targetVnode->hdr.mtx);
                if(!targetVnode->isFull()) {
                    Key_t old_min_key = targetVnode->getMinKey();
                    if(targetVnode->insert(key, value)) {
                     // propogating the new minkey to the related inodes
                        ret = mainIndex->update(old_min_key, key, value);
                        return ret;                 
                    }
                }
            }
        }
        // the first vnode either not exist or full, need to add it to header
        targetVnode = valueList->pmemVnodePool->getNextNode();
        if(targetVnode == nullptr) {
            std::cout << "Failed to get a new node from the pool." << std::endl;
            return false;
        }
        ret = targetVnode->insert(key, value);
        if(ret == false) {
            std::cout << "Failed to insert the key and value into the vnode when inode is null. Key: " << key << std::endl;
            return ret;
        }
        // headVnode is the first node of the value list
        ret = valueList->append(headVnode, targetVnode);
        if(ret == false) {
                //Todo: rollback vnode
            std::cout << "Failed to append new node to the valuelist." << std::endl;
            return ret;
        }
        //vnode is successfully inserted into the value list, unlock the head node
        ret = mainIndex->insert(targetVnode);
        if(ret == false) {
            std::cout << "Failed to insert the key and value into the main index." << std::endl;
        }
    #ifdef DBG
        int id = inode->getId();
        cout << "inserted inode " << id <<endl;
    #endif
    }
     if(needToRebalance && inode){
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
    {
        std::shared_lock<std::shared_mutex> lock(mainIndex->inode_locks[inode->getId()]);
        vnode = valueList->pmemVnodePool->at(inode->gps[idx].value);
    }
    while(true) {
        std::shared_lock<std::shared_mutex> lock(vnode->hdr.mtx);
#ifdef DBG
        cout << "look up $_vnode id: " << vnode->hdr.id << " max key: " << vnode->getMaxKey() << endl;
#endif
        if(vnode->hdr.next != -1 && key > vnode->getMaxKey()) {
            Vnode *next = valueList->pmemVnodePool->at(vnode->hdr.next);
            lock.unlock();
            vnode = next;
        }else {
            if(vnode->lookup(key, value)) {
                return value;
            } else {
//#ifdef DBG
                lock.unlock();
                std::unique_lock<std::shared_mutex> lock_vnode(vnode->hdr.mtx);
                vnode->dump();
                Vnode *next_vnode = nullptr;
                if(vnode->hdr.next != -1) {
                    next_vnode= valueList->pmemVnodePool->at(vnode->hdr.next);
                    std::cout << "this is next vnode: " << std::endl;
                    std::shared_lock<std::shared_mutex> lock_next(next_vnode->hdr.mtx);
                    next_vnode->dump();
                }else {
                    std::cout << " this is the last vnode" << std::endl;
                }
//#endif
                cout << "Failed to find the key in the value list." << endl;
                return -1;
            }
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
        std::unique_lock<std::shared_mutex> lock(vnode->hdr.mtx);
        if(key > vnode->getMaxKey() && vnode->hdr.next != -1) {
            vnode = valueList->pmemVnodePool->at(vnode->hdr.next);
        }else {
            int pos = -1;
            if(vnode->lookup(key,pos)) {
                if(!vnode->isFull()) {
                    vnode->insert(key, value);
                    vnode->hdr.unsetBit(pos);
                    return;
                }else {
                    targetVnode = valueList->pmemVnodePool->getNextNode();  // get a new vnode;
                    valueList->split(vnode, targetVnode); //redisribute the keys between the two vnodes
                    if(key <= vnode->getMaxKey()) { // key is smaller than the max key of the previous value node after split
                        if(!vnode->lookup(key, pos)) {
                            vnode->insert(key, value);
                            vnode->hdr.unsetBit(pos);
                        }
                    }else{
                        std::unique_lock<std::shared_mutex> lock_target(targetVnode->hdr.mtx);
                        if(!targetVnode->lookup(key, pos)) {
                            targetVnode->insert(key, value);
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
                std::shared_lock<std::shared_mutex> lock_target(targetVnode->hdr.mtx);
                targetKey = targetVnode->getMinKey();
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