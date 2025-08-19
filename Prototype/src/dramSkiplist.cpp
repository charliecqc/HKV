#include "dramSkiplist.h"
#include "checkpoint.h"
#include <cassert>
#include <mutex>
#include <optional>
#include <map>
#include <shared_mutex>

#define numNodesInPool 10000000

// 线程本地路标定义
thread_local Key_t  DramSkiplist::tls_pivot_key_  = std::numeric_limits<Key_t>::min();
thread_local Inode* DramSkiplist::tls_pivot_node_ = nullptr;
thread_local decltype(DramSkiplist::tls_pivot_set_) DramSkiplist::tls_pivot_set_{ {}, 0 };

namespace {
    std::atomic<uint64_t> g_lfi_start_node_verify_failures{0};

    struct LfiCounterPrinter {
        ~LfiCounterPrinter() {
            std::cout << "[统计] lookupForInsert start_node 验证失败次数: "
                      << g_lfi_start_node_verify_failures.load() << std::endl;
        }
    } g_lfi_counter_printer; // 程序结束时自动打印
}


void DramSkiplist::bump_epoch() {
    global_epoch.fetch_add(1, std::memory_order_relaxed);
}

Inode* DramSkiplist::tls_try_match(Key_t key, int& start_level) {
    auto cur_epoch = global_epoch.load(std::memory_order_relaxed);
    for (int i = 0; i < tls_pivot_set_.used; ++i) {
        auto &e = tls_pivot_set_.pivots[i];
        if (!e.node) continue;
        if (e.epoch != cur_epoch || e.fail_cnt >= 3) { e.node = nullptr; continue; }
        if (key >= e.min_key && key < e.upper_key) {
            start_level = e.node->hdr.level;
            return e.node;
        }
    }
    return nullptr;
}

void DramSkiplist::tls_record_pivot(Inode* node) {
    if (!node || node->isHeader()) return;
    Key_t min_k = node->getMinKey();
    Key_t upper = std::numeric_limits<Key_t>::max();
    Inode* nxt = dramInodePool->at(node->hdr.next);
    if (nxt && !nxt->isTail()) upper = nxt->getMinKey();

    // 已存在 → 刷新
    for (int i = 0; i < tls_pivot_set_.used; ++i) {
        auto &e = tls_pivot_set_.pivots[i];
        if (e.node && e.min_key == min_k) {
            e.upper_key = upper;
            e.fail_cnt = 0;
            e.epoch = global_epoch.load(std::memory_order_relaxed);
            e.node = node;
            return;
        }
    }
    // 未满
    if (tls_pivot_set_.used < 3) {
        auto &e = tls_pivot_set_.pivots[tls_pivot_set_.used++];
        e = { node, min_k, upper, global_epoch.load(std::memory_order_relaxed), 0 };
        return;
    }
    // 选择 victim：fail_cnt 最大，其次区间更宽
    int victim = 0;
    auto span = [&](int i){ return (uint64_t)tls_pivot_set_.pivots[i].upper_key - tls_pivot_set_.pivots[i].min_key; };
    for (int i = 1; i < 3; ++i) {
        auto &best = tls_pivot_set_.pivots[victim];
        auto &cand = tls_pivot_set_.pivots[i];
        if (cand.fail_cnt > best.fail_cnt ||
           (cand.fail_cnt == best.fail_cnt && span(i) > span(victim))) {
            victim = i;
        }
    }
    tls_pivot_set_.pivots[victim] = { node, min_k, upper, global_epoch.load(std::memory_order_relaxed), 0 };
}

