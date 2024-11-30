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
    int updatedlevel = newlevel;
    inodes[newlevel - 1] = dramInodePool->getNextNode();
    inodes[newlevel - 1]->hdr.level = newlevel - 1;
    inodes[newlevel - 1]->hdr.next = std::numeric_limits<uint32_t>::max();
    inodes[newlevel - 1]->gps[0].key = key;
    for(int i = newlevel - 2; i >= 0; i--) {
        inodes[i] = dramInodePool->getNextNode();
        inodes[i]->gps[0].key = key;
        inodes[i+1]->gps[0].value = inodes[i]->getId();
        inodes[i+1]->hdr.last_index = 0;
        inodes[i+1]->hdr.coveredNodes++;
        inodes[i]->hdr.level = i;
        inodes[i]->hdr.next = std::numeric_limits<uint32_t>::max();
    }
    ret = linkVnodeToInode(*inodes[0], 0, *reinterpret_cast<Vnode *>(val));
    if(ret == false) {
        return ret;
    }
    inodes[0]->hdr.last_index = 0;
    inodes[0]->hdr.coveredNodes++;
    while(newlevel > 0) {
        inodes[newlevel-1]->hdr.next = header[newlevel-1]->hdr.next;
        header[newlevel-1]->hdr.next = inodes[newlevel-1]->getId();
        newlevel--;
    }
    if(updatedlevel > level) {
        level = updatedlevel;
    }
    return true;
}

void DramSkiplist::getPivotNodesForInsert(Key_t key, Inode* updates[])
{
    int currentHighestLevelIndex = level - 1;
    Inode *current = header[currentHighestLevelIndex];
    for(int i = currentHighestLevelIndex; i >= 0; i--) {
        Inode *prev = nullptr;
        while(current->hdr.next != tail[i]->getId() && key > current->gps[current->hdr.last_index].key) {
            prev = current;
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
    return current;
}

Inode* DramSkiplist::getHeader()
{
    return header[0];
}

bool DramSkiplist::linkVnodeToInode(Inode &inode, int idx, Vnode &vnode)
{
    inode.gps[idx].value = vnode.getId();
    return true;
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
        inodes[i+1]->hdr.coveredNodes++;
        inodes[i]->hdr.level = i;
        inodes[i]->hdr.next = std::numeric_limits<uint32_t>::max();
    }
}

// rebalance will be called when activateGP failed, in this case targetKey need to be reinsert
bool DramSkiplist::rebalanceInode(Inode &inode, Vnode &targetVnode)
{
    bool ret = false;
    Inode* updates[MAX_LEVEL];
    int newlevel = generateRandomLevel();
    //Inode *newInodes[newlevel];
    Key_t targetKey = targetVnode.getMinKey();
    //initInodes(newInodes, newlevel, targetKey);
    // updates stores the previous node of the new inodes;
    getPivotNodesForInsert(targetKey, updates);
    if(newlevel > level) { // for the case when the new level is higher than the current level
        for(int i = level; i < newlevel; i++) {
            updates[i] = header[i];
        }
        level = newlevel;
    }
    Inode *prev_update = nullptr;
    int prev_pos = 0; 
    for(int i = newlevel - 1; i >= 0; i--) {
        Inode *current_update = updates[i];
        Inode *current = nullptr;
        if(targetKey > current_update->gps[current_update->hdr.last_index].key) { //if the target key is larger than the max key in the current node
            if(!current_update->isFull()) { // append it to the last available position since the previous gps are already sorted
                current_update->hdr.last_index++;
                current_update->gps[current_update->hdr.last_index].key = targetKey;
                if(i != newlevel - 1) {
                    prev_update->gps[prev_pos].value = current_update->getId();
                    prev_update->hdr.coveredNodes++;
                }
                prev_update = current_update;
                prev_pos = current_update->hdr.last_index;
            }else { //the current node is full, need to split it into two nodes, the target key will be in the new inode.
                Inode *next = dramInodePool->at(current_update->hdr.next);
                current = dramInodePool->getNextNode(); 
                current->hdr.last_index++; // last_index = 0
                current->gps[current->hdr.last_index].key = targetKey; // for now only add the target key to the new node, later will do the range re-split.
                current->hdr.next = current_update->hdr.next;
                current_update->hdr.next = current->getId();
                if(i != newlevel - 1) {
                    prev_update->gps[prev_pos].value = current->getId();
                    prev_update->hdr.coveredNodes++;
                }else {
                    updates[i+1]->hdr.coveredNodes++;
                }

                prev_update = current;
                prev_pos = current->hdr.last_index;
            }
            
        }else if(targetKey >= current_update->gps[0].key && targetKey < current_update->gps[current_update->hdr.last_index].key) {
            //[a, b) -> [a, key) [key, b)]
            //Todo: need move half of the contents from current_update to current
            if(!current_update->isFull()) {// if the current node is not full, find the insert postion, shift the gps and insert the target key
                int pos = current_update->findInsertKeyPos(targetKey);
                current_update->shift(pos);
                current_update->hdr.last_index++;
                current_update->gps[pos].key = targetKey;
                if(i != newlevel - 1) {
                    prev_update->gps[prev_pos].value = current_update->getId();
                    prev_update->hdr.coveredNodes++;
                }
                prev_update = current_update;
                prev_pos = pos;
            } else {
                //the current node is full, need to split it into two nodes, the target key will be in one of them.
                current = dramInodePool->getNextNode();
                current->hdr.last_index++;
                current_update->split(current); // spilt will move the last half of the current node to the new node
                Inode *target = nullptr; // target is the node that the target key will be inserted
                if(targetKey < current->getMinKey()) { 
                    target = current_update;
                }else {
                    target = current;
                }
                int pos = target->findInsertKeyPos(targetKey);
                target->shift(pos);
                target->hdr.last_index++;
                target->gps[pos].key = targetKey;
                if(i != newlevel - 1) {
                    prev_update->gps[prev_pos].value = target->getId();
                    prev_update->hdr.coveredNodes++;
                }else {
                    updates[i+1]->hdr.coveredNodes++;
                }
                prev_update = target;
                prev_pos = pos;
                current->hdr.next = current_update->hdr.next;
                current_update->hdr.next = current->getId();
            }
        }
    }
    prev_update->gps[prev_pos].value = targetVnode.getId();
    return ret;
}
