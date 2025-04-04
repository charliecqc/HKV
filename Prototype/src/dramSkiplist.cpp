#include "dramSkiplist.h"
#include "checkpoint.h"
#include <cassert>
#include <mutex>
#include <optional>
#define numNodesInPool 10000000

DramSkiplist::DramSkiplist(CkptLog *ckp_log, DramInodePool* pool)
{
    ckpt_log = ckp_log;
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
            ckpt_log->enq(*tail[i]);
            ckpt_log->enq(*header[i]);
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

bool DramSkiplist::insert(Vnode *targetVnode) 
{
    Key_t targetKey = std::numeric_limits<Key_t>::max();
    Inode* updates[MAX_LEVEL];
    {
        std::shared_lock<std::shared_mutex> lock(reinterpret_cast<Vnode *>(targetVnode)->hdr.mtx);
        targetKey = reinterpret_cast<Vnode *>(targetVnode)->getMinKey();
    }
    int newlevel = generateRandomLevel();
    {
        std::unique_lock<std::shared_mutex> lock(level_lock);
        if(newlevel > level) {
            
            level = newlevel;
        }
    }
    for(int i = 0; i < newlevel; i++) {
        updates[i] = header[i];
    }
    std::optional<std::unique_lock<std::shared_mutex> > lock_updates[MAX_LEVEL];
    std::optional<std::unique_lock<std::shared_mutex> > lock_updates_next[MAX_LEVEL];
    Inode *prev_update = nullptr; // the update node in the previous round
    Inode *next = nullptr;
    Inode *current_update = nullptr;
    //CheckpointVector checkVec[newlevel-1];
    for(int i = 0; i < newlevel; i++) {
        lock_updates[i].emplace(inode_locks[updates[i]->getId()]);
        current_update = updates[i]; // current previous node of the node to be inserted
        next = dramInodePool->getNextNode();
        lock_updates_next[i].emplace(inode_locks[next->getId()]);

        next->hdr.next = current_update->hdr.next;
        current_update->hdr.next = next->getId();

        ckpt_log->enq(*next);
        ckpt_log->enq(*current_update);

        current_update = next;
        if (i == 0) {
            current_update->insertAtPos(targetKey, targetVnode->getId(), 0);
        } else {
            current_update->insertAtPos(targetKey, prev_update->getId(), 0);
        }
        prev_update = current_update;

        ckpt_log->enq(*prev_update);

        if(i != 0 && lock_updates[i-1]) {
            lock_updates[i-1]->unlock();
            lock_updates[i-1].reset();
        }
        if(i != 0 && lock_updates_next[i-1]) {
            lock_updates_next[i-1]->unlock();
            lock_updates_next[i-1].reset();
        }
//        ckpq->push(&checkVec[i]);
    }
    if(lock_updates[newlevel-1]) {
        lock_updates[newlevel-1]->unlock();
        lock_updates[newlevel-1].reset();
    }
    if(lock_updates_next[newlevel-1]) {
        lock_updates_next[newlevel-1]->unlock();
        lock_updates_next[newlevel-1].reset();
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
    //CheckpointVector checkVec[currentHighestLevelIndex + 1];
    for(int i = currentHighestLevelIndex; i >= 0; i--) {
        while(true) {
            std::shared_lock<std::shared_mutex> lock(inode_locks[target->getId()]);
            Inode *next = dramInodePool->at(target->hdr.next);
            std::shared_lock<std::shared_mutex> lock_next(inode_locks[next->getId()]);
            if(!next->isTail() && oldKey >= next->getMinKey()) {
                target = next;
            } else {
                break;
            }
        }
        {
            std::unique_lock<std::shared_mutex> lock3(inode_locks[target->getId()]);
            int idx = target->findKeyPos(oldKey);
            if(target->gps[idx].key == oldKey) {
                target->gps[idx].key = newKey;
                ckpt_log->enq(*target);
            }   
            if(i != 0) {
                target = dramInodePool->at(target->gps[idx].value);
            }
        }
        //ckpq->push(&checkVec[i]);
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
                std::shared_lock<std::shared_mutex> lock_current(inode_locks[current->getId()]);
                Inode *next = dramInodePool->at(current->hdr.next);
                std::shared_lock<std::shared_mutex> lock_next(inode_locks[next->getId()]);
                if(!next->isTail() && key >= next->getMinKey()) {
                    current = next;
                } else {
                    update_target = current;
                    if(update_target->isTail()) {
                        std::cout << "tail" << std::endl;
                    }
                    break;
                }
            }
        }
        
        updates[i] = update_target;
        {
            std::shared_lock<std::shared_mutex> lock_current(inode_locks[current->getId()]);
            if(i != 0) {
                int pos = current->findKeyPos(key);
                if (current->isHeader() && pos > 0)
                    assert(false);
                Inode *temp = dramInodePool->at(current->gps[pos].value);
                assert(temp != nullptr);
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
        //search among the nodes in the current level
        while(true) {
            assert(current != nullptr);
            {
                std::shared_lock<std::shared_mutex> lock_current(inode_locks[current->getId()]);
                Inode *next = dramInodePool->at(current->hdr.next);
                std::shared_lock<std::shared_mutex> lock_next(inode_locks[next->getId()]);
                if(!next->isTail() && key >= next->getMinKey()) {
                    current = next;
                } else {
                    break;
                }
            }
        }
        {
            std::shared_lock<std::shared_mutex> lock_current(inode_locks[current->getId()]);
            assert(current->hdr.last_index >= 0);
            if(current->isHeader()) {
                if(i != 0) {
                    Inode *temp = dramInodePool->at(current->gps[0].value);
                    assert(temp != nullptr);
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
    std::optional<std::unique_lock<std::shared_mutex> > lock_updates[MAX_LEVEL];
    std::optional<std::unique_lock<std::shared_mutex> > lock_updates_next[MAX_LEVEL];
    int pos = -1;
    Inode *prev_update = nullptr; // the update node in the previous round
    Inode *next = nullptr;
    Inode *current_update = nullptr;
    //CheckpointVector checkVec[newlevel];

    for(int i = 0; i < newlevel; i++) {
        lock_updates[i].emplace(inode_locks[updates[i]->getId()]);
        current_update = updates[i]; // current previous node of the node to be inserted
        if(current_update->isFull() || current_update->isHeader()) {
            next = dramInodePool->getNextNode();
            lock_updates_next[i].emplace(inode_locks[next->getId()]);
            next->hdr.next = current_update->hdr.next;
            current_update->hdr.next = next->getId();

            ckpt_log->enq(*next);
            ckpt_log->enq(*current_update); 

            if(current_update->isHeader()) {
                current_update = next;
            } else { 
                current_update->split(next);
                assert(current_update->hdr.last_index != -1 && next->hdr.last_index != -1);
                current_update = (targetKey < next->getMinKey()) ? current_update : next;
            }
        }
        pos = current_update->findInsertKeyPos(targetKey);
        if (i == 0) {
            current_update->insertAtPos(targetKey, targetVnode.getId(), pos);
        } else {
            current_update->insertAtPos(targetKey, prev_update->getId(), pos);
        }

        ckpt_log->enq(*current_update);
        prev_update = current_update;
        if(i != 0 && lock_updates[i-1]) {
            lock_updates[i-1]->unlock();
            lock_updates[i-1].reset();
        }
        if(i != 0 && lock_updates_next[i-1]) {
            lock_updates_next[i-1]->unlock();
            lock_updates_next[i-1].reset();
        }
    //    ckpq->push(&checkVec[i]);
    }
    if(lock_updates[newlevel-1]) {
        lock_updates[newlevel-1]->unlock();
        lock_updates[newlevel-1].reset();
    }
    if(lock_updates_next[newlevel-1]) {
        lock_updates_next[newlevel-1]->unlock();
        lock_updates_next[newlevel-1].reset();
    }
    return true;
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
