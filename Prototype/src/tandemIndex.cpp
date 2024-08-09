#include "tandemIndex.h"
#include "valuelist.h"
#include "common.h"

bool TandemIndex::insert(Key_t key, Val_t value)
{
//1. allocate a new vnode 
    Vnode *vnode = valueList->pmemVnodePool->getNextNode();
    if(vnode == nullptr) {
        std::cout << "Failed to get a new node from the pool." << std::endl;
        return false;
    }
    vnode->key = key;
    vnode->value = value;
//2. find the start of the vnode chain in the value list
    Inode *inode = mainIndex->lookup(key);
    Vnode* startNode = nullptr;
    if(inode != nullptr) {
        startNode = valueList->pmemVnodePool->at(inode->down);
    } else {
        startNode = valueList->head;
    }
//3. insert the vnode into the value list
    bool ret = valueList->insert(startNode, vnode);
    if(ret == false) {
        //Todo: rollback vnode
        std::cout << "Failed to insert the value in the value list." << std::endl;
        return ret;
    }
//4. insert the new inodes with key and value node id into the main index 
    if (inode == nullptr) {
        Val_t vnode_id = reinterpret_cast<Val_t>(vnode);
        ret = mainIndex->insert(key, vnode_id);
    }else {
        if(mainIndex->increaseCoveredNodesAndVerifyRebalance(inode)) {
            //Todo: rebalance the inode
            bool lastLevelInode = true;
            if(lastLevelInode) {
                Vnode* currentNode = valueList->pmemVnodePool->at(inode->down);
                int count = 1;
                while(count <= (inode->coveredNodes) / 2) {
                    currentNode = valueList->pmemVnodePool->at(currentNode->next);
                    count ++;
                }
                Key_t key = currentNode->key;
                Val_t vnode_id = reinterpret_cast<Val_t>(currentNode);
                ret = mainIndex->rebalanceInode(inode, key, vnode_id, count);
            }
        }
    }
    return ret;
}

Val_t TandemIndex::lookup(Key_t key)
{
    Inode *inode = mainIndex->lookup(key);
    if(inode == nullptr) {
        return -1;
    }
    Vnode *vnode = valueList->pmemVnodePool->at(inode->down);
    while(vnode != nullptr) {
        if(vnode->key == key) {
            return vnode->value;
        } else if(vnode->key > key) {
            return -1;
        }
        vnode = valueList->pmemVnodePool->at(vnode->next);
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