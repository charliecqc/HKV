#include "dramSkiplist.h"
#include "checkpoint.h"
#include <cassert>
#include <mutex>
#include <optional>
#define numNodesInPool 10000000

DramSkiplist::DramSkiplist(CheckpointQueue *q, DramInodePool* pool)
{
    ckpq = q;
    dramInodePool = pool;
    if(dramInodePool->getCurrentIdx() == 0) {
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
    }else {
        for(int i = MAX_LEVEL - 1; i >= 0; i--) {
            header[i] = dramInodePool->at(MAX_LEVEL-1-i);
            tail[i] = dramInodePool->at(2 * MAX_LEVEL - i - 1);
        }
    }
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
            //std::unique_lock<std::shared_mutex> lock4(header[newlevel-1]->hdr.mtx);
            //std::unique_lock<std::shared_mutex> lock3(inodes[newlevel-1]->hdr.mtx);
            std::unique_lock<std::shared_mutex> lock4(inode_locks[header[newlevel-1]->getId()]);
            std::unique_lock<std::shared_mutex> lock3(inode_locks[inodes[newlevel-1]->getId()]);
            inodes[newlevel-1]->hdr.next = header[newlevel-1]->hdr.next;
            header[newlevel-1]->hdr.next = inodes[newlevel-1]->getId();
            ckp_entry *entry = new ckp_entry(header[newlevel-1]);
            ckpq->push(entry);
            ckp_entry *entry2 = new ckp_entry(inodes[newlevel-1]);
            ckpq->push(entry2);
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
            //std::shared_lock<std::shared_mutex> lock(target->hdr.mtx);
            std::shared_lock<std::shared_mutex> lock(inode_locks[target->getId()]);
            Inode *next = dramInodePool->at(target->hdr.next);
            //std::shared_lock<std::shared_mutex> lock_next(next->hdr.mtx);
            std::shared_lock<std::shared_mutex> lock_next(inode_locks[next->getId()]);
            if(next->getId() != tail[i]->getId() && oldKey >= next->getMinKey()) {
                target = next;
            } else {
                break;
            }
        }
        {
            Val_t index = 0;
            //std::unique_lock<std::shared_mutex> lock3(target->hdr.mtx);
            std::unique_lock<std::shared_mutex> lock3(inode_locks[target->getId()]);
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
                //std::shared_lock<std::shared_mutex> lock_current(current->hdr.mtx);
                std::shared_lock<std::shared_mutex> lock_current(inode_locks[current->getId()]);
                Inode *next = dramInodePool->at(current->hdr.next);
                //std::shared_lock<std::shared_mutex> lock_next(next->hdr.mtx);
                std::shared_lock<std::shared_mutex> lock_next(inode_locks[next->getId()]);
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
            //std::shared_lock<std::shared_mutex> lock_current(current->hdr.mtx);
            std::shared_lock<std::shared_mutex> lock_current(inode_locks[current->getId()]);
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
                //std::shared_lock<std::shared_mutex> lock_current(current->hdr.mtx);
                std::shared_lock<std::shared_mutex> lock_current(inode_locks[current->getId()]);
                Inode *next = dramInodePool->at(current->hdr.next);
                //std::shared_lock<std::shared_mutex> lock_next(next->hdr.mtx);
                std::shared_lock<std::shared_mutex> lock_next(inode_locks[next->getId()]);
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
            //std::shared_lock<std::shared_mutex> lock_current(current->hdr.mtx);
            std::shared_lock<std::shared_mutex> lock_current(inode_locks[current->getId()]);
            assert(current->hdr.last_index >= 0);
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
#if 0
    Inode *prev_update = nullptr; // the update node in the previous round
    int prev_pos = 0; // the position in the previous update node
    for(int i = newlevel - 1; i >= 0; i--) {
        Inode *current_prev = updates[i]; // cureent previous node of the node to be inserted
        Inode *next = nullptr;
        {
            //std::unique_lock<std::shared_mutex> lock(current_update->hdr.mtx);
            std::unique_lock<std::shared_mutex> lock(inode_locks[current_prev->getId()]);
            bool is_current_top = (i == newlevel - 1) ? true : false;
            if(!current_prev->isFull() && !current_prev->isHeader()) {
                rebalanceInodeImp(current_prev, prev_update, prev_pos, targetKey, is_current_top);
                ckp_entry *entry = new ckp_entry(prev_update);
                ckpq->push(entry);
            }else {
                next = dramInodePool->getNextNode();
                next->hdr.next = current_prev->hdr.next;
                current_prev->hdr.next = next->getId();
                ckp_entry *entry1 = new ckp_entry(current_prev);
                ckpq->push(entry1);
                {
                    //std::unique_lock<std::shared_mutex> lock_next(next->hdr.mtx);
                    Inode *target = nullptr;
                    //std::unique_lock<std::shared_mutex> lock_next(inode_locks[next->getId()]);
                    if(current_prev->isHeader()) {
                        target = next; // we dont insert into header
                    }else {
                        current_prev->split(next);
                        assert(current_prev->hdr.last_index != -1 && next->hdr.last_index != -1);
                        target = (targetKey < next->getMinKey()) ? current_prev : next;
                    }
                    rebalanceInodeImp(target, prev_update, prev_pos, targetKey, is_current_top);
                    //ckp_entry *entry1 = new ckp_entry(prev_update);
                    //ckpq->push(entry1);
                    ckp_entry *entry2 = new ckp_entry(target);
                    ckpq->push(entry2);
                }
            }
        }
    }
    //std::unique_lock<std::shared_mutex> lock_updates(prev_update->hdr.mtx);
    std::unique_lock<std::shared_mutex> lock_updates(inode_locks[prev_update->getId()]);
    prev_update->shift(prev_pos);
    prev_update->hdr.last_index++;
    prev_update->gps[prev_pos].key = targetKey;
    prev_update->gps[prev_pos].value = targetVnode.getId();
    prev_update->hdr.coveredNodes++;
    ckp_entry *entry = new ckp_entry(prev_update);
    ckpq->push(entry);
   #endif 
    std::optional<std::unique_lock<std::shared_mutex> > lock_updates[newlevel];
    std::optional<std::unique_lock<std::shared_mutex> > lock_updates_next[newlevel];
    int pos = -1;
    Inode *prev_update = nullptr; // the update node in the previous round
    for(int i = newlevel - 1; i >= 0; i--) {
        lock_updates[i].emplace(inode_locks[updates[i]->getId()]);
        Inode *current_update = updates[i]; // cureent previous node of the node to be inserted
        Inode *next = nullptr;
        {
            if(current_update->isFull() || current_update->isHeader()) {
                next = dramInodePool->getNextNode();
                lock_updates_next[i].emplace(inode_locks[next->getId()]);
                next->hdr.next = current_update->hdr.next;
                current_update->hdr.next = next->getId();
                if(current_update->isHeader()) {
                    current_update = next;
                }else { 
                    current_update->split(next);
                    assert(current_update->hdr.last_index != -1 && next->hdr.last_index != -1);
                    current_update = (targetKey < next->getMinKey()) ? current_update : next;
                }
            }
        }
        if(i != newlevel - 1) {
            prev_update->insertAtPos(targetKey, current_update->getId(), pos);
        }
        if(i!= newlevel - 1 && lock_updates[i+1]) {
            lock_updates[i+1]->unlock();
            lock_updates[i+1].reset();
        }
        if(i!= newlevel - 1 && lock_updates_next[i+1]) {
            lock_updates_next[i+1]->unlock();
            lock_updates_next[i+1].reset();
        }
        pos = current_update->findInsertKeyPos(targetKey);
        prev_update = current_update;
    }
   #if 0
    std::optional<std::unique_lock<std::shared_mutex> > lock_updates[newlevel];
    std::optional<std::unique_lock<std::shared_mutex> > lock_updates_next[newlevel];
    int pos = -1;
    Inode *prev_update = nullptr; // the update node in the previous round
    Inode *current_update = updates[newlevel - 1];
    lock_updates[newlevel - 1].emplace(inode_locks[current_update->getId()]);
    if(current_update->isFull() || current_update->isHeader()) {
        Inode *next = dramInodePool->getNextNode();
        lock_updates_next[newlevel - 1].emplace(inode_locks[next->getId()]);
        next->hdr.next = current_update->hdr.next;
        current_update->hdr.next = next->getId();
        if(current_update->isHeader()) {
            current_update = next;
        }else { 
            current_update->split(next);
            assert(current_update->hdr.last_index != -1 && next->hdr.last_index != -1);
            current_update = (targetKey < next->getMinKey()) ? current_update : next;
        }
    }
    pos = current_update->findInsertKeyPos(targetKey);
    prev_update = current_update;

    for(int i = newlevel - 2; i >= 0; i--) {
        lock_updates[i].emplace(inode_locks[updates[i]->getId()]);
        current_update = updates[i]; // cureent previous node of the node to be inserted
        Inode *next = nullptr;
        {
            if(current_update->isFull() || current_update->isHeader()) {
                next = dramInodePool->getNextNode();
                lock_updates_next[i].emplace(inode_locks[next->getId()]);
                next->hdr.next = current_update->hdr.next;
                current_update->hdr.next = next->getId();
                if(current_update->isHeader()) {
                    current_update = next;
                }else { 
                    current_update->split(next);
                    assert(current_update->hdr.last_index != -1 && next->hdr.last_index != -1);
                    current_update = (targetKey < next->getMinKey()) ? current_update : next;
                }
            }
        }
        prev_update->insertAtPos(targetKey, current_update->getId(), pos);
        if(lock_updates[i+1]) {
            lock_updates[i+1]->unlock();
            lock_updates[i+1].reset();
        }
        if(lock_updates_next[i+1]) {
            lock_updates_next[i+1]->unlock();
            lock_updates_next[i+1].reset();
        }
        pos = current_update->findInsertKeyPos(targetKey);
        prev_update = current_update;
    }
    #endif
    assert(lock_updates[0]);
    prev_update->insertAtPos(targetKey, targetVnode.getId(), pos);
    if(lock_updates[0]) {
        lock_updates[0]->unlock();
        lock_updates[0].reset();
    }
    if(lock_updates_next[0]) {
        lock_updates_next[0]->unlock();
        lock_updates_next[0].reset();
    }
    ckp_entry *entry = new ckp_entry(prev_update);
    ckpq->push(entry);
    return ret;
}

void DramSkiplist::rebalanceInodeImp(Inode *cur_target, Inode *&prev_target, int &prev_pos, Key_t targetKey, bool is_current_top)
{
    int pos = cur_target->findInsertKeyPos(targetKey);
    if(!is_current_top) {
        //std::unique_lock<std::shared_mutex> lock_prev(prev_target->hdr.mtx);
        std::unique_lock<std::shared_mutex> lock_prev(inode_locks[prev_target->getId()]);
        prev_target->shift(prev_pos);
        prev_target->hdr.last_index++;
        prev_target->gps[prev_pos].key = targetKey;
        prev_target->gps[prev_pos].value = cur_target->getId();
        prev_target->hdr.coveredNodes++;
    }
    prev_target = cur_target;
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
