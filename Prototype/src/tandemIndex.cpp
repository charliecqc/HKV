#include "tandemIndex.h"
#include "valuelist.h"
#include "common.h"

bool TandemIndex::insert(Key_t key, Val_t value)
{
//0. find the start of the vnode chain in the value list
    int idx = -1;
    bool ret = false;
    bool needToRebalance = false;
    Inode *inode = mainIndex->lookup(key, idx);
    if(inode != nullptr) {
        Vnode *valueNode = valueList->pmemVnodePool->at(inode->gps[idx].value);
        while(valueNode->hdr.next != -1 && key > valueNode->getMaxKey()) {
            valueNode = valueList->pmemVnodePool->at(valueNode->hdr.next);
        }
        ret = valueNode->insert(key, value);
        if(ret) {
            // insert vaule to value node successfully (there was a empty slot)
            return ret;
        }
        else {
            // need to allocate a new value node
            Vnode *vnode = valueList->pmemVnodePool->getNextNode();
            if(vnode == nullptr) {
                std::cout << "Failed to get a new node from the pool." << std::endl;
                return false;
            }
            // insert the key and value into new vnode
            ret = vnode->insert(key, value);
            if(ret == false) {
                std::cout << "Failed to insert the key and value into the vnode." << std::endl;
                return ret;
            }
            //insert the vnode into the value list
            //startNode: the start of the range that vnode should be inserted
            Vnode *startNode = valueList->pmemVnodePool->at(inode->gps[idx].value);
            ret = valueList->insert(startNode, vnode);
            if(ret == false) {
                //Todo: rollback vnode
                std::cout << "Failed to insert the value in the value list." << std::endl;
                return ret;
            }
            //incease the covered nodes of the inode due to newly added vnodes
            inode->hdr.coveredNodes++;
            if(inode->checkForActivateGP()) {
                if(inode->activateGP()) {
                    //Vnode *vnode = getVnodeForNewGP(*inode);
                    ret = mainIndex->linkVnodeToInode(*inode, inode->hdr.last_index, *vnode);
                    if(!ret) {
                        std::cout << "Failed to link the vnode to the inode." << std::endl;
                        return ret;
                    }
                    inode->gps[inode->hdr.last_index].key = vnode->getMaxKey();
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
    }else {
        // case of no inode is found
        //in the case that eitehr key is smaller than the min key of first inode or current value node is full
        Vnode *vnode = valueList->pmemVnodePool->getNextNode();
        if(vnode == nullptr) {
            std::cout << "Failed to get a new node from the pool." << std::endl;
            return false;
        }
        ret = vnode->insert(key, value);
        if(ret == false) {
            std::cout << "Failed to insert the key and value into the vnode." << std::endl;
            return ret;
        }
        // startNode is the first node of the value list
        Vnode *startNode = valueList->getHeader();
        ret = valueList->insert(startNode, vnode);
        if(ret == false) {
            //Todo: rollback vnode
            std::cout << "Failed to insert the value in the value list." << std::endl;
            return ret;
        }
        Val_t vnodeVal = reinterpret_cast<Val_t>(vnode);
        int newLevel = mainIndex->generateRandomLevel();
        Inode *inodes[newLevel];
        ret = mainIndex->insert(key, vnodeVal, inodes, newLevel);
        if(ret == false) {
            std::cout << "Failed to insert the key and value into the main index." << std::endl;
        }
        inodes[0]->hdr.coveredNodes++;
    }
//4. rebalance the main index, if necessary
    if(needToRebalance && inode) {
        ret = mainIndex->rebalanceInode(*inode);    
    }
    return ret;
}

Val_t TandemIndex::lookup(Key_t key)
{
    int idx = -1;
    Inode *inode = mainIndex->lookup(key, idx);
    if(inode == nullptr) {
        return -1;
    }
    Vnode *vnode = valueList->pmemVnodePool->at(inode->gps[idx].value);
    while(vnode->hdr.next != -1 && key > vnode->getMaxKey()) {
        vnode = valueList->pmemVnodePool->at(vnode->hdr.next);
    }
    Val_t value;
    bool ret = vnode->lookup(key, value);
    if(ret) {
        return value;
    }
    return -1;
}

Vnode *TandemIndex::getVnodeForNewGP(Inode &inode)
{
    int preIndex = inode.hdr.last_index - 1;
    int curIndex = inode.hdr.last_index;
    Vnode *preVnode = valueList->pmemVnodePool->at(inode.gps[preIndex].value);
    Vnode *curVnode = valueList->pmemVnodePool->at(inode.gps[curIndex].value);
    int count =  SEARCH_STABLITY_COEFFICIENT / 2;
    Vnode *nextVnode = preVnode;
    while(count > 0) {
        nextVnode = valueList->pmemVnodePool->at(nextVnode->hdr.next);
        count--;
    }
    return nextVnode;
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