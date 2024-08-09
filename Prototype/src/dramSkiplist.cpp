#include "dramSkiplist.h"
#define numNodesInPool 10000

DramSkiplist::DramSkiplist()
{
    dramInodePool = new DramInodePool(sizeof(Inode), numNodesInPool);
    header[MAX_LEVEL - 1] = dramInodePool->getNextNode();
    header[MAX_LEVEL - 1]->min_key = std::numeric_limits<Key_t>::min();
    header[MAX_LEVEL - 1]->max_key = std::numeric_limits<Key_t>::min();
    for(int i = MAX_LEVEL - 2; i >= 0; i--) {
        header[i] = dramInodePool->getNextNode();
        header[i+1]->down = header[i]->getId();
        header[i]->min_key = std::numeric_limits<Key_t>::min();
        header[i]->max_key = std::numeric_limits<Key_t>::min();
        header[i]->next = std::numeric_limits<uint32_t>::max();
    }

    tail[MAX_LEVEL - 1] = dramInodePool->getNextNode();
    for(int i = MAX_LEVEL - 2; i >= 0; i--) {
        tail[i] = dramInodePool->getNextNode();
        tail[i+1]->down = tail[i]->getId();
        tail[i]->min_key = std::numeric_limits<Key_t>::max();
        tail[i]->max_key = std::numeric_limits<Key_t>::max();
        tail[i]->next = std::numeric_limits<uint32_t>::max();
    }

    for(int i = 0; i < MAX_LEVEL; i++) {
        header[i]->next = tail[i]->getId();
    }
    level = 1;
}

int DramSkiplist::generateRandomLevel()
{
    int level = 1;
    while (rand() < RAND_MAX / 2 && level < MAX_LEVEL) {
        level++;
    }
    return level;
} 

//Val is the address of vnode, not value itself
bool DramSkiplist::insert(Key_t &key, Val_t &val)
{
    bool ret = false;
    int newlevel = generateRandomLevel();
    Inode* inodes[newlevel];
    Inode* updates[MAX_LEVEL];
    inodes[newlevel - 1] = dramInodePool->getNextNode();
    inodes[newlevel - 1]->min_key = key;
    inodes[newlevel - 1]->max_key = key;
    inodes[newlevel - 1]->next = std::numeric_limits<uint32_t>::max();
    for(int i = newlevel - 2; i >= 0; i--) {
        inodes[i] = dramInodePool->getNextNode();
        inodes[i+1]->down = inodes[i]->getId();
        inodes[i]->min_key = key;
        inodes[i]->max_key = key;
        inodes[i]->next = std::numeric_limits<uint32_t>::max();
    }
    //inodes[0]->down = reinterpret_cast<Vnode *>(val)->getId();
    ret = linkVnodeToInode(inodes[0], reinterpret_cast<Vnode *>(val));
    if(ret == false) {
        return ret;
    }
    getPivotNodesForInsert(key, updates);
    if(newlevel > level) {
        for(int i = level; i < newlevel; i++) {
            updates[i] = header[i];
        }
        level = newlevel;
    }
    while(newlevel > 0) {
        Inode *next = dramInodePool->at(updates[newlevel-1]->next);
        inodes[newlevel-1]->next = updates[newlevel-1]->next;
        updates[newlevel-1]->next = inodes[newlevel-1]->getId();
        inodes[newlevel-1]->min_key = key;
        inodes[newlevel-1]->max_key = next->min_key;
        newlevel--;
    }
    return true;
}

//updates  will store the previous node of the new inodes
bool DramSkiplist::insertWhenRebalance(Key_t &key, Val_t &val, Inode* updates[], int count)
{
    bool ret = false;
    int newlevel = generateRandomLevel();
    //inodes are the newly inserted nodes
    Inode* inodes[newlevel];
    inodes[newlevel - 1] = dramInodePool->getNextNode();
    inodes[newlevel - 1]->min_key = key;
    inodes[newlevel - 1]->max_key = key;
    inodes[newlevel - 1]->next = std::numeric_limits<uint32_t>::max();
    for(int i = newlevel - 2; i >= 0; i--) {
        inodes[i] = dramInodePool->getNextNode();
        inodes[i+1]->down = inodes[i]->getId();
        inodes[i]->min_key = key;
        inodes[i]->max_key = key;
        inodes[i]->next = std::numeric_limits<uint32_t>::max();
    }
    ret = linkVnodeToInode(inodes[0], reinterpret_cast<Vnode *>(val)); 
    getPivotNodesForInsert(key, updates);
    if(newlevel > level) {
        for(int i = level; i < newlevel; i++) {
            updates[i] = header[i];
        }
        level = newlevel;
    }
    while(newlevel > 0) {
        inodes[newlevel-1]->next = updates[newlevel-1]->next;
        updates[newlevel-1]->next = inodes[newlevel-1]->getId();
        updates[newlevel-1]->max_key = key;
        inodes[newlevel-1]->min_key = key;
        inodes[newlevel-1]->max_key = updates[newlevel-1]->max_key;
        newlevel--;
    }
    inodes[0]->coveredNodes = updates[0]->coveredNodes - count;
    updates[0]->coveredNodes = count;
    return true;
}

bool DramSkiplist::getPivotNodesForInsert(Key_t key, Inode* updates[])
{
    int currentHighestLevelIndex = level - 1;
    Inode *current = header[currentHighestLevelIndex];
    for(int i = currentHighestLevelIndex; i >= 0; i--) {
        while(current->next != tail[i]->getId() && key > current->max_key) {
            current = dramInodePool->at(current->next);
        }
        updates[i] = current;
        current = dramInodePool->at(current->down);
    }
    return true;
 }

//Get the target node that will point to the first vnode in the range that key belongs to
Inode *DramSkiplist::getPivotNode(Key_t key)
{
    Inode *node = lookup(key);
    return node;
}

bool isInKeyRange(Key_t key, Inode *node)
{
    return key >= node->min_key && key <= node->max_key;
}

Inode* DramSkiplist::lookup(Key_t key)
{
    int currentHighestLevelIndex = level - 1;
    Inode* current = header[currentHighestLevelIndex];
    for(int i = currentHighestLevelIndex; i >= 0; i--) {
        while(current->next != tail[i]->getId() && key > current->max_key) {
            current = dramInodePool->at(current->next);
        }
        if(i != 0) {
            current = dramInodePool->at(current->down);
        }
    }
    return current->min_key == std::numeric_limits<Key_t>::min() ? nullptr : current;
}

Inode* DramSkiplist::getHeader()
{
    return header[0];
}

bool DramSkiplist::linkVnodeToInode(Inode *inode, Vnode *vnode)
{
    if(inode->down == -1) {
        inode->down = vnode->getId();
        inode->coveredNodes++;
        return true;
    }
    // inode already point to the vnode range
    return false;
}

bool DramSkiplist::increaseCoveredNodesAndVerifyRebalance(Inode *inode)
{
    inode->coveredNodes++;
    if(inode->coveredNodes > SEARCH_STABLITY_COEFFICIENT) {
        return true;
    }
    return false;
}

//count is the number of covered nodes that will stay in the old inode
bool DramSkiplist::rebalanceInode(Inode *inode, Key_t key, Val_t node_id, int count)
{
    bool ret = false;
    Inode* updates[MAX_LEVEL];
    ret = insertWhenRebalance(key, node_id, updates, count); 
    return ret;
}