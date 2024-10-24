#include "dramSkiplist.h"
#define numNodesInPool 10000

DramSkiplist::DramSkiplist()
{
    dramInodePool = new DramInodePool(sizeof(Inode), numNodesInPool);
    header[MAX_LEVEL - 1] = dramInodePool->getNextNode();
    header[MAX_LEVEL - 1]->gps[0].key = std::numeric_limits<Key_t>::min();
    header[MAX_LEVEL - 1]->gps[fanout/2].key = std::numeric_limits<Key_t>::min();
    header[MAX_LEVEL - 1]->hdr.last_index = 0;
    for(int i = MAX_LEVEL - 2; i >= 0; i--) {
        header[i] = dramInodePool->getNextNode();
        header[i+1]->gps[0].value = header[i]->getId();
        header[i]->gps[0].key = std::numeric_limits<Key_t>::min();
        header[i]->hdr.next = std::numeric_limits<uint32_t>::max();
        header[i]->hdr.last_index = 0;
    }

    tail[MAX_LEVEL - 1] = dramInodePool->getNextNode();
    for(int i = MAX_LEVEL - 2; i >= 0; i--) {
        tail[i] = dramInodePool->getNextNode();
        tail[i+1]->gps[0].value = tail[i]->getId();
        tail[i]->gps[0].key = std::numeric_limits<Key_t>::max();
        tail[i]->gps[fanout/2].key = std::numeric_limits<Key_t>::max();
        tail[i]->hdr.next = std::numeric_limits<uint32_t>::max();
    }

    for(int i = 0; i < MAX_LEVEL; i++) {
        header[i]->hdr.next = tail[i]->getId();
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

//Val is the address of vnode, not value itself, this insert is used for the first insert or key is the smallest
bool DramSkiplist::insert(Key_t &key, Val_t &val, Inode *inodes[], int newlevel)
{
    bool ret = false;
    inodes[newlevel - 1] = dramInodePool->getNextNode();
    inodes[newlevel - 1]->hdr.level = newlevel - 1;
    inodes[newlevel - 1]->hdr.next = std::numeric_limits<uint32_t>::max();
    inodes[newlevel - 1]->gps[0].key = key;
    for(int i = newlevel - 2; i >= 0; i--) {
        inodes[i] = dramInodePool->getNextNode();
        inodes[i]->gps[0].key = key;
        inodes[i+1]->gps[0].value = inodes[i]->getId();
        inodes[i]->hdr.level = i;
        inodes[i]->hdr.next = std::numeric_limits<uint32_t>::max();
    }
    ret = linkVnodeToInode(*inodes[0], 0, *reinterpret_cast<Vnode *>(val));
    if(ret == false) {
        return ret;
    }
    while(newlevel > 0) {
        inodes[newlevel-1]->hdr.next = header[newlevel-1]->hdr.next;
        header[newlevel-1]->hdr.next = inodes[newlevel-1]->getId();
        newlevel--;
    }
    return true;
}

//updates  will store the previous node of the new inodes
bool DramSkiplist::insertWhenRebalance(Key_t &key, Val_t &val, Inode* updates[], int count)
{
    bool ret = true;
    int newlevel = generateRandomLevel();
    //inodes are the newly inserted nodes
    Inode* inodes[newlevel];
    inodes[newlevel - 1] = dramInodePool->getNextNode();
    inodes[newlevel - 1]->hdr.level = newlevel - 1;
    inodes[newlevel - 1]->hdr.next = std::numeric_limits<uint32_t>::max();
    inodes[newlevel - 1]->gps[0].key = key;
    for(int i = newlevel - 2; i >= 0; i--) {
        inodes[i] = dramInodePool->getNextNode();
        inodes[i]->gps[0].key = key;
        inodes[i+1]->gps[0].value = inodes[i]->getId();
        inodes[i]->hdr.level = i;
        inodes[i]->hdr.next = std::numeric_limits<uint32_t>::max();
    }
    getPivotNodesForInsert(key, updates);
    if(newlevel > level) {
        for(int i = level; i < newlevel; i++) {
            updates[i] = header[i];
        }
        level = newlevel;
    }
    while(newlevel > 0) {
        // if the updates[newlevel-1] is the header noode
        Inode *current_update = updates[newlevel-1];
        Inode *current = inodes[newlevel-1];
        if(current_update->gps[current_update->hdr.last_index].key == std::numeric_limits<Key_t>::min()) {
            Inode *next = dramInodePool->at(current_update->hdr.next);
            current->gps[current->hdr.last_index].key = key;
            current->hdr.last_index++;
            current->gps[current->hdr.last_index].key = next->gps[0].key;
        }else {
            //[a, b) -> [a, key) [key, b)]kk
            current->gps[current->hdr.last_index].key = current_update->gps[current_update->hdr.last_index].key;
            current_update->gps[current_update->hdr.last_index].key = key;
            current->gps[0].key = key;
        }
        current->hdr.next = current_update->hdr.next;
        current_update->hdr.next = current->getId();
        newlevel--;
    }
    inodes[0]->hdr.coveredNodes = updates[0]->hdr.coveredNodes - count;
    updates[0]->hdr.coveredNodes = count;
    return ret;
}

void DramSkiplist::getPivotNodesForInsert(Key_t key, Inode* updates[])
{
    int currentHighestLevelIndex = level - 1;
    Inode *current = header[currentHighestLevelIndex];
    for(int i = currentHighestLevelIndex; i >= 0; i--) {
        Inode *prev = nullptr;
        while(current->hdr.next != tail[i]->getId() && key > current->gps[current->hdr.last_index].key) {
            current = dramInodePool->at(current->hdr.next);
            if(key < current->gps[0].key && prev != nullptr) {
                current = prev;
                break;
            }
        }
        updates[i] = current;
        if(current->isHeader()) {
            if(i != 0) {
                current = dramInodePool->at(current->gps[0].value);
                continue;
            }
        }
        Val_t index = 0;
        for(int j = 0; j <= current->hdr.last_index; j++) {
            if(key >= current->gps[j].key) {
                if(j + 1 <= current->hdr.last_index) {
                    if(key < current->gps[j+1].key) {
                        index = j;
                        break;
                    }
                } else {
                    index = j;
                    break;
                }
            }
        }
        if(i != 0) {
            current = dramInodePool->at(current->gps[index].value);
        }
    }
 }

Inode* DramSkiplist::lookup(Key_t key, int &idx)
{
    int currentHighestLevelIndex = level - 1;
    Inode* current = header[currentHighestLevelIndex];
    for(int i = currentHighestLevelIndex; i >= 0; i--) {
        // no real index nodes between header and tail
        Inode* prev = nullptr;
        //search among the nodes in the current level
        while(current->hdr.next != tail[i]->getId() && key > current->gps[current->hdr.last_index].key) {
            prev = current;
            current = dramInodePool->at(current->hdr.next);
            if(key < current->gps[0].key && prev != nullptr) {
                current = prev;
                break;
            }
        }
        if(current->isHeader()) { // if the current node is the header node, only happens when the key is the smallest
            if(i != 0) {
                current = dramInodePool->at(current->gps[0].value);
                continue;
            } else {
                idx = -1;
                return nullptr;
            }
        }
        for(int j = 0; j <= current->hdr.last_index; j++) {
            if(key >= current->gps[j].key) {
                if(j + 1 <= current->hdr.last_index) {
                    if(key < current->gps[j+1].key) {
                        idx = j;
                        break;
                    }
                } else {
                    idx = j;
                    break;
                }
            }
        }
        if(i != 0) {
            current = dramInodePool->at(current->gps[idx].value);
        }
    }
#if 0
    //search inside the node in the last level
    for(int j = 0; j <= current->hdr.last_index; j++) {
        if(key >= current->gps[j].key && key < current->gps[j+1].key) {
            idx = j;
            return current;
        }
    }
#endif
    return current;
}

#if 0
Vnode *DramSkiplist::lookup(Key_t key)
{
    int currentHighestLevelIndex = level - 1;
    Inode* current = header[currentHighestLevelIndex];
    for(int i = currentHighestLevelIndex; i >= 0; i--) {
        Inode* prev = nullptr;
        while(current->next != tail[i]->getId() && key > current->max_key) {
            prev = current;
            current = dramInodePool->at(current->next);
        }
        if(key < current->min_key && prev != nullptr) {
                current = prev;
        }
        if(i != 0) {
            current = dramInodePool->at(current->down);
        }
    }
}
#endif

Inode* DramSkiplist::getHeader()
{
    return header[0];
}

bool DramSkiplist::linkVnodeToInode(Inode &inode, int idx, Vnode &vnode)
{
    if(inode.gps[idx].value == std::numeric_limits<Val_t>::max()) {
        inode.gps[idx].value = vnode.getId();
        inode.hdr.last_index = idx;
        return true;
    }
    return false;
}

bool DramSkiplist::checkForRebalance(Inode &inode, bool &activeNewGP)
{
    bool ret = false;
    if(inode.hdr.coveredNodes > SEARCH_STABLITY_COEFFICIENT * (inode.hdr.last_index + 1)) {
        activeNewGP = true;
    }
    if(inode.hdr.last_index == fanout/2 - 1) {
        ret = true;
    }
    return ret;
}

//return true if the inode cant 1. allocate new gp 2. slide gps
bool DramSkiplist::increaseCoveredNodesAndVerifyRebalance(Inode &inode, bool &activeNewGP)
{
    bool ret = false;
    inode.hdr.coveredNodes++;
    if(inode.hdr.coveredNodes > SEARCH_STABLITY_COEFFICIENT * (inode.hdr.last_index + 1)) {
        activeNewGP = true;
    }
    if(inode.hdr.last_index == fanout/2 - 1) {
        ret = true;
    }
    return ret;
}

//count is the number of covered nodes that will stay in the old inode
bool DramSkiplist::rebalanceInode(Inode *inode, Key_t key, Val_t node_id, int count)
{
    bool ret = false;
    Inode* updates[MAX_LEVEL];
    ret = insertWhenRebalance(key, node_id, updates, count); 
    return ret;
}

void DramSkiplist::initInodes(Inode* inodes[], int newlevel, Key_t key)
{
    inodes[newlevel - 1] = dramInodePool->getNextNode();
    inodes[newlevel - 1]->hdr.level = newlevel - 1;
    inodes[newlevel - 1]->hdr.next = std::numeric_limits<uint32_t>::max();
    inodes[newlevel - 1]->gps[0].key = key;
    for(int i = newlevel - 2; i >= 0; i--) {
        inodes[i] = dramInodePool->getNextNode();
        inodes[i]->gps[0].key = key;
        inodes[i+1]->gps[0].value = inodes[i]->getId();
        inodes[i]->hdr.level = i;
        inodes[i]->hdr.next = std::numeric_limits<uint32_t>::max();
    }
}

bool DramSkiplist::rebalanceInode(Inode &inode, Vnode &targetVnode)
{
    bool ret = false;
    Inode* updates[MAX_LEVEL];
    int newlevel = generateRandomLevel();
    Inode *newInodes[newlevel];
    Key_t targetKey = targetVnode.getMinKey();
    initInodes(newInodes, newlevel, targetKey);
    // updates stores the previous node of the new inodes;
    getPivotNodesForInsert(targetKey, updates);
    if(newlevel > level) { // for the case when the new level is higher than the current level
        for(int i = level; i < newlevel; i++) {
            updates[i] = header[i];
        }
        level = newlevel;
    }
    while(newlevel > 0) {
        Inode *current_update = updates[newlevel-1];
        Inode *current = newInodes[newlevel-1];
        if(targetKey > current_update->gps[current_update->hdr.last_index].key) {
            Inode *next = dramInodePool->at(current_update->hdr.next);
            current->gps[current->hdr.last_index].key = targetKey;
            
        }else if(targetKey >= current_update->gps[0].key && targetKey < current_update->gps[current_update->hdr.last_index].key) {
            //[a, b) -> [a, key) [key, b)]
            //Todo: need move half of the contents from current_update to current
            current->gps[current->hdr.last_index].key = current_update->gps[current_update->hdr.last_index].key;
            current->gps[current->hdr.last_index].value = current_update->gps[current_update->hdr.last_index].value;
            current_update->gps[current_update->hdr.last_index].key = inode.gps[inode.hdr.last_index].key;
            current_update->gps[current_update->hdr.last_index].value = inode.gps[inode.hdr.last_index].value;
            current->gps[0].key = inode.gps[inode.hdr.last_index].key;
            current->gps[0].value = inode.gps[inode.hdr.last_index].value;
        }
        current->hdr.next = current_update->hdr.next;
        current_update->hdr.next = current->getId();
        if(newlevel == 1) {
            current->gps[current->hdr.last_index].value = targetVnode.getId();
        }
        newlevel--;
    }
    return ret;
}
