#include "dramSkiplist.h"
#include "checkpoint.h"
#include <cassert>
#include <mutex>
#define numNodesInPool 10000000

DramSkiplist::DramSkiplist(CheckpointQueue *q, DramInodePool* pool)
{
    ckpq = q;
    dramInodePool = pool;
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
        ckp_entry *header_entry = new ckp_entry(header[i]);
        ckpq->push(header_entry);
        ckp_entry *tail_entry = new ckp_entry(tail[i]);
        ckpq->push(tail_entry);
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
    assert(inodes[newlevel - 1] != nullptr);
    //std::unique_lock<std::shared_mutex> lock(inodes[newlevel - 1]->hdr.mtx);
    inodes[newlevel - 1]->hdr.level = newlevel - 1;
    inodes[newlevel - 1]->hdr.next = std::numeric_limits<uint32_t>::max();
    inodes[newlevel - 1]->gps[0].key = key;
    //lock.unlock();
    for(int i = newlevel - 2; i >= 0; i--) {
        inodes[i] = dramInodePool->getNextNode();
        assert(inodes[i] != nullptr);
        inodes[i]->gps[0].key = key;
        inodes[i]->hdr.level = i;
        inodes[i]->hdr.next = std::numeric_limits<uint32_t>::max();
        inodes[i+1]->gps[0].value = inodes[i]->getId();
        inodes[i+1]->hdr.last_index = 0;
        inodes[i+1]->hdr.coveredNodes++;
    }
    {
       // std::unique_lock<std::shared_mutex> lock2(inodes[0]->hdr.mtx);
        ret = linkVnodeToInode(*inodes[0], 0, *reinterpret_cast<Vnode *>(val));
        if(ret == false) {
            return ret;
        }
        inodes[0]->hdr.last_index = 0;
        inodes[0]->hdr.coveredNodes++;
    }
    while(newlevel > 0) {
        {
            std::unique_lock<std::shared_mutex> lock4(header[newlevel-1]->hdr.mtx);
            //std::unique_lock<std::shared_mutex> lock3(inodes[newlevel-1]->hdr.mtx);
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
        while(true) {
            std::shared_lock<std::shared_mutex> lock(target->hdr.mtx);
            Inode *next = dramInodePool->at(target->hdr.next);
            std::shared_lock<std::shared_mutex> lock_next(next->hdr.mtx);
            if(next->getId() != tail[i]->getId() && oldKey >= next->getMinKey()) {
                target = next;
            } else {
                break;
            }
        }
        {
            Val_t index = 0;
            std::unique_lock<std::shared_mutex> lock3(target->hdr.mtx);
            int idx = target->findKeyPos(oldKey);
            if(target->gps[idx].key == oldKey) {
                target->gps[idx].key = newKey;
                ckp_entry *entry = new ckp_entry(target);
                ckpq->push(entry);
            }   
            if(i != 0) {
                target = dramInodePool->at(target->gps[idx].value);
            }
        }
    }
    return true;
}

void DramSkiplist::getPivotNodesForInsert(Key_t key, Inode *updates[])
{
    int currentHighestLevelIndex = -1;
    {
        std::shared_lock<std::shared_mutex> lock(level_lock);
        currentHighestLevelIndex = level - 1;
    }
    Inode *current = header[currentHighestLevelIndex]; 
    for(int i = currentHighestLevelIndex; i >= 0; i--) {
        Inode *update_target = nullptr;
        while(true) { // search horizontally to find the node in this level
            assert(current != nullptr);
            {
                std::shared_lock<std::shared_mutex> lock_current(current->hdr.mtx);
                Inode *next = dramInodePool->at(current->hdr.next);
                std::shared_lock<std::shared_mutex> lock_next(next->hdr.mtx);
                if(next->getId() != tail[i]->getId() && key >= next->getMinKey()) {
                    current = next;
                } else {
                    update_target = current->isHeader()? next : current;
                    break;
                }
            }
        }
        updates[i] = update_target;
        {
            std::shared_lock<std::shared_mutex> lock_current(current->hdr.mtx);
            if(current->isHeader()) {
                if(i != 0) { // if not the bottom level
                    Inode *temp = dramInodePool->at(current->gps[0].value);
                    assert(temp != nullptr);
                    current = temp;
                } 
            }else {
                if(i != 0) {
                    int pos = current->findKeyPos(key);
                    Inode *temp = dramInodePool->at(current->gps[pos].value);
                    assert(temp != nullptr);
                    current = temp;
                }
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
        //search among the nodes in the current level
        while(true) {
            assert(current != nullptr);
            {
                std::shared_lock<std::shared_mutex> lock_current(current->hdr.mtx);
                Inode *next = dramInodePool->at(current->hdr.next);
                std::shared_lock<std::shared_mutex> lock_next(next->hdr.mtx);
                if(next->getId() != tail[i]->getId() && key >= next->getMinKey()) {
                    current = next;
                    lock_current.unlock();
                    lock_next.unlock();
                } else {
                    break;
                }
            }
        }
        {
            std::shared_lock<std::shared_mutex> lock_current(current->hdr.mtx);
            if(current->isHeader()) {
                if(i != 0) {
                    Inode *temp = dramInodePool->at(current->gps[0].value);
                    assert(temp != nullptr);
                    lock_current.unlock();
                    current = temp;
                    continue;
                } else {
                    idx = -1;
                    return nullptr;
                }
            }else {
                idx = current->findKeyPos(key);
                if(i != 0) {
                    Inode *temp = dramInodePool->at(current->gps[idx].value);
                    assert(temp != nullptr);
                    lock_current.unlock();
                    current = temp;
                }
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


bool DramSkiplist::rebalanceInode(Inode &inode, Vnode &targetVnode)
{
    bool ret = false;
    Key_t targetKey = std::numeric_limits<Key_t>::max();
    Inode* updates[MAX_LEVEL];
    {
        std::shared_lock<std::shared_mutex> lock(targetVnode.hdr.mtx);
        targetKey = targetVnode.getMinKey();
    }
    int newlevel = generateRandomLevel();
    getPivotNodesForInsert(targetKey, updates);
    {
        std::unique_lock<std::shared_mutex> lock(level_lock);
        if(newlevel > level) {
            for(int i = level; i < newlevel; i++) {
                updates[i] = header[i];
            }
            level = newlevel;
        }
    }

    Inode *prev_update = nullptr; // the update node in the previous round
    int prev_pos = 0; // the position in the previous update node
    for(int i = newlevel - 1; i >= 0; i--) {
        Inode *current_update = updates[i];
        Inode *next = nullptr;
        {
            std::unique_lock<std::shared_mutex> lock(current_update->hdr.mtx);
            bool is_current_top = (i == newlevel - 1) ? true : false;
            if(!current_update->isFull()) {
                rebalanceInodeImp(current_update, prev_update, prev_pos, targetKey, is_current_top, lock);
                ckp_entry *entry = new ckp_entry(prev_update);
                ckpq->push(entry);
            }else {
                next = dramInodePool->getNextNode();
                {
                    std::unique_lock<std::shared_mutex> lock_next(next->hdr.mtx);
                    current_update->split(next);
                    Inode *target = (targetKey < next->getMinKey()) ? current_update : next;
                    rebalanceInodeImp(target, prev_update, prev_pos, targetKey, is_current_top, lock);
                    ckp_entry *entry1 = new ckp_entry(prev_update);
                    ckpq->push(entry1);
                    ckp_entry *entry2 = new ckp_entry(target);
                    ckpq->push(entry2);
                }
            }
        }
    }
    std::unique_lock<std::shared_mutex> lock_updates(prev_update->hdr.mtx);
    prev_update->gps[prev_pos].value = targetVnode.getId();
    ckp_entry *entry = new ckp_entry(prev_update);
    ckpq->push(entry);
    return ret;
}

void DramSkiplist::rebalanceInodeImp(Inode *target, Inode *&prev_target, int &prev_pos, Key_t targetKey, bool is_current_top, std::unique_lock<std::shared_mutex> &lock)
{
    int pos = target->findInsertKeyPos(targetKey);
    if(!is_current_top) {
        std::unique_lock<std::shared_mutex> lock_prev(prev_target->hdr.mtx);
        prev_target->shift(prev_pos);
        prev_target->hdr.last_index++;
        prev_target->gps[prev_pos].value = target->getId();
        prev_target->hdr.coveredNodes++;
    }
    prev_target = target;
    prev_pos = pos;
}

void DramSkiplist::setLevel(int level)
{
    std::unique_lock<std::shared_mutex> lock(level_lock);
    this->level = level;
}

int DramSkiplist::getLevel()
{
    std::shared_lock<std::shared_mutex> lock(level_lock);
    return level;
}

#if 0
void DramSkiplist::rebalanceInodeImp(Inode *target, Inode *&prev_target, int &prev_pos, Key_t targetKey, bool is_current_top, std::unique_lock<std::shared_mutex> &lock)
{
    int pos = target->findInsertKeyPos(targetKey);
    target->shift(pos);
    target->hdr.last_index++;
    target->gps[pos].key = targetKey;
    if(!is_current_top) {
        prev_target->gps[prev_pos].value = target->getId();
        prev_target->hdr.coveredNodes++;
    }
    prev_target = target;
    prev_pos = pos;
}
#endif
