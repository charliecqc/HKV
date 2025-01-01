#include "dramSkiplist.h"
#include <mutex>
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
    std::unique_lock<std::shared_mutex> lock(inodes[newlevel - 1]->hdr.mtx);
    inodes[newlevel - 1]->hdr.level = newlevel - 1;
    inodes[newlevel - 1]->hdr.next = std::numeric_limits<uint32_t>::max();
    inodes[newlevel - 1]->gps[0].key = key;
    lock.unlock();
    for(int i = newlevel - 2; i >= 0; i--) {
        inodes[i] = dramInodePool->getNextNode();
        {
            std::unique_lock<std::shared_mutex> ilock(inodes[i]->hdr.mtx);
            inodes[i]->gps[0].key = key;
            inodes[i]->hdr.level = i;
            inodes[i]->hdr.next = std::numeric_limits<uint32_t>::max();
        }
        {
            std::unique_lock<std::shared_mutex> ilock1(inodes[i+1]->hdr.mtx);
            inodes[i+1]->gps[0].value = inodes[i]->getId();
            inodes[i+1]->hdr.last_index = 0;
            inodes[i+1]->hdr.coveredNodes++;
        }
        
    }
    {
        std::unique_lock<std::shared_mutex> lock2(inodes[0]->hdr.mtx);
        ret = linkVnodeToInode(*inodes[0], 0, *reinterpret_cast<Vnode *>(val));
        if(ret == false) {
            return ret;
        }
        inodes[0]->hdr.last_index = 0;
        inodes[0]->hdr.coveredNodes++;
    }
    while(newlevel > 0) {
        {
            std::unique_lock<std::shared_mutex> lock3(inodes[newlevel-1]->hdr.mtx);
            std::unique_lock<std::shared_mutex> lock4(header[newlevel-1]->hdr.mtx);
            inodes[newlevel-1]->hdr.next = header[newlevel-1]->hdr.next;
            header[newlevel-1]->hdr.next = inodes[newlevel-1]->getId();
        }
        newlevel--;
    }
    std::unique_lock<std::shared_mutex> lock5(level_lock);
    if(updatedlevel > level) {
        level = updatedlevel;
    }
    return true;
}

bool DramSkiplist::update(Key_t &oldKey, Key_t &newKey, Val_t &val)
{
    int currentHighestLevelIndex = -1;
    {
        std::shared_lock<std::shared_mutex> lock(level_lock); //to protect level
        currentHighestLevelIndex = level - 1;
    }
    Inode *target = header[currentHighestLevelIndex];
    for(int i = currentHighestLevelIndex; i >= 0; i--) {
        Inode *prev = nullptr;
        while(true) {
            std::shared_lock<std::shared_mutex> lock(target->hdr.mtx);
            if(target->hdr.next != tail[i]->getId() && oldKey > target->gps[target->hdr.last_index].key) {
                prev = target;
                Inode *temp = dramInodePool->at(target->hdr.next);
                lock.unlock();
                target = temp;
                std::shared_lock<std::shared_mutex> lock2(target->hdr.mtx);
                if(oldKey < target->gps[0].key && prev != nullptr) {
                    lock2.unlock();
                    target = prev;
                    break;
                }
            } else {
                lock.unlock();
                break;
            }
        }
        {
            std::shared_lock<std::shared_mutex> lock1(target->hdr.mtx);
            if(target->isHeader()) {
                if(i != 0) {
                    Inode *temp = dramInodePool->at(target->gps[0].value);
                    lock1.unlock();
                    target = temp;
                    continue;
                }
            }
        }
        {
            Val_t index = 0;
            std::unique_lock<std::shared_mutex> lock3(target->hdr.mtx);
            for(int j = 0; j <= target->hdr.last_index; j++) {
                if(oldKey >= target->gps[j].key) {
                    if(j + 1 <= target->hdr.last_index) {
                        if(oldKey < target->gps[j+1].key) {
                            index = j;
                            if(target->gps[j].key == oldKey) {
                                target->gps[j].key = newKey;
                            }
                            break;
                        }
                    } else { // if the key is the largest in the node
                        index = j;
                        if(target->gps[j].key == oldKey) {
                            target->gps[j].key = newKey;
                        }
                        break;
                    }
                }
            }
            if(i != 0) {
                Inode *temp = dramInodePool->at(target->gps[index].value);
                lock3.unlock();
                target = temp;
            }
        }
    }
    return true;
}