void DramSkiplist::tls_mark_fail(Key_t min_key) {
    for (int i = 0; i < tls_pivot_set_.used; ++i) {
        auto &e = tls_pivot_set_.pivots[i];
        if (e.node && e.min_key == min_key) {
            if (++e.fail_cnt >= 3) e.node = nullptr;
            return;
        }
    }
}

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
            assert(header[i]->hdr.next != 0);
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
            assert(tail[i]->hdr.next != 0);
            tail[i]->hdr.level = i;
        }
        for(int i = 0; i < MAX_LEVEL; i++) {
            header[i]->hdr.next = tail[i]->getId();
            assert(header[i]->hdr.next != 0);

            // **修改：使用新的构造函数，并正确设置初始 covered_nodes**
            // header 的 GP 指向下一层，初始覆盖数为1（或0，如果它是最底层）
            header[i]->gps[0].covered_nodes = (i > 0) ? 1 : 0;
            dram_log_entry_t *header_entry = new dram_log_entry_t(header[i]->getId(), header[i]->hdr.last_index, header[i]->hdr.next, header[i]->hdr.level);
            header_entry->setKeyVal(0, header[i]->gps[0].key, header[i]->gps[0].value, header[i]->gps[0].covered_nodes);

            // tail 的 GP 不覆盖任何东西
            tail[i]->gps[0].covered_nodes = 0;
            dram_log_entry_t *tail_entry = new dram_log_entry_t(tail[i]->getId(), tail[i]->hdr.last_index, tail[i]->hdr.next, tail[i]->hdr.level);
            tail_entry->setKeyVal(0, tail[i]->gps[0].key, tail[i]->gps[0].value, tail[i]->gps[0].covered_nodes);

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
        BloomFilter *bloom = &valueList->bf[targetVnode->hdr.id];
        std::shared_lock<std::shared_mutex> lock(bloom->vnode_mtx);
        targetKey = reinterpret_cast<Vnode *>(targetVnode)->getMinKey();
    }
    int newlevel = generateRandomLevel();
    bool level_grew = false;          // 新增：记录是否提升层数
    {
        std::shared_lock<std::shared_mutex> read_lock(level_lock);
        if (newlevel > level) {
            read_lock.unlock();
            std::unique_lock<std::shared_mutex> write_lock(level_lock);
            if (newlevel > level) {
                level = newlevel;
                level_grew = true;
                bump_epoch();         // 1) 层数增长立即推进 epoch（全局拓扑变化）
            }
        }
    }

    // collecting predecessors...
    std::vector<Inode *> predecessors;
    for (int i = 0; i < newlevel; i++) {
        predecessors.push_back(header[i]);
        updates[i] = header[i];
    }
    std::sort(predecessors.begin(), predecessors.end(),
              [](Inode *a, Inode *b){ return a->hdr.level > b->hdr.level; });

    std::map<Inode *, size_t> node_to_lock_index;
    for (size_t k = 0; k < predecessors.size(); k++) {
        node_to_lock_index[predecessors[k]] = k;
    }

    std::vector<std::unique_lock<std::shared_mutex>> acquired_locks;
    for (Inode *update: predecessors) {
        acquired_locks.emplace_back(inode_locks[update->getId()]);
        Inode *next = dramInodePool->at(update->hdr.next);
        if (targetKey >= next->getMinKey()) {
            return false; // 失败路径：未完成结构修改，不推进 epoch
        }
    }

    std::vector<Inode *> new_nodes(newlevel);
    for (int i = 0; i < newlevel; i++) new_nodes[i] = dramInodePool->getNextNode();

    bool tower_linked = false; // 新增：标记整塔是否成功链接
    for (int i = 0; i < newlevel; i++) {
        Inode *current_update = updates[i];
        Inode *next = new_nodes[i];

        next->hdr.next = current_update->hdr.next;
        current_update->hdr.next = next->getId();
        next->hdr.level = current_update->hdr.level;

        if (i == 0) 
            next->insertAtPos(targetKey, targetVnode->getId(), 0, 1);
        else        
            next->insertAtPos(targetKey, new_nodes[i-1]->getId(), 0, 1);

        dram_log_entry_t *next_entry = create_log_entry(next);
        dram_log_entry_t *cur_entry  = create_log_entry(current_update);
        ckpt_log->enq(next_entry);
        ckpt_log->enq(cur_entry);

        auto it = node_to_lock_index.find(current_update);
        if (it != node_to_lock_index.end()) {
            size_t index = it->second;
            if (index < acquired_locks.size() && acquired_locks[index].owns_lock()) {
                acquired_locks[index].unlock();
            }
        }
    }
    tower_linked = true;

    // 2) 塔成功插入（无论是否提升层数）再推进一次 epoch
    if (tower_linked && !level_grew) {
        // 若上面因层数提升已 bump_epoch，这里可选择跳过；保持条件可避免双重推进
        bump_epoch();
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
                // **修改：在节点更新后，使用辅助函数创建日志**
                target->updateKeyVal(newKey,idx);
                dram_log_entry_t *entry = create_log_entry(target);
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
    int start_level = -1;
    Inode* current = nullptr;
    bool used_cache = false;
    int current_total_level = 0; // 用于缓存填充

    // **步骤 1: 尝试从缓存获取起点**
    current = find_start_node_from_cache_shards(key, start_level);

    // **步骤 2: 如果缓存未命中，则执行完整查找**
    if (current == nullptr) {
        std::shared_lock<std::shared_mutex> lock(level_lock);
        current_total_level = level; // 获取当前总层数
        lock.unlock();
        start_level = current_total_level - 1;
        current = header[start_level];
    }

    // **步骤 3: 从起点开始向下遍历**
    for(int i = start_level; i >= 0; i--) {
        // 水平查找逻辑 (保持不变)
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

        // 垂直下降逻辑 (保持不变)
        if (i > 0) {
            std::shared_lock<std::shared_mutex> lock_current(inode_locks[current->getId()]);
            int temp_idx = current->findKeyPos(key);
            uint32_t next_level_node_id = current->gps[temp_idx].value;
            current = dramInodePool->at(next_level_node_id);
            assert(current != nullptr);
        }
    }

    // **步骤 4: 填充缓存并返回结果**
    // 此时, 'current' 是在最底层找到的目标 Inode
    populate_cache_shards(key, current, current_total_level);

    std::shared_lock<std::shared_mutex> lock_current(inode_locks[current->getId()]);
    if(current->isHeader()) {
        idx = -1;
        return nullptr;
    }
    assert(current->hdr.last_index >= 0);
    idx = current->findKeyPos(key);
    return current;
}

