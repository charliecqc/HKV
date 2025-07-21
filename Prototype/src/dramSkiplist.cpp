#include "dramSkiplist.h"
#include "checkpoint.h"
#include <cassert>
#include <mutex>
#include <optional>
#include <map>

#define numNodesInPool 10000000

DramSkiplist::DramSkiplist(CkptLog *ckp_log, DramInodePool* pool, ValueList *valuelist)
{
    ckpt_log = ckp_log;
    dramInodePool = pool;
    valueList = valuelist;
    if(dramInodePool->getCurrentIdx() == 0) {
        header[MAX_LEVEL - 1] = dramInodePool->getNextNode();
        header[MAX_LEVEL - 1]->gps[0].key = std::numeric_limits<Key_t>::min();
        header[MAX_LEVEL - 1]->gps[fanout/2 - 1].key = std::numeric_limits<Key_t>::min();
        header[MAX_LEVEL - 1]->hdr.last_index = 0;
        header[MAX_LEVEL - 1]->hdr.level = MAX_LEVEL - 1;
        for(int i = MAX_LEVEL - 2; i >= 0; i--) {
            header[i] = dramInodePool->getNextNode();
            header[i+1]->gps[0].value = header[i]->getId();
            header[i]->gps[0].key = std::numeric_limits<Key_t>::min();
            header[i]->hdr.next = std::numeric_limits<uint32_t>::max();
            header[i]->hdr.last_index = 0;
            header[i]->hdr.level = i;
        }

        tail[MAX_LEVEL - 1] = dramInodePool->getNextNode();
        tail[MAX_LEVEL - 1]->hdr.level = MAX_LEVEL - 1;
        for(int i = MAX_LEVEL - 2; i >= 0; i--) {
            tail[i] = dramInodePool->getNextNode();
            tail[i+1]->gps[0].value = tail[i]->getId();
            tail[i]->gps[0].key = std::numeric_limits<Key_t>::max();
            tail[i]->gps[fanout/2 - 1].key = std::numeric_limits<Key_t>::max();
            tail[i]->hdr.next = std::numeric_limits<uint32_t>::max();
            tail[i]->hdr.level = i;
        }
        for(int i = 0; i < MAX_LEVEL; i++) {
            header[i]->hdr.next = tail[i]->getId();

            dram_log_entry_t *header_entry = new dram_log_entry_t(header[i]->getId(), header[i]->hdr.coveredNodes, header[i]->hdr.last_index,header[i]->hdr.next, header[i]->hdr.level);
            header_entry->setKeyVal(0, header[i]->gps[0].key, header[i]->gps[0].value);
            header_entry->setCoveredNodes(header[i]->hdr.coveredNodes);
            header_entry->setLastIndex(header[i]->hdr.last_index);

            dram_log_entry_t *tail_entry = new dram_log_entry_t(tail[i]->getId(), tail[i]->hdr.coveredNodes,tail[i]->hdr.last_index,tail[i]->hdr.next, tail[i]->hdr.level);
            tail_entry->setKeyVal(0, tail[i]->gps[0].key, tail[i]->gps[0].value);
            tail_entry->setCoveredNodes(tail[i]->hdr.coveredNodes);
            tail_entry->setLastIndex(tail[i]->hdr.last_index);

            ckpt_log->enq(tail_entry);
            ckpt_log->enq(header_entry);
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

// add index for the newly inserted vnode
bool DramSkiplist::add(Vnode *targetVnode) 
{
    Key_t targetKey = std::numeric_limits<Key_t>::max();
    Inode* updates[MAX_LEVEL];
    {
        //std::shared_lock<std::shared_mutex> lock(reinterpret_cast<Vnode *>(targetVnode)->hdr.mtx);
        BloomFilter *bloom = &valueList->bf[targetVnode->hdr.id];
        std::shared_lock<std::shared_mutex> lock(bloom->vnode_mtx);
        targetKey = reinterpret_cast<Vnode *>(targetVnode)->getMinKey();
    }
    int newlevel = generateRandomLevel();
    {
        std::shared_lock<std::shared_mutex> read_lock(level_lock);
        if (newlevel <= level) {
            // if there is no need to increase the level, we can proceed with the existing level
        } else {
            read_lock.unlock();
            std::unique_lock<std::shared_mutex> write_lock(level_lock);
            // double check the level after acquiring the write lock
            if (newlevel > level) {
                level = newlevel;
            }
        }
    }

    //collecting all the predecessors of inserting value
    std::vector<Inode *> predecessors;

    for(int i = 0; i < newlevel; i++) {
        predecessors.push_back(header[i]);
        updates[i] = header[i];
    }
    // sort the predecessors based on their level
    std::sort(predecessors.begin(), predecessors.end(), [](Inode *a, Inode *b) {
        return a->hdr.level > b->hdr.level;
    });

    std::map<Inode *, size_t> node_to_lock_index;
    for(size_t k = 0; k < predecessors.size(); k++) {
        node_to_lock_index[predecessors[k]] = k;
    }

    std::vector<std::unique_lock<std::shared_mutex> > acquired_locks;
    // acuqire lock for all predecessors according to their level
    for(Inode *update: predecessors) {
        acquired_locks.emplace_back(inode_locks[update->getId()]);
        Inode *next = dramInodePool->at(update->hdr.next);
        if(targetKey >= next->getMinKey()) {
            //if the target key is greater than or equal to the next node's min key, we can skip this update
            return false;
        }
    }

    std::vector<Inode *> new_nodes(newlevel);
    for(int i = 0; i < newlevel; i ++) {
        new_nodes[i] = dramInodePool->getNextNode();
    }

    Inode *current_update = nullptr; // the update node in the previous round
    Inode *next = nullptr;
    for(int i = 0; i < newlevel; i++) {
        current_update = updates[i];
        next = new_nodes[i];

        //link the new node to the current update node
        next->hdr.next = current_update->hdr.next;
        current_update->hdr.next = next->getId();
        next->hdr.level = current_update->hdr.level;

        if(i == 0) {
            next->insertAtPos(targetKey, targetVnode->getId(), 0);
        } else {
            next->insertAtPos(targetKey, new_nodes[i-1]->getId(), 0);
        }

        dram_log_entry_t *next_entry = new dram_log_entry_t(next->getId(), next->hdr.coveredNodes, next->hdr.last_index, next->hdr.next, next->hdr.level);
        dram_log_entry_t *current_update_entry = new dram_log_entry_t(current_update->getId(), current_update->hdr.coveredNodes, current_update->hdr.last_index, current_update->hdr.next, current_update->hdr.level);
        next_entry->setKeyVal(0, next->gps[0].key, next->gps[0].value);
        next_entry->setCoveredNodes(next->hdr.coveredNodes);
        next_entry->setLastIndex(next->hdr.last_index);

        ckpt_log->enq(next_entry);
        ckpt_log->enq(current_update_entry);

        auto it = node_to_lock_index.find(current_update);
        if (it != node_to_lock_index.end()) {
            size_t index = it->second;
            if (index < acquired_locks.size() && acquired_locks[index].owns_lock()) {
                acquired_locks[index].unlock();
            }
        }
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
                dram_log_entry_t *entry = new dram_log_entry_t(target->getId(),target->hdr.coveredNodes, target->hdr.last_index, target->hdr.next, target->hdr.level);
                target->updateKeyVal(newKey,idx);
                entry->setKeyVal(idx, target->gps[idx].key, target->gps[idx].value);
                ckpt_log->enq(entry);
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

Inode *DramSkiplist::lookupForInsert(Key_t key, Inode *current, int currentHighestLevelIndex, std::shared_lock<std::shared_mutex> &current_lock, int &idx, std::vector<Inode *> &updates)
{
    for(int i = currentHighestLevelIndex; i >= 0; i--) {
        // no real index nodes between header and tail
        //search among the nodes in the current level
        while(true) {
            assert(current != nullptr);
            {
                Inode *next = dramInodePool->at(current->hdr.next);
                std::shared_lock<std::shared_mutex> next_horizental_lock(inode_locks[next->getId()]);
                if(!next->isTail() && key >= next->getMinKey()) {
                    assert(current->getMaxKey() <= next->getMinKey());
                    current = next;
                    current_lock.unlock();
                    current_lock = std::move(next_horizental_lock);
                } else {
                    // found the node in this level, escape the look and go to the next level
                    break;
                }
            }
        }
        //if already on the last level, return the current node
        if(i ==0) {
            break;
        }

        uint32_t next_level_node_id;
        if(current->isHeader()) {
            next_level_node_id = current->gps[0].value;            
        }else {
            int temp_idx = current->findKeyPos(key);
            next_level_node_id = current->gps[temp_idx].value;
        }

        Inode *temp = dramInodePool->at(next_level_node_id);
        assert(temp != nullptr);

        //lock passing for the next level node
        std::shared_lock<std::shared_mutex> next_vertical_lock(inode_locks[temp->getId()]);
        current = temp;
        updates.push_back(current);
        current_lock.unlock();
        current_lock = std::move(next_vertical_lock);
    }

    // at this point, current is the node in the last level
    if(current->isHeader()) { //if the current node is a header node, it means no valid data exists
        idx = -1;
        return nullptr;
    }

    assert(current->hdr.last_index >= 0);
    idx = current->findKeyPos(key);
    return current;
}

Inode *DramSkiplist::lookup(Key_t key, Inode *current, int currentHighestLevelIndex, std::shared_lock<std::shared_mutex> &current_lock, int &idx)
{
    for(int i = currentHighestLevelIndex; i >= 0; i--) {
        // no real index nodes between header and tail
        //search among the nodes in the current level
        while(true) {
            assert(current != nullptr);
            {
                Inode *next = dramInodePool->at(current->hdr.next);
                std::shared_lock<std::shared_mutex> next_horizental_lock(inode_locks[next->getId()]);
                if(!next->isTail() && key >= next->getMinKey()) {
                    current = next;
                    current_lock.unlock();
                    current_lock = std::move(next_horizental_lock);
                } else {
                    // found the node in this level, escape the look and go to the next level
                    break;
                }
            }
        }
        //if already on the last level, return the current node
        if(i ==0) {
            break;
        }

        uint32_t next_level_node_id;
        if(current->isHeader()) {
            next_level_node_id = current->gps[0].value;            
        }else {
            int temp_idx = current->findKeyPos(key);
            next_level_node_id = current->gps[temp_idx].value;
        }

        Inode *temp = dramInodePool->at(next_level_node_id);
        assert(temp != nullptr);

        //lock passing for the next level node
        std::shared_lock<std::shared_mutex> next_vertical_lock(inode_locks[temp->getId()]);
        current = temp;
        current_lock.unlock();
        current_lock = std::move(next_vertical_lock);
    }

    // at this point, current is the node in the last level
    //if current is a header node, nothing exists.
    if(current->isHeader()) {
        idx = -1;
        return nullptr;
    }

    assert(current->hdr.last_index >= 0);
    idx = current->findKeyPos(key);
    return current;
}

Inode* DramSkiplist::getHeader()
{
    return header[0];
}

Inode *DramSkiplist::getHeader(int level)
{
    if(level < 0 || level >= MAX_LEVEL) {
        std::cout << "Invalid level: " << level << std::endl;
        return nullptr;
    }
    return header[level];
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

//return 0 if no rebalance is needed, return 1 if the target node is split, return 2 if the parent node is split
int DramSkiplist::fastRebalance(Inode* &inode, Inode* &parent_inode) 
{
    int ret = 0;
    std::vector<std::unique_ptr<dram_log_entry_t>> log_entries;
    log_entries.reserve(3);

    Inode *next_node = dramInodePool->getNextNode();
    Inode *next_parent_inode = nullptr;
    next_node->hdr.level = inode->hdr.level;

    // 【优化】将分裂和更新逻辑放在一个独立的块中，以控制锁的作用域
    {
        // 1. 【优化】处理创建新父节点的特殊情况
        // 如果没有父节点，先创建它并插入到上一层。
        if (parent_inode == nullptr && inode->hdr.level < MAX_LEVEL - 1) {
            // This is a top-level node for its level, and we can create a new level above it.
            // We need to create a new parent node in the level above.
            Inode *header_above = getHeader(inode->hdr.level + 1);
            std::unique_lock<std::shared_mutex> lock_header(inode_locks[header_above->getId()]);

            // Double-check locking: another thread might have created the parent
            // between the initial check and acquiring the lock.
            // We assume if the header's next is not the tail, a parent might exist.
            // A full traversal would be needed for correctness in all cases, but
            // checking for an empty level is a common and efficient pattern.
            if (isTail(header_above->hdr.next)) {
            // The level above is empty, so we create the first node.
                parent_inode = dramInodePool->getNextNode();
                parent_inode->hdr.level = inode->hdr.level + 1;
            
            // Link the new parent into the level above
                parent_inode->hdr.next = header_above->hdr.next;
                header_above->hdr.next = parent_inode->getId();
                Key_t minKey = inode->getMinKey();
                int pos = parent_inode->findInsertKeyPos(minKey);
                parent_inode->insertAtPos(minKey, inode->getId(), pos);

                //increase the level of the whole skiplist
                increaseLevel();
            // Log the changes to the new parent and the header.
                log_entries.emplace_back(create_log_entry(parent_inode));
                log_entries.emplace_back(create_log_entry(header_above));
            
            // Record the relationship for future lookups.
                recordInodeRelation(inode, parent_inode);

            }else {
                // If the parent already exists, we can use it directly.
                parent_inode = dramInodePool->at(header_above->hdr.next);
            }
        }

        // 2. 【优化】执行公共的分裂和更新逻辑
        // 为所有相关节点加锁
        std::unique_lock<std::shared_mutex> lock_parent;
        std::unique_lock<std::shared_mutex> lock_parent_next;
        if (parent_inode) {
            lock_parent = std::unique_lock<std::shared_mutex>(inode_locks[parent_inode->getId()]);
        }
        std::unique_lock<std::shared_mutex> lock_target(inode_locks[inode->getId()]);
        std::unique_lock<std::shared_mutex> lock_next(inode_locks[next_node->getId()]);
        assert(inode->hdr.last_index == 13);

        // 执行分裂
        next_node->hdr.next = inode->hdr.next;
        inode->hdr.next = next_node->getId();
        inode->split(next_node);
        log_entries.emplace_back(create_log_entry(next_node));
        Key_t minKey = next_node->getMinKey();
        lock_next.unlock(); // 释放 next_node 的锁
        lock_target.unlock(); // 释放 inode 的锁

        while(!isTail(parent_inode->hdr.next)) {
            next_parent_inode = dramInodePool->at(parent_inode->hdr.next);
            std::unique_lock<std::shared_mutex> lock_next_parent(inode_locks[next_parent_inode->getId()]);
            if(minKey > parent_inode->getMaxKey()) {
                parent_inode = next_parent_inode;
                lock_parent.unlock();
                lock_parent = std::move(lock_next_parent);
            }
            else {
                break;
            }
        }
        // this is because next_node is already added in the chain
        parent_inode->hdr.coveredNodes++;

        std::unique_lock<std::shared_mutex> lock_target_again(inode_locks[inode->getId()]);
        std::unique_lock<std::shared_mutex> lock_next_again(inode_locks[next_node->getId()]);
        if(minKey != next_node->getMinKey()) { // other thrads might have updated the minKey
            return 0;
        }

        // 更新父节点（如果存在）
        if (parent_inode ){
            if(parent_inode->checkForActivateGP()) {
                int pos = -1;
                if (parent_inode->activateGP(minKey, next_node->getId(), pos)) {
                    log_entries.emplace_back(create_log_entry(parent_inode));
                    recordInodeRelation(next_node, parent_inode);
                    ret = 1; // 分裂成功
                } else {
                    assert(parent_inode != nullptr);
                    ret = 2; // 父节点也满了，需要重平衡
                }
            } else {
            // 没有父节点（分裂的是最高层级的根），分裂成功
                ret = 1;
            }
        // 在这个块的末尾，lock_target, lock_next, lock_parent 会被自动释放
        }
    }
    // 【关键】现在所有节点锁都已释放，再执行可能耗时的日志提交
    for (auto &entry : log_entries) {
        ckpt_log->enq(entry.release());
    }
    return ret;
}

int DramSkiplist::rebalanceIdx(Vnode &targetVnode, Key_t targetKey) 
{
    // targetVnode is still locked with shared lock
    int pos = -1;
    Inode *prev_update = nullptr; // the update node in the previous round
    Inode *next = nullptr;
    Inode *current_update = nullptr;
    Inode *prev = nullptr;// to point the one before the current node in split case 

    std::vector<std::unique_ptr<dram_log_entry_t> > log_entries;
    log_entries.reserve(MAX_LEVEL * 2);

    //no need to lock targetVnode as it is already locked in the caller function
    Inode *updates[MAX_LEVEL];
    int newlevel = generateRandomLevel();

    getPivotNodesForInsert(targetKey, updates);

    {
        std::shared_lock<std::shared_mutex> read_lock(level_lock);
        if (newlevel > level) {
            read_lock.unlock();
            std::unique_lock<std::shared_mutex> write_lock(level_lock);
            // double check the level after acquiring the write lock
            if (newlevel > level) {
                for(int i = level; i < newlevel; i++) {
                    updates[i] = header[i];
                }
                level = newlevel;
            }
        }
    }

    std::vector<Inode *> predecessors;
    std::vector<Inode *> new_nodes_map(newlevel, nullptr);
    for(int i = 0; i < newlevel; i++) {
        if(updates[i]->isFull() || updates[i]->isHeader()) {
            Inode *next_node = dramInodePool->getNextNode();
            next_node->hdr.level = updates[i]->hdr.level;
            new_nodes_map[i] = next_node;
        }
    }

    std::vector<Inode *> nodes_to_lock;
    nodes_to_lock.reserve(newlevel * 2);
    for(int i = 0; i < newlevel; i++) {
        nodes_to_lock.push_back(updates[i]);
        if(new_nodes_map[i] != nullptr) {
            nodes_to_lock.push_back(new_nodes_map[i]);
        }
    }

    std::sort(nodes_to_lock.begin(), nodes_to_lock.end(), [](Inode *a, Inode *b) {
        if(a->hdr.level != b->hdr.level)
            return a->hdr.level > b->hdr.level;
        return a->getId() < b->getId();
    });

    std::unordered_map<Inode *, size_t> node_to_lock_index;
    node_to_lock_index.reserve(nodes_to_lock.size());
    for(size_t k = 0; k < nodes_to_lock.size(); k++) {
        node_to_lock_index[nodes_to_lock[k]] = k;
    }

    //acquire locks for all predecessors according to their level
    std::vector<std::unique_lock<std::shared_mutex> > acquired_locks;
    acquired_locks.reserve(nodes_to_lock.size());
    for(Inode *node: nodes_to_lock) {
        acquired_locks.emplace_back(inode_locks[node->getId()]);
    }

    BloomFilter *bloom = &valueList->bf[targetVnode.hdr.id];
    std::shared_lock<std::shared_mutex> target_lock(bloom->vnode_mtx);
    if(targetKey != targetVnode.getMinKey()) {
        // targetVnode has already been updated with the new key
        return 2;
    }

    for(int i = 0; i < newlevel; i++) 
    {
       Inode *current_update = updates[i];
       Inode *prev = current_update; // to point the one before the current node in split case
       Inode *next_node = nullptr;

       bool need_spilt = current_update->isFull() || current_update->isHeader();
       if(need_spilt) {
            next_node = new_nodes_map[i];

            next_node->hdr.next = current_update->hdr.next;
            current_update->hdr.next = next_node->getId();

            Inode *node_to_release = nullptr;
            if(current_update->isHeader()) {
                node_to_release = current_update;
                current_update = next_node;
            } else { 
                current_update->split(next_node);
                assert(current_update->hdr.last_index != -1 && next_node->hdr.last_index != -1);
                if(targetKey >= next_node->getMinKey()) {
                    node_to_release = current_update;
                    current_update = next_node;
                }else {
                    node_to_release = next_node;
                }

                auto it = node_to_lock_index.find(node_to_release);
                if(it != node_to_lock_index.end()) {
                    size_t index = it->second;
                    if(index < acquired_locks.size() && acquired_locks[index].owns_lock()) {
                        acquired_locks[index].unlock(); 
                    }
                }
            }
       }

       pos = current_update->findInsertKeyPos(targetKey);
       if (i == 0) {
            current_update->insertAtPos(targetKey, targetVnode.getId(), pos);
            target_lock.unlock();
       } else {
            current_update->insertAtPos(prev_update->getMinKey(), prev_update->getId(), pos);
       }

       //log the state of the modified nodes
       auto create_log_entry = [](Inode *node) {
            auto entry = std::make_unique<dram_log_entry_t>(node->getId(), node->hdr.coveredNodes, node->hdr.last_index, node->hdr.next, node->hdr.level);
            for(int j = 0; j <= node->hdr.last_index; j++) {
                entry->setKeyVal(j, node->gps[j].key, node->gps[j].value);
            }
            return entry;
        };

        if(need_spilt) {
            log_entries.push_back(create_log_entry(prev));
            log_entries.push_back(create_log_entry(next_node));
        } else {
            log_entries.push_back(create_log_entry(prev));
        }

       auto it = node_to_lock_index.find(prev_update);
       if(it != node_to_lock_index.end()) {
            size_t index = it->second;
            if(index < acquired_locks.size() && acquired_locks[index].owns_lock()) {
                acquired_locks[index].unlock(); 
            }
        }

       prev_update = current_update;
    }

    for(auto &entry: log_entries) {
        ckpt_log->enq(entry.release());
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

dram_log_entry_t *DramSkiplist::create_log_entry(Inode *inode)
{
    auto entry = new dram_log_entry_t(inode->getId(), inode->hdr.coveredNodes, inode->hdr.last_index, inode->hdr.next, inode->hdr.level);
    for(int i = 0; i <= inode->hdr.last_index; i++) {
        entry->setKeyVal(i, inode->gps[i].key, inode->gps[i].value);
    }
    entry->setCoveredNodes(inode->hdr.coveredNodes);
    entry->setLastIndex(inode->hdr.last_index);
    return entry;
}

void DramSkiplist::recordInodeRelation(Inode* &child, Inode* &parent) {
    std::lock_guard<std::mutex> lock(inodeRelationMutex);
    childToParentMap[child] = parent;
}

Inode* DramSkiplist::getParentInode(Inode* &child) {
    std::lock_guard<std::mutex> lock(inodeRelationMutex);
    auto it = childToParentMap.find(child);
    if (it != childToParentMap.end()) {
        return it->second;
    }
    return nullptr; // 未找到
}

void DramSkiplist::removeInodeRelation(Inode* &child) {
    std::lock_guard<std::mutex> lock(inodeRelationMutex);
    childToParentMap.erase(child);
}

void DramSkiplist::printStats()
{
    for (int i = 0; i < level; ++i) {
        Inode* current = header[i];
        int count = 0;
        while (current->hdr.next != tail[i]->getId()) {
            current = dramInodePool->at(current->hdr.next);
            count++;
        }
        std::cout << "Level " << i << " has " << count << " inodes." << std::endl;
    }
}