void DramSkiplist::getPivotNodesForInsert(Key_t key, Inode* updates[])
{
    int currentHighestLevelIndex = -1;
    {
        std::shared_lock<std::shared_mutex> lock(level_lock);
        currentHighestLevelIndex = level - 1;
    }
    Inode *current = header[currentHighestLevelIndex];
    for(int i = currentHighestLevelIndex; i >= 0; i--) {
        Inode *prev = nullptr;
        while(true) {
            std::shared_lock<std::shared_mutex> lock(current->hdr.mtx);
            if(current->hdr.next != tail[i]->getId() && key > current->gps[current->hdr.last_index].key) {
                prev = current;
                Inode *temp = dramInodePool->at(current->hdr.next);
                lock.unlock();
                current = temp;
                std::shared_lock<std::shared_mutex> lock2(current->hdr.mtx);
                if(key < current->gps[0].key && prev != nullptr) {
                    lock2.unlock();
                    current = prev;
                    break;
                }
            } else {
                lock.unlock();
                break;
            }
        }
        updates[i] = current;
        {
            std::shared_lock<std::shared_mutex> lock1(current->hdr.mtx);
            if(current->isHeader()) {
                if(i != 0) {
                    Inode *temp = dramInodePool->at(current->gps[0].value);
                    lock1.unlock();
                    current = temp;
                    continue;
                }
            }
        }
        {
            Val_t index = 0;
            std::shared_lock<std::shared_mutex> lock3(current->hdr.mtx);
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
                Inode *temp = dramInodePool->at(current->gps[index].value);
                lock3.unlock();
                current = temp;
            }
        }
    }
 }

