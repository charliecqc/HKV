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
        return false;
    }
//4. insert the new inodes with key and value node id into the main index 
    if (inode == nullptr) {
        Val_t vnode_id = reinterpret_cast<Val_t>(vnode);
       ret = mainIndex->insert(key, vnode_id);
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

int TandemIndex::lookup(int key)
{
    return mainIndex->lookup(key);
}

void TandemIndex::print()
{
    mainIndex->print();
}
#endif