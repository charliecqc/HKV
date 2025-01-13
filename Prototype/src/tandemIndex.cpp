#include <queue>
#include <vector>
#include "tandemIndex.h"
#include "valuelist.h"
#include "spinLock.h"
#include "workerThread.h"
#include "common.h"

std::queue<std::vector<wq_entry *>*> g_workQueue[WORKERQUEUE_NUM];
//std::queue <wq_entry_t *> g_workQueue;
//std::vector<int> g_workQueue;
bool wqReady[WORKERQUEUE_NUM] = {false};
volatile bool wtInitialized = false;
std::atomic<bool> g_endTandem;
SpinLock g_spinLock;

TandemIndex::TandemIndex() {
    g_endTandem = false;   
    mainIndex = new DramSkiplist();
    //shadowIndex = new PmemSkiplist();
    valueList = new ValueList();
    //createWorkerThread();
    Inode *index_header = mainIndex->getHeader();
    Vnode *value_header = valueList->getHeader();
    index_header->gps[0].value = value_header->getId();
}

TandemIndex::~TandemIndex() {
   g_endTandem = true; 
   if(workerThread->joinable()) {
       workerThread->join();
       delete workerThread;
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
        shared_lock<std::shared_mutex> inode_lock(inode->hdr.mtx);
        Vnode *valueNode = valueList->pmemVnodePool->at(inode->gps[idx].value);
        inode_lock.unlock();
        {
            while(1) {
                std::shared_lock<std::shared_mutex> lock_value(valueNode->hdr.mtx);
                if(key > valueNode->getMaxKey() && valueNode->hdr.next != -1) {
                    Vnode *temp = valueList->pmemVnodePool->at(valueNode->hdr.next);
                    lock_value.unlock();
                    valueNode = temp;
                }else 
                    break;

            }
#if 0
            std::shared_lock<std::shared_mutex> lock_value(valueNode->hdr.mtx);
            while(valueNode->hdr.next != -1 && key > valueNode->getMaxKey() && valueNode->isFull()) {
                Vnode *temp = valueList->pmemVnodePool->at(valueNode->hdr.next);
                lock_value.unlock();
                valueNode = temp;
            }
#endif
        }
        {
            std::unique_lock<std::shared_mutex> lock_value(valueNode->hdr.mtx);
            ret = valueNode->insert(key, value);
        }
        if(ret) {
            // insert vaule to value node successfully (there was a empty slot)
            return ret;
        }
        else {
            // need to allocate a new value node
            targetVnode = valueList->pmemVnodePool->getNextNode();
            if(targetVnode == nullptr) {
                std::cout << "Failed to get a new node from the pool." << std::endl;
                return false;
            }
            //target node is full, need to split the value node
            {
                std::unique_lock<std::shared_mutex> lock_value(valueNode->hdr.mtx);
                ret = valueList->split(valueNode, targetVnode);
                if(ret == false) {
                    std::cout << "Failed to split the value node." << std::endl;
                }
                if(key <= valueNode->getMaxKey()) { // key is smaller than the max key of the previous value node after split
                    ret = valueNode->insert(key, value);
                    if(ret == false) {
                        std::cout << "Failed to insert the key and value into the vnode after split. key: " << key << std::endl;
                        return ret;
                    }
                } else {
                    lock_value.unlock();
                    {
                        std::unique_lock<std::shared_mutex> lock_target(targetVnode->hdr.mtx);
                        ret = targetVnode->insert(key, value);
                        if(ret == false) {
                            std::cout << "Failed to insert the key and value into the vnode." << std::endl;
                            return ret;
                        }
                    }
                }
            }
            //incease the covered nodes of the inode due to newly added vnodes
            {
                // scope of inode_wlock
                std::unique_lock<std::shared_mutex> inode_wlock(inode->hdr.mtx);
                inode->hdr.coveredNodes++;
                if(inode->checkForActivateGP()) {
                    Key_t targetKey;
                    {
                        std::shared_lock<std::shared_mutex> lock_target(targetVnode->hdr.mtx);
                        targetKey = targetVnode->getMinKey();
                    }
                    int pos = -1;
                    if(inode->activateGP(targetKey, pos)) {
                        ret = mainIndex->linkVnodeToInode(*inode, pos, *targetVnode);
                        if(!ret) {
                            std::cout << "Failed to link the vnode to the inode." << std::endl;
                            return ret;
                        }
                        inode->gps[pos].key = targetKey;
                    }else {
                        //TODO: rebalance the inode
                        //inode has no empty gp slots, need to split the inode
                        needToRebalance = true;
                    }
                }
                else {
                    //no needs to activate GP, just return 
                    return true;
                }
            }
        }
    }else {
        // case of no inode is found
        //in the case that eitehr key is smaller than the min key of first inode or current value node is full
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
        Val_t vnodeVal = reinterpret_cast<Val_t>(targetVnode);
        int newLevel = mainIndex->generateRandomLevel();
        Inode *inodes[newLevel];
        ret = mainIndex->insert(key, vnodeVal, inodes, newLevel);
        if(ret == false) {
            std::cout << "Failed to insert the key and value into the main index." << std::endl;
        }
        inode = inodes[0];
    #ifdef DBG
        int id = inode->getId();
        cout << "inserted inode " << id <<endl;
    #endif
    }
//4. rebalance the main index, if necessary
    if(needToRebalance && inode) {
#ifdef DBG
        int id = inode->getId();    
        cout << "Need to rebalance the inode " << id << " with key " << targetVnode->getMaxKey() << endl;
#endif
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
        std::shared_lock<std::shared_mutex> lock(inode->hdr.mtx);
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
#ifdef DBG
                vnode->dump();
                Vnode *next = valueList->pmemVnodePool->at(vnode->hdr.next);
                std::cout << "this is next vnode" << std::endl;
                next->dump();
#endif
                cout << "Failed to find the key in the value list." << endl;
                return -1;
            }
        }
    }
}

void TandemIndex::createWorkerThread()
{
    g_spinLock.lock();
    workerThread = new std::thread(&TandemIndex::workerThreadExec, this);
    wtInitialized = true;
    g_spinLock.unlock();
}

void TandemIndex::workerThreadExec()
{
    while(true)
    {
        g_spinLock.lock();
        if(!wtInitialized) {
            g_spinLock.unlock();
            usleep(500);
            continue;
        }
        g_spinLock.unlock();
    }
    while(!g_endTandem) {
    }
}

#if 0
void TandemIndex::update(int key, int value)
{
    mainIndex->update(key, value);
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