Inode *DramSkiplist::lookupForInsert(Key_t key, Inode * &current, int currentHighestLevelIndex, std::shared_lock<std::shared_mutex> &current_lock, int &idx, std::vector<Inode *> &updates)
{
    int start_level = -1;
    bool cache_hit_and_verified = false;
    int current_total_level = currentHighestLevelIndex + 1; // 保存总层数
    Inode *temp_start = current;
    //cout << "start header is : " << temp_start->getId() << " for key : " << key << endl;

    // **步骤 1: 尝试从缓存获取起点**
    Inode *start_node = find_start_node_from_cache_shards(key, start_level);

    if (start_node != nullptr) {
        std::shared_lock<std::shared_mutex> lock_start(inode_locks[start_node->getId()]);
        
        // === 修改开始 ===
        Key_t lower_bound = start_node->getMinKey();
        Key_t upper_bound = get_node_upper_bound(start_node);

        if (key >= lower_bound && key < upper_bound) {
            // 验证成功！
            idx = start_node->findKeyPos(key); // findKeyPos 内部会处理边界
            cache_hit_and_verified = true;
            current = start_node;
            start_level = start_node->hdr.level;
            // 在验证成功后，也应该记录/刷新 pivot，因为它是一个高质量的缓存项
            tls_record_pivot(start_node); 
            current_lock.unlock();
            current_lock = std::move(lock_start);
        } else {
        // === 修改结束 ===
            // 验证失败：统计 + 使 TLS 标记失效
            g_lfi_start_node_verify_failures.fetch_add(1, std::memory_order_relaxed);
            tls_mark_fail(start_node->getMinKey());
            current = temp_start;
        }
    }

    // **步骤 3: 如果缓存未命中，则执行完整查找**
    if (!cache_hit_and_verified) {
        start_level = currentHighestLevelIndex;
    }
    //cout << "Starting lookup for key: " << key << " at level: " << start_level << endl;
    for(int i = start_level; i >= 0; i--) {
        // no real index nodes between header and tail
        //search among the nodes in the current level
        while(true) {
            assert(current != nullptr);
            {
                Inode *next = dramInodePool->at(current->hdr.next);
                std::shared_lock<std::shared_mutex> next_horizental_lock(inode_locks[next->getId()]);
                //cout << "current: " << current->getId() << ", next: " << next->getId() << " at level: " << i << " for key: "<< key << endl;
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
            //cout << "Header node, next level node ID: " << next_level_node_id << endl;
        }else {
            int temp_idx = current->findKeyPos(key);
            next_level_node_id = current->gps[temp_idx].value;
            //cout << "Non-header node, next level node ID: " << next_level_node_id << endl;
        }

        Inode *temp = dramInodePool->at(next_level_node_id);
        assert(temp != nullptr);

        //lock passing for the next level node
        std::shared_lock<std::shared_mutex> next_vertical_lock(inode_locks[temp->getId()]);
        updates.push_back(current);
        current = temp;
        current_lock.unlock();
        current_lock = std::move(next_vertical_lock);
    }

    // at this point, current is the node in the last level
    if(current->isHeader()) { //if the current node is a header node, it means no valid data exists
        idx = -1;
        return nullptr;
    }

    // **步骤 5: 返回结果并填充缓存**
    populate_cache_shards(key, current, current_total_level);

    assert(current->hdr.last_index >= 0);
    idx = current->findKeyPos(key);
    return current;
}

Inode *DramSkiplist::lookup(Key_t key, Inode *current, int currentHighestLevelIndex, std::shared_lock<std::shared_mutex> &current_lock, int &idx)
{
     int start_level = -1;
    bool cache_hit_and_verified = false;
    int current_total_level = currentHighestLevelIndex + 1; // 保存总层数

    // **步骤 1: 尝试从缓存获取起点**
    Inode *start_node = find_start_node_from_cache_shards(key, start_level);

    // **步骤 2: 如果缓存命中，验证并返回结果
    if (start_node != nullptr) {
        std::shared_lock<std::shared_mutex> lock_start(inode_locks[start_node->getId()]);
        // 修正：用 start_node 校验
        int temp_idx = start_node->findKeyPos(key);
        if (key >= start_node->gps[0].key && key <= start_node->gps[start_node->hdr.last_index].key) {
            idx = temp_idx;
            cache_hit_and_verified = true;
        }
        current = start_node;
        current_lock.unlock();
        current_lock = std::move(lock_start);
    }

    // **步骤 3: 如果缓存未命中，则执行完整查找**
    if (!cache_hit_and_verified) {
        start_level = currentHighestLevelIndex;
    }

    for(int i = start_level; i >= 0; i--) {
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

    // **步骤 5: 填充缓存并返回结果**
    // **修改：传递当前总层数给 populate_cache**
    populate_cache_shards(key, current, current_total_level);

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

//return 0 if no rebalance is needed, return 1 if the target node is split, return 2 if the parent node is split
int DramSkiplist::fastRebalance(Inode* &inode, Inode* &parent_inode_hint) 
{
    int ret = 0;
    std::vector<std::unique_ptr<dram_log_entry_t>> log_entries;
    log_entries.reserve(4);

    Inode *next_node = dramInodePool->getNextNode();
    if (!next_node) return 0;
    next_node->hdr.level = inode->hdr.level;

    bool did_split_child   = false; // 新增：记录子节点是否分裂
    bool created_parent    = false; // 新增：上层是否创建新父节点
    bool parent_gp_changed = false; // 新增：父节点是否插入/激活了新 GP（结构变化）

    while (true) {
        Inode* candidate_parent = nullptr;
        Inode* candidate_next = nullptr;
        Inode* header_above = nullptr;

        if (parent_inode_hint != nullptr) {
            Key_t child_min_key = inode->getMinKey();
            Inode* current_parent = parent_inode_hint;
            while (true) {
                Inode* next_parent = dramInodePool->at(current_parent->hdr.next);
                if (isTail(next_parent->getId()) || child_min_key < next_parent->getMinKey()) {
                    candidate_parent = current_parent;
                    candidate_next = next_parent;
                    break;
                }
                current_parent = next_parent;
            }
        } else if (inode->hdr.level < MAX_LEVEL - 1) {
            header_above = getHeader(inode->hdr.level + 1);
        }

        std::vector<Inode*> nodes_to_lock;
        nodes_to_lock.push_back(inode);
        nodes_to_lock.push_back(next_node);
        if (candidate_parent) {
            nodes_to_lock.push_back(candidate_parent);
            if (candidate_next && !isTail(candidate_next->getId()))
                nodes_to_lock.push_back(candidate_next);
        }
        if (header_above) nodes_to_lock.push_back(header_above);

        std::vector<std::unique_lock<std::shared_mutex>> acquired_locks;
        acquireLocksInOrder(nodes_to_lock, acquired_locks);

        try {
            if (inode->hdr.last_index < fanout / 2 - 1) {
                return 0; // 无需重平衡：未改变结构，不 bump_epoch
            }

            Inode* verified_parent = nullptr;
            if (candidate_parent) {
                if (dramInodePool->at(candidate_parent->hdr.next) != candidate_next) {
                    continue; // 重试，不 bump
                }
                verified_parent = candidate_parent;
            }

            if (!verified_parent && header_above) {
                if (isTail(header_above->hdr.next)) {
                    verified_parent = dramInodePool->getNextNode();
                    if (!verified_parent) return 0;
                    verified_parent->hdr.level = inode->hdr.level + 1;
                    verified_parent->hdr.next  = header_above->hdr.next;
                    header_above->hdr.next     = verified_parent->getId();
                    verified_parent->insertAtPos(inode->getMinKey(), inode->getId(), 0, 1);

                    increaseLevel();          // 提升全局层数
                    created_parent = true;     // 标记结构变化
                    log_entries.emplace_back(create_log_entry(verified_parent));
                    log_entries.emplace_back(create_log_entry(header_above));
                } else {
                    verified_parent = dramInodePool->at(header_above->hdr.next);
                }
            }

            // 子节点分裂
            next_node->hdr.next = inode->hdr.next;
            inode->hdr.next = next_node->getId();
            inode->split(next_node);
            did_split_child = true;

            Key_t new_min_key = next_node->getMinKey();

            if (verified_parent) {
                while (true) {
                    Inode* next_parent_node = dramInodePool->at(verified_parent->hdr.next);
                    if (isTail(next_parent_node->getId()) || new_min_key < next_parent_node->getMinKey()) {
                        break;
                    }
                    if (next_parent_node != candidate_next) {
                        verified_parent = nullptr;
                        break;
                    }
                    verified_parent = next_parent_node;
                    candidate_next  = dramInodePool->at(verified_parent->hdr.next);
                }
                if (!verified_parent) {
                    continue; // 父链验证失败重试
                }

                if (verified_parent->isFull()) {
                    ret = 2;
                } else {
                    int pos = verified_parent->findKeyPos(inode->getMinKey());
                    if (verified_parent->isUnbalanced(pos)) {
                        int temp_pos = -1;
                        if (verified_parent->activateGP(new_min_key, next_node->getId(), temp_pos, 1)) {
                            log_entries.emplace_back(create_log_entry(verified_parent));
                            recordInodeRelation(next_node, verified_parent);
                            parent_gp_changed = true;
                            ret = 1;
                        } else {
                            ret = 2;
                        }
                    } else {
                        verified_parent->gps[pos].covered_nodes++;
                        recordInodeRelation(next_node, verified_parent);
                        parent_gp_changed = true; // 覆盖数变化也改变区间边界
                        ret = 1;
                    }
                }
            } else {
                ret = 1;
            }

            log_entries.emplace_back(create_log_entry(inode));
            log_entries.emplace_back(create_log_entry(next_node));
            parent_inode_hint = verified_parent;
            break;

        } catch (...) {
            return 0;
        }
    }

    for (auto &e : log_entries) ckpt_log->enq(e.release());

    // 统一：只要发生任何结构性修改就 bump_epoch
    if (created_parent) {
        bump_epoch();
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
            // **修改：为 insertAtPos 提供第四个参数 (initial_covered_nodes)**
            current_update->insertAtPos(targetKey, targetVnode.getId(), pos, 1);
            target_lock.unlock();
       } else {
            // **修改：为 insertAtPos 提供第四个参数**
            // 新GP指向一个Inode，其初始负载是该Inode的大小
            current_update->insertAtPos(prev_update->getMinKey(), prev_update->getId(), pos, prev_update->hdr.last_index + 1);
       }

       // **修改：移除错误的 lambda，直接调用 this->create_log_entry**
       if(need_spilt) {
            log_entries.push_back(std::unique_ptr<dram_log_entry_t>(this->create_log_entry(prev)));
            log_entries.push_back(std::unique_ptr<dram_log_entry_t>(this->create_log_entry(next_node)));
       } else {
            log_entries.push_back(std::unique_ptr<dram_log_entry_t>(this->create_log_entry(prev)));
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
   
    auto entry = new dram_log_entry_t(inode->getId(), inode->hdr.last_index, inode->hdr.next, inode->hdr.level);
    for(int j = 0; j <= inode->hdr.last_index; j++) {
        entry->setKeyVal(j, inode->gps[j].key, inode->gps[j].value, inode->gps[j].covered_nodes);
    }
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

void DramSkiplist::acquireLocksInOrder(std::vector<Inode*>& nodes, std::vector<std::unique_lock<std::shared_mutex>>& locks) 
{
    // 1. 按 level 从大到小排序，level 相同 先锁定前驱节点 
    std::sort(nodes.begin(), nodes.end(), [](Inode* a, Inode* b) {
        if (a == nullptr || b == nullptr) {
            return b == nullptr; // 将非空指针排在空指针前面
        }
        if (a->hdr.level != b->hdr.level) {
            return a->hdr.level > b->hdr.level; // level 大的在前
        }
        if (a->hdr.next == b->getId()) {
            return true; // a 是 b 的前驱，a 在前
        }
        if (b->hdr.next == a->getId()) {
            return false; // b 是 a 的前驱，b 在前
        }
        //return a->getId() < b->getId(); // level 相同，id 小的在前
    });
    // 2. 去除重复节点，防止对同一个互斥量加锁两次
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());

    // 3. 按照排好序的顺序依次加锁
    for (Inode* node : nodes) {
        if (node != nullptr) {
            locks.emplace_back(inode_locks[node->getId()]);
        }
    }
}

Inode *DramSkiplist::find_start_node_from_cache_shards(Key_t key, int& start_level) 
{
    if (Inode* n = tls_try_match(key, start_level)) return n;

    size_t s = shard_of(key);
    CacheShard& shard = cache_shards[s];
    std::shared_lock<std::shared_mutex> r(shard.mtx);
    if (shard.table.empty()) return nullptr;

    auto it = shard.table.upper_bound(key);
    if (it == shard.table.begin()) {
        return nullptr;
    }
    -- it;
    Inode* start_node = it->second;
    if (!start_node) return nullptr;
    start_level = start_node->hdr.level;
    tls_record_pivot(start_node);
    return start_node;
}

// **新增实现：从缓存中查找起点**
Inode* DramSkiplist::find_start_node_from_cache(Key_t key, int& start_level) {

    static thread_local Key_t   tl_pivot_key  = std::numeric_limits<Key_t>::min();
    static thread_local Inode*  tl_pivot_node = nullptr;

    if (tl_pivot_node && key >= tl_pivot_key) {
        start_level = tl_pivot_node->hdr.level;
        return tl_pivot_node;
    }

    std::shared_lock<std::shared_mutex> lock(cache_mutex);
    if (lookup_cache.empty()) {
        return nullptr;
    }

    // 在缓存中，我们想找到键值 <= key 的最佳起点。
    // upper_bound 会找到第一个键值 > key 的元素。
    auto it = lookup_cache.upper_bound(key);

    // 如果 it 是 begin()，说明缓存中所有的键都比要查找的 key 大，
    // 因此没有可用的起点。
    if (it == lookup_cache.begin()) {
        return nullptr;
    }

    // 回退一个位置，it 现在指向键值 <= key 的元素中键值最大的那个。
    // 这就是我们寻找的最佳查找起点。
    --it;
    
    Inode* start_node = it->second;
    if (start_node) {
        start_level = start_node->hdr.level;
        tl_pivot_key = it->first;
        tl_pivot_node = start_node; // 更新线程局部变量
        return start_node;
    }

    return nullptr;
}

// **修改：实现自适应的、结构感知的缓存填充**
void DramSkiplist::populate_cache(Key_t key, Inode* leaf_node, int current_total_level) {
    if (leaf_node == nullptr || leaf_node->hdr.level != 0 || leaf_node->isHeader() || current_total_level <= 1) {
        return;
    }

    // 采样：把写缓存频率降到约 1/64，显著降低争用
    static thread_local uint32_t pc_counter = 0;
    if ((++pc_counter & 63) != 0) {
        return;
    }

    int target_cache_level = std::max(1, current_total_level / 2);

    Inode* ancestor = leaf_node;
    Inode* node_to_cache = nullptr;

    // 向上追溯，尽量靠近目标层；找不到就取能到达的最高祖先
    while (ancestor != nullptr) {
        Inode* parent = getParentInode(ancestor);
        if (parent == nullptr) {
            node_to_cache = ancestor;
            break;
        }
        if (parent->hdr.level >= target_cache_level) {
            node_to_cache = parent;
            break;
        }
        ancestor = parent;
    }

    if (node_to_cache == nullptr) return;
    Key_t cache_key;
    // 非阻塞获取祖先最小键（失败就放弃本次写入）
    {
        std::shared_lock<std::shared_mutex> ancestor_lock(inode_locks[node_to_cache->getId()], std::try_to_lock);
        if (!ancestor_lock.owns_lock()) return;
        cache_key = node_to_cache->getMinKey();
    }

    // 非阻塞写缓存：抢不到锁就放弃，避免阻塞其他线程
    std::unique_lock<std::shared_mutex> lock(cache_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;

    auto it = lookup_cache.find(cache_key);
    if (it == lookup_cache.end() || it->second->hdr.level < node_to_cache->hdr.level) {
        lookup_cache[cache_key] = node_to_cache;
    }
}


void DramSkiplist::populate_cache_shards(Key_t key, Inode* leaf_node, int current_total_level) {
    if (!leaf_node || leaf_node->hdr.level != 0 || leaf_node->isHeader() || current_total_level <= 1) {
        return;
    }
    // 采样：约 1/64 次写，降低争用
    static thread_local uint32_t pc_counter = 0;
    if ((++pc_counter & 31) != 0) return;

    int target_cache_level = std::max(1, current_total_level / 2);

    Inode * ancestor = leaf_node;
    Inode * node_to_cache = nullptr;

    while(ancestor != nullptr) {
        Inode *parent = getParentInode(ancestor);
        if(parent == nullptr) {
            node_to_cache = ancestor;
            break;
        }
        if(parent->hdr.level >= target_cache_level) {
            node_to_cache = parent;
            break;
        }
        ancestor = parent;
    }
    if(node_to_cache == nullptr) return;

    Key_t cache_key;
    {
        std::shared_lock<std::shared_mutex> ancestor_lock(inode_locks[node_to_cache->getId()], std::try_to_lock);
        if(!ancestor_lock.owns_lock()) return; // 非阻塞获取祖先最小键
        cache_key = node_to_cache->getMinKey();
    }

    size_t s = shard_of(cache_key);
    CacheShard &shard = cache_shards[s];
    std::unique_lock<std::shared_mutex> lock(shard.mtx, std::try_to_lock);
    if(!lock.owns_lock()) return; // 非阻塞写缓存
    auto it = shard.table.find(cache_key);
    if(it == shard.table.end() || it->second->hdr.level < node_to_cache->hdr.level) {
        shard.table[cache_key] = node_to_cache; 
    }
}

void DramSkiplist::invalidate_tls_pivot() {
    tls_pivot_node_ = nullptr;
    tls_pivot_key_  = std::numeric_limits<Key_t>::min();
}

void DramSkiplist::update_tls_pivot(Key_t key, Inode* node) {
    tls_pivot_key_  = key;
    tls_pivot_node_ = node;
}

// 新增辅助函数：获取节点的真实管辖上界
Key_t DramSkiplist::get_node_upper_bound(Inode* node) {
    if (!node || node->isTail()) {
        return std::numeric_limits<Key_t>::max();
    }
    Inode* next_node = dramInodePool->at(node->hdr.next);
    if (!next_node || next_node->isTail()) {
        return std::numeric_limits<Key_t>::max();
    }
    return next_node->getMinKey();
}