Inode* DramSkiplist::lookup(Key_t key, int &idx)
{
    std::shared_lock<std::shared_mutex> lock(level_lock);
    int currentHighestLevelIndex = level - 1;
    lock.unlock();
    Inode* current = header[currentHighestLevelIndex];
    for(int i = currentHighestLevelIndex; i >= 0; i--) {
        // no real index nodes between header and tail
        Inode* prev = nullptr;
        //search among the nodes in the current level
        while(true) {
            std::shared_lock<std::shared_mutex> lock(current->hdr.mtx);
            if(current->hdr.next != tail[i]->getId() && key > current->gps[current->hdr.last_index].key) {
                prev = current;
                Inode *temp = dramInodePool->at(current->hdr.next);
                lock.unlock();
                current = temp;
                std::shared_lock<std::shared_mutex> lock2(current->hdr.mtx);
                if(key < current->gps[0].key && prev != nullptr) {
                    lock2.unlock();
                    current = prev;
                    break;
                }
            } else {
                lock.unlock();
                break;
            }
        }
        {
            std::shared_lock<std::shared_mutex> lock1(current->hdr.mtx);
            if(current->isHeader()) { // if the current node is the header node, only happens when the key is the smallest
                if(i != 0) {
                    Inode *temp = dramInodePool->at(current->gps[0].value);
                    lock1.unlock();
                    current = temp;
                    continue;
                } else {
                    idx = -1;
                    return nullptr;
                }
            }
        }
        {
            std::shared_lock<std::shared_mutex> lock3(current->hdr.mtx);
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
                Inode *temp = dramInodePool->at(current->gps[idx].value);
                lock3.unlock();
                current = temp;
            }
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
    Key_t targetKey = std::numeric_limits<Key_t>::max();
    {
        std::shared_lock<std::shared_mutex> lock(targetVnode.hdr.mtx);
        targetKey = targetVnode.getMinKey();
    }
    int newlevel = generateRandomLevel();
    //updates stores the precious nodes of the inodes that targetKey should be inserted 
    getPivotNodesForInsert(targetKey, updates);

    {
        std::unique_lock<std::shared_mutex> lock(level_lock);
        if (newlevel > level) {
            for (int i = level; i < newlevel; i++) {
                updates[i] = header[i];
            }
            level = newlevel;
        }
    }

    Inode *prev_update = nullptr;
    int prev_pos = 0;

    for (int i = newlevel - 1; i >= 0; i--) {
        Inode *current_update = updates[i];
        Inode *current = nullptr;
        {
            std::unique_lock<std::shared_mutex> lock(current_update->hdr.mtx);
            if (targetKey >= current_update->gps[current_update->hdr.last_index].key) { // if the target key is largert than the mex key in the current node
                if (!current_update->isFull()) { // append the key to the last available position since its sorted
                    current_update->hdr.last_index++;
                    current_update->gps[current_update->hdr.last_index].key = targetKey;
                    if (i != newlevel - 1) {
                        std::unique_lock<std::shared_mutex> lock_prev(prev_update->hdr.mtx);
                        prev_update->gps[prev_pos].value = current_update->getId();
                        prev_update->hdr.coveredNodes++;
                    }
                    prev_update = current_update;
                    prev_pos = current_update->hdr.last_index;
                } else { // the current node is full, need to split it into two nodes, the target key will be in the new inode
                    current = dramInodePool->getNextNode();
                    {
                        std::unique_lock<std::shared_mutex> lock_current(current->hdr.mtx);
                        current->hdr.last_index++;
                        current->gps[current->hdr.last_index].key = targetKey;
                        current->hdr.next = current_update->hdr.next;
                        current_update->hdr.next = current->getId();
                        if (i != newlevel - 1) {
                            std::unique_lock<std::shared_mutex> lock_prev(prev_update->hdr.mtx);
                            prev_update->gps[prev_pos].value = current->getId();
                            prev_update->hdr.coveredNodes++;
                        } else {
                            std::unique_lock<std::shared_mutex> lock_updates(updates[i + 1]->hdr.mtx);
                            updates[i + 1]->hdr.coveredNodes++;
                        }
                        prev_update = current;
                        prev_pos = current->hdr.last_index;
                    }
                }
            } else if (targetKey >= current_update->gps[0].key && targetKey < current_update->gps[current_update->hdr.last_index].key) { //key is in the middle of the current node
                if (!current_update->isFull()) { // if the current is not full, find the insert position and insert the key
                    int pos = current_update->findInsertKeyPos(targetKey);
                    current_update->shift(pos);
                    current_update->hdr.last_index++;
                    current_update->gps[pos].key = targetKey;
                    if (i != newlevel - 1) {
                        std::unique_lock<std::shared_mutex> lock_prev(prev_update->hdr.mtx);
                        prev_update->gps[prev_pos].value = current_update->getId();
                        prev_update->hdr.coveredNodes++;
                    }
                    prev_update = current_update;
                    prev_pos = pos;
                } else {
                    current = dramInodePool->getNextNode();
                    {
                        std::unique_lock<std::shared_mutex> lock_current(current->hdr.mtx);
                        current->hdr.last_index++;
                        current_update->split(current); // split will move the last half of the current node to the new node
                        Inode *target = (targetKey < current->getMinKey()) ? current_update : current; // target node is the node that the key should be inserted
                        int pos = target->findInsertKeyPos(targetKey);
                        target->shift(pos);
                        target->hdr.last_index++;
                        target->gps[pos].key = targetKey;
                        if (i != newlevel - 1) {
                            std::unique_lock<std::shared_mutex> lock_prev(prev_update->hdr.mtx);
                            prev_update->gps[prev_pos].value = target->getId();
                            prev_update->hdr.coveredNodes++;
                        } else {
                            std::unique_lock<std::shared_mutex> lock_updates(updates[i + 1]->hdr.mtx);
                            updates[i + 1]->hdr.coveredNodes++;
                        }
                        prev_update = target;
                        prev_pos = pos;
                        current->hdr.next = current_update->hdr.next;
                        current_update->hdr.next = current->getId();
                    }
                }
            }
        }
    }
    {
        std::unique_lock<std::shared_mutex> lock_updates(prev_update->hdr.mtx);
        prev_update->gps[prev_pos].value = targetVnode.getId();
    }
    return ret;
}
