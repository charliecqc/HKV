#include "dramSkiplist.h"
#include "checkpoint.h"
#include <cassert>
#include <mutex>
#include <optional>
#include <map>
#include <shared_mutex>
#include <atomic>
#include <fstream>   // 新增

#define numNodesInPool 10000000

#ifndef ENABLE_CACHE_STATS
#define ENABLE_CACHE_STATS 1
#endif

#ifndef ENABLE_PARENT_RELATION
#define ENABLE_PARENT_RELATION 1   
#endif


#ifndef ENABLE_L2_SHARD_CACHE
#define ENABLE_L2_SHARD_CACHE 1
#endif

#ifndef L2_BACKFILL_SAMPLE_MASK
#define L2_BACKFILL_SAMPLE_MASK 15
#endif

#ifndef TLS_PIVOT_MAX
#define TLS_PIVOT_MAX 8
#endif

#if ENABLE_CACHE_STATS
#define DBG_CACHE 1
#endif

// 线程本地路标定义
thread_local Key_t  DramSkiplist::tls_pivot_key_  = std::numeric_limits<Key_t>::min();
thread_local Inode* DramSkiplist::tls_pivot_node_ = nullptr;
thread_local decltype(DramSkiplist::tls_pivot_set_) DramSkiplist::tls_pivot_set_{ {}, 0 };

namespace {
#ifdef DBG_CACHE
    // --- 新增：全面的缓存统计变量 ---
    alignas(64) std::atomic<uint64_t> g_total_cache_lookups{0};
    alignas(64) std::atomic<uint64_t> g_l1_hits{0};
    alignas(64) std::atomic<uint64_t> g_l1_misses{0};
    alignas(64) std::atomic<uint64_t> g_l2_hits{0};
    alignas(64) std::atomic<uint64_t> g_l2_misses{0};
    alignas(64) std::atomic<uint64_t> g_lfi_start_node_verify_failures{0};
#endif

    struct LfiCounterPrinter {
        ~LfiCounterPrinter() {
#ifdef DBG_CACHE
            uint64_t total_lookups = g_total_cache_lookups.load();
            uint64_t l1_hits = g_l1_hits.load();
            uint64_t l1_misses = g_l1_misses.load();
            uint64_t l2_hits = g_l2_hits.load();
            uint64_t l2_misses = g_l2_misses.load();
            uint64_t verify_failures = g_lfi_start_node_verify_failures.load();

            double l1_hit_rate = (total_lookups == 0) ? 0.0 : (double)l1_hits / total_lookups * 100.0;
            // L2 的查询总数等于 L1 的未命中数
            uint64_t l2_lookups = l1_misses;
            double l2_hit_rate = (l2_lookups == 0) ? 0.0 : (double)l2_hits / l2_lookups * 100.0;

            std::cout << "--- Cache & Verification Statistics ---" << std::endl;
            std::cout << "[统计] Total Fast Path Entries (总缓存查询): " << total_lookups << std::endl;
            std::cout << "-----------------------------------------" << std::endl;
            std::cout << "[统计] L1 (TLS) Hits: " << l1_hits << ", Misses: " << l1_misses 
                      << ", Hit Rate: " << l1_hit_rate << "%" << std::endl;
            std::cout << "[统计] L2 (Shard) Hits: " << l2_hits << ", Misses: " << l2_misses 
                      << ", Hit Rate: " << l2_hit_rate << "%" << std::endl;
            std::cout << "-----------------------------------------" << std::endl;
            std::cout << "[统计] Start Node Verification Failures: " << verify_failures << std::endl;
            std::cout << "[统计] Total Cache Miss (start_node不存在): " << l2_misses << std::endl;
            std::cout << "-----------------------------------------" << std::endl;
#endif
        }
    } g_lfi_counter_printer; // 程序结束时自动打印

    // ==== 新增：横向移动超阈值事件统计 ====

#ifndef ENABLE_SEARCH_STABILITY
#define ENABLE_SEARCH_STABILITY 0
#endif

//#define ENABLE_SEARCH_STABILITY 1

#if ENABLE_SEARCH_STABILITY
#define DBG_SEARCH_STABILITY 1
    struct HorizontalExcessEvent {
        Key_t    key;
        uint32_t level;
        uint32_t steps; // 实际横移次数
    };

    // 事件缓冲容量（可根据需要调整，避免占用过多内存）
    constexpr size_t HORIZONTAL_EVENT_CAPACITY = 1000000; // 最多记录50万条
    alignas(64) static HorizontalExcessEvent g_horizontal_events[HORIZONTAL_EVENT_CAPACITY];

    std::atomic<uint64_t> g_horizontal_event_write_idx{0};  // 已写入事件数(可能超过容量)
    std::atomic<uint64_t> g_horizontal_event_overflow{0};   // 超出容量丢弃计数
    alignas(64) std::atomic<uint64_t> g_horizontal_total{0}; // 总触发次数
    alignas(64) std::atomic<uint64_t> g_horizontal_by_level[MAX_LEVEL]; // 分层计数
    alignas(64) std::atomic<uint64_t> g_horizontal_steps_hist[64]; // 简单步数桶 (0..63, >=63 合并)


    struct HorizontalEventDumper {
        ~HorizontalEventDumper() {
            uint64_t total = g_horizontal_total.load(std::memory_order_relaxed);
            if (total == 0) {
                std::cout << "[统计] 无横移超阈值事件\n";
                return;
            }
            // 输出汇总
            std::cout << "[统计] 横向超阈值事件总次数: " << total
                      << " (记录上限=" << HORIZONTAL_EVENT_CAPACITY
                      << ", 丢弃=" << g_horizontal_event_overflow.load() << ")\n";
            for (int l = 0; l < MAX_LEVEL; ++l) {
                uint64_t c = g_horizontal_by_level[l].load(std::memory_order_relaxed);
                if (c)
                    std::cout << "  Level " << l << ": " << c << " 次\n";
            }
            std::cout << "[统计] 步数分布(steps -> count): ";
            for (int i = 0; i < 64; ++i) {
                uint64_t c = g_horizontal_steps_hist[i].load(std::memory_order_relaxed);
                if (c) {
                    if (i < 63)
                        std::cout << i << ":" << c << " ";
                    else
                        std::cout << ">=63:" << c << " ";
                }
            }
            std::cout << "\n";

            // 将事件写入文件
            std::ofstream ofs("horizontal_travel_events.log");
            if (ofs) {
                uint64_t cap = std::min<uint64_t>(g_horizontal_event_write_idx.load(), HORIZONTAL_EVENT_CAPACITY);
                ofs << "# key level steps\n";
                for (uint64_t i = 0; i < cap; ++i) {
                    const auto &e = g_horizontal_events[i];
                    ofs << e.key << " " << e.level << " " << e.steps << "\n";
                }
                if (g_horizontal_event_overflow.load() > 0) {
                    ofs << "# dropped " << g_horizontal_event_overflow.load()
                        << " events due to capacity limit\n";
                }
            } else {
                std::cout << "[警告] 无法写入 horizontal_travel_events.log\n";
            }
        }
    } g_horizontal_event_dumper;

    inline void record_horizontal_excess(Key_t key, int level, uint32_t steps) {
        g_horizontal_total.fetch_add(1, std::memory_order_relaxed);
        g_horizontal_by_level[level].fetch_add(1, std::memory_order_relaxed);
        uint32_t bucket = steps < 63 ? steps : 63;
        g_horizontal_steps_hist[bucket].fetch_add(1, std::memory_order_relaxed);

        uint64_t idx = g_horizontal_event_write_idx.fetch_add(1, std::memory_order_relaxed);
        if (idx < HORIZONTAL_EVENT_CAPACITY) {
            g_horizontal_events[idx] = HorizontalExcessEvent{ key, static_cast<uint32_t>(level), steps };
        } else {
            g_horizontal_event_overflow.fetch_add(1, std::memory_order_relaxed);
        }
    }
#endif
}



void DramSkiplist::bump_epoch() {
    global_epoch.fetch_add(1, std::memory_order_relaxed);
}

Inode* DramSkiplist::tls_try_match(Key_t key, int& start_level) {
    auto cur_epoch = global_epoch.load(std::memory_order_relaxed);
    for (int i = 0; i < tls_pivot_set_.used; ++i) {
        auto &e = tls_pivot_set_.pivots[i];
        if (!e.node) continue;
        if (e.epoch != cur_epoch || e.fail_cnt >= 5) { // 放宽寿命以便顺序场景更长复用
            e.node = nullptr;
            continue;
        }
        if (key >= e.min_key && key < e.upper_key) {
#ifdef DBG_CACHE
            g_l1_hits.fetch_add(1, std::memory_order_relaxed);
#endif
            e.hit_cnt++;
            // 提供起始层（即便当前只关注 lookupForInsert，这里仍标准化）
            start_level = e.node->hdr.level;
            // 简单 MRU：命中项前移
            if (i != 0) {
                auto hit = e;
                for (int k = i; k > 0; --k)
                    tls_pivot_set_.pivots[k] = tls_pivot_set_.pivots[k-1];
                tls_pivot_set_.pivots[0] = hit;
            }
            return e.node;
        }
    }
    return nullptr;
}

void DramSkiplist::tls_record_pivot(Inode* node) {
    if (!node || node->isHeader()) return;
    Key_t min_k = node->getMinKey();
    Key_t upper = get_node_upper_bound(node);
    uint32_t ep = global_epoch.load(std::memory_order_relaxed);

    for (int i = 0; i < tls_pivot_set_.used; ++i) {
        auto &e = tls_pivot_set_.pivots[i];
        if (e.node && e.min_key == min_k) {
            e.upper_key = upper;
            e.fail_cnt = 0;
            e.epoch = ep;
            // 不重置 hit_cnt，保持历史热度
            return;
        }
    }

    if (tls_pivot_set_.used < TLS_PIVOT_MAX) {
        tls_pivot_set_.pivots[tls_pivot_set_.used++] =
            TlsPivot{ node, min_k, upper, ep, 0, 1 };
        return;
    }

    auto span = [&](int i) {
        const auto &p = tls_pivot_set_.pivots[i];
        return (uint64_t)(p.upper_key - p.min_key);
    };
    int victim = 0;
    for (int i = 1; i < TLS_PIVOT_MAX; ++i) {   // 修改：使用 TLS_PIVOT_MAX
        auto &v = tls_pivot_set_.pivots[victim];
        auto &c = tls_pivot_set_.pivots[i];
        if (c.fail_cnt > v.fail_cnt ||
           (c.fail_cnt == v.fail_cnt && c.hit_cnt < v.hit_cnt) ||
           (c.fail_cnt == v.fail_cnt && c.hit_cnt == v.hit_cnt && span(i) < span(victim))) {
            victim = i;
        }
    }
    tls_pivot_set_.pivots[victim] = TlsPivot{ node, min_k, upper, ep, 0, 1 };
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
            header[i]->setParent(header[i+1]->getId());
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
            tail[i]->setParent(tail[i+1]->getId());
        }
        for(int i = 0; i < MAX_LEVEL; i++) {
            header[i]->hdr.next = tail[i]->getId();
            assert(header[i]->hdr.next != 0);

            // **修改：使用新的构造函数，并正确设置初始 covered_nodes**
            // header 的 GP 指向下一层，初始覆盖数为1（或0，如果它是最底层）
            header[i]->gps[0].covered_nodes = (i > 0) ? 1 : 0;
            dram_log_entry_t *header_entry = new dram_log_entry_t(header[i]->getId(), header[i]->hdr.last_index, header[i]->hdr.next, header[i]->hdr.level, header[i]->hdr.parent_id);
            header_entry->setKeyVal(0, header[i]->gps[0].key, header[i]->gps[0].value, header[i]->gps[0].covered_nodes);

            // tail 的 GP 不覆盖任何东西
            tail[i]->gps[0].covered_nodes = 0;
            dram_log_entry_t *tail_entry = new dram_log_entry_t(tail[i]->getId(), tail[i]->hdr.last_index, tail[i]->hdr.next, tail[i]->hdr.level, tail[i]->hdr.parent_id);
            tail_entry->setKeyVal(0, tail[i]->gps[0].key, tail[i]->gps[0].value, tail[i]->gps[0].covered_nodes);

            ckpt_log->batcher().addFull(tail_entry);
            ckpt_log->batcher().addFull(header_entry);
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
        //targetKey = reinterpret_cast<Vnode *>(targetVnode)->getMinKey();
        targetKey = bloom->getMinKey();
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
        else {
            next->insertAtPos(targetKey, new_nodes[i-1]->getId(), 0, 1);
            new_nodes[i-1]->setParent(next->getId());
        }

        dram_log_entry_t *next_entry = create_log_entry(next);
        dram_log_entry_t *cur_entry  = create_log_entry(current_update);
        ckpt_log->batcher().addFull(next_entry);
        ckpt_log->batcher().addFull(cur_entry);

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
                target->updateKeyVal(newKey,idx);
#if ENABLE_DELTA_LOG
                ckpt_log_single_slot_delta(ckpt_log, target, static_cast<int16_t>(idx));
#endif
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

Inode *DramSkiplist::lookupForInsert(Key_t key, Inode * &current,
                                     int currentHighestLevelIndex,
                                     std::shared_lock<std::shared_mutex> &current_lock,
                                     int &idx,
                                     std::vector<Inode *> &updates)
{
    int  start_level = -1;
    bool cache_hit_and_verified = false;
    int  current_total_level = currentHighestLevelIndex + 1;

    // 步骤 1: 尝试从缓存获取起点
    Inode *start_node = find_start_node_from_cache_shards(key, start_level);

    if (start_node != nullptr) {
        std::shared_lock<std::shared_mutex> lock_start(inode_locks[start_node->getId()]);
        Key_t lower_bound = start_node->getMinKey();
        Key_t upper_bound = get_node_upper_bound(start_node);

        // 软前推允许的最大横移步数（固定 2，不做动态调整）
        constexpr int SOFT_MAX_STEPS = 4;

        if (key < lower_bound) {
            // Hard fail：缓存起点在 key 右侧 -> 视为严重失效
#ifdef DBG_CACHE
            g_lfi_start_node_verify_failures.fetch_add(1, std::memory_order_relaxed);
#endif
            tls_mark_fail(start_node->getMinKey());
            // 回退到原始 temp_start，不采用该缓存节点
        } else {
            // key >= lower_bound：可以把 start_node 当作一个“候选起点”
            // 如果 key 已经在区间内，直接接受；否则尝试向右最多 SOFT_MAX_STEPS 步
            Inode *probe = start_node;
            std::shared_lock<std::shared_mutex> probe_lock = std::move(lock_start);
            if (key >= upper_bound) {
                int soft_steps = 0;
                while (soft_steps < SOFT_MAX_STEPS && key >= upper_bound) {
                    Inode *next_node = dramInodePool->at(probe->hdr.next);
                    if (!next_node || next_node->isTail()) break;

                    // 预判：如果下一节点最小键仍大于 key，则无需再前推
                    Key_t next_min = next_node->getMinKey();
                    if (key < next_min) break;

                    // 锁传递：先锁 next，再释放当前
                    std::shared_lock<std::shared_mutex> next_lock(inode_locks[next_node->getId()]);
                    probe_lock.unlock();
                    probe = next_node;
                    probe_lock = std::move(next_lock);
                    upper_bound = get_node_upper_bound(probe);
                    ++soft_steps;
                }

                // 若软前推后仍未覆盖 key，则放弃此次缓存使用（不记失败，不惩罚 pivot）
                if (key >= upper_bound) {
                    probe_lock.unlock();
                    probe = nullptr;
                }

            }
            if (probe) {
                // 接受（无论是直接 exact 还是 soft 前推后）
                current = probe;
                start_level = probe->hdr.level;
                cache_hit_and_verified = true;
                tls_record_pivot(probe);  // 仅记录起点，不在此调用 findKeyPos
                current_lock.unlock();
                current_lock = std::move(probe_lock);
            }
        }
    }

    // 步骤 2: 若缓存未命中或未被接受，则从最高层重新开始
    if (!cache_hit_and_verified) {
        start_level = currentHighestLevelIndex;
    }

    // 步骤 3: 正常自顶向下搜索（最终的 idx 在最底层统一计算）
    for (int i = start_level; i >= 0; --i) {
#ifdef DBG_SEARCH_STABILITY
        uint32_t horizontal_steps = 0;
#endif
        while (true) {
            assert(current != nullptr);
            Inode *next = dramInodePool->at(current->hdr.next);
            __builtin_prefetch(&next->hdr, 0, 1);
            __builtin_prefetch(next->gps,  0, 1);
            std::shared_lock<std::shared_mutex> next_h_lock(inode_locks[next->getId()]);
            if (!next->isTail() && key >= next->getMinKey()) {
                assert(current->getMaxKey() <= next->getMinKey());
                current = next;
                current_lock.unlock();
                current_lock = std::move(next_h_lock);
#ifdef DBG_SEARCH_STABILITY
                ++horizontal_steps;
#endif
            } else {
                break;
            }
        }
#ifdef DBG_SEARCH_STABILITY
        {
            const int threshold = SEARCH_STABILITY_COEFFICIENT_BY_LEVEL[i] + 1.0;
            if (horizontal_steps > (uint32_t)threshold) {
                record_horizontal_excess(key, i, horizontal_steps);
            }
        }
#endif
        if (i == 0) break;

        uint32_t next_level_node_id;
        if (current->isHeader()) {
            next_level_node_id = current->gps[0].value;
        } else {
            int temp_pos = current->findKeyPos(key);
            next_level_node_id = current->gps[temp_pos].value;
        }
        Inode *down = dramInodePool->at(next_level_node_id);
        assert(down != nullptr);
        std::shared_lock<std::shared_mutex> next_v_lock(inode_locks[down->getId()]);
        updates.push_back(current);
        current = down;
        current_lock.unlock();
        current_lock = std::move(next_v_lock);
    }

    // **步骤 4: 底层判空**
    if (current->isHeader()) {
        idx = -1;
        return nullptr;
    }

    // 步骤 5: 填充缓存（使用叶子 + 总层数信息）
    populate_cache_shards(key, current, current_total_level);

    // 步骤 6: 最终定位 idx（只在此处调用 findKeyPos）
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

        Key_t lower_bound = start_node->getMinKey();
        Key_t upper_bound = get_node_upper_bound(start_node);

        constexpr int SOFT_MAX_STEPS = 4; // max soft steps

        if(key < lower_bound) {
            // Hard fail: the cached start node is to the right of the key
#ifdef DBG_CACHE
            g_lfi_start_node_verify_failures.fetch_add(1, std::memory_order_relaxed);
#endif
            tls_mark_fail(start_node->getMinKey());
        }else {
            Inode *probe = start_node;
            std::shared_lock<std::shared_mutex> probe_lock = std::move(lock_start);
            if(key >= upper_bound) {
                int soft_steps = 0;
                while(soft_steps < SOFT_MAX_STEPS && key >= upper_bound) {
                    Inode *next_node = dramInodePool->at(probe->hdr.next);
                    if(!next_node || next_node->isTail()) break;

                    Key_t next_min = next_node->getMinKey();
                    if(key < next_min) break;

                    std::shared_lock<std::shared_mutex> next_lock(inode_locks[next_node->getId()]);
                    probe_lock.unlock();
                    probe = next_node;
                    probe_lock = std::move(next_lock);
                    upper_bound = get_node_upper_bound(probe);
                    ++soft_steps;
                }

                if(key >= upper_bound) {
                    probe_lock.unlock();
                    probe = nullptr;
                }
            }
            if(probe) {
                current = probe;
                start_level = probe->hdr.level;
                cache_hit_and_verified = true;
                tls_record_pivot(probe); // only record the start node, do not call findKeyPos here
                current_lock.unlock();
                current_lock = std::move(probe_lock);
            }
        }
    }

    // **步骤 3: 如果缓存未命中，则执行完整查找**
    if (!cache_hit_and_verified) {
        start_level = currentHighestLevelIndex;
    }

    for(int i = start_level; i >= 0; i--) {
        // no real index nodes between header and tail
        //search among the nodes in the current level
#ifdef DBG_SEARCH_STABILITY
        uint32_t horizontal_steps = 0;
#endif
        while(true) {
            assert(current != nullptr);
            {
                Inode *next = dramInodePool->at(current->hdr.next);
                __builtin_prefetch(&next->hdr, 0, 3);
                __builtin_prefetch(next->gps,  0, 3);
                std::shared_lock<std::shared_mutex> next_horizental_lock(inode_locks[next->getId()]);
                if(!next->isTail() && key >= next->getMinKey()) {
                    assert(current->getMaxKey() <= next->getMinKey());
                    current = next;
                    current_lock.unlock();
                    current_lock = std::move(next_horizental_lock);
#ifdef DBG_SEARCH_STABILITY
                    ++horizontal_steps;
#endif
                } else {
                    // found the node in this level, escape the look and go to the next level
                    break;
                }
            }
        }
#ifdef DBG_SEARCH_STABILITY
        {
            const int threshold = SEARCH_STABILITY_COEFFICIENT_BY_LEVEL[i] + 1.0;
            if (horizontal_steps > (uint32_t)threshold) {
                record_horizontal_excess(key, i, horizontal_steps);     
            }
        }
#endif
        //if already on the last level, return the current node
        if(i == 0) break;
        
        uint32_t next_level_node_id;
        if(current->isHeader()) {
            next_level_node_id = current->gps[0].value;            
        }else {
            int temp_idx = current->findKeyPos(key);
            next_level_node_id = current->gps[temp_idx].value;
        }

        Inode *down = dramInodePool->at(next_level_node_id);
        assert(down != nullptr);
        //lock passing for the next level node
        std::shared_lock<std::shared_mutex> next_vertical_lock(inode_locks[down->getId()]);
        current = down;
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

    // 步骤 6: 最终定位 idx（只在此处调用 findKeyPos）
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
    if (level < 0 || level >= MAX_LEVEL) {
        std::cout << "Invalid level: " << level << std::endl;
        return nullptr;
    }
    return header[level];
}

// 新增辅助函数：在无锁状态下寻找候选父节点
bool DramSkiplist::find_and_verify_candidate_parent(Inode* inode, Inode* parent_hint, 
                                        Inode*& candidate_parent, Inode*& candidate_next, 
                                        Inode*& header_above) {
    Inode* current = nullptr;
    int search_steps = 10;
    if (parent_hint) {
        Inode *current_parent = parent_hint;
        while(true) {
            Inode* next_parent = dramInodePool->at(current_parent->hdr.next);
            if(Inode::parentCoversChild(current_parent, next_parent, inode)) {
                candidate_parent = current_parent;
                candidate_next = next_parent;
                return true;
            }
            current_parent = next_parent;
            if(--search_steps <= 0) break;
        }
    }
    
    if (inode->hdr.level < MAX_LEVEL - 1) {
        header_above = getHeader(inode->hdr.level + 1);
        current = header_above;
    } else {
        cout << " this is on the top level " << endl;
        return false; 
    }

    while (true) {
        Inode* next_node = dramInodePool->at(current->hdr.next);
        Key_t key = inode->getMinKey();
        //cout << "Looking for key: " << key << " current node: "<< current->getId() << " current node minkey: " << current->getMinKey() <<" next node: " << next_node->getId() <<" next node minKey: " << next_node->getMinKey()<< endl;
        
        if (next_node->isTail() || key < next_node->getMinKey()) 
        {
            //cout << " going to break the loop, next_node: " << next_node->getId() << " is tail: " << isTail(next_node->getId()) << " key < next_node->getMinKey() " << (key < next_node->getMinKey()) << endl;
            candidate_parent = current;
            candidate_next = next_node;
            // 如果是从 header 开始找，且一步都没移动，说明父链表为空
            if (header_above && current->getId() == header_above->getId()) {
                 candidate_parent = nullptr; // 让上层逻辑去创建新父节点
            }
            return true;
        }
        current = next_node;
    }
}

int DramSkiplist::fastRebalance(Inode* &inode, Inode* &parent_inode_hint) 
{
    int ret = 0;
    bool created_parent = false;

    // 预分配新节点
    Inode *next_node = dramInodePool->getNextNode();
    if (!next_node) return 0;
    next_node->hdr.level = inode->hdr.level;

    while (true) {
        Inode* candidate_parent = nullptr;
        Inode* candidate_next   = nullptr;
        Inode* header_above     = nullptr;

        // 如有 hint，优先尝试；否则从上一层 header 线性定位
        find_and_verify_candidate_parent(inode, parent_inode_hint,
                                         candidate_parent, candidate_next, header_above);

        // 收集需加锁节点（过滤空指针）
        std::vector<Inode*> nodes_to_lock;
        nodes_to_lock.reserve(4);
        if (inode)           nodes_to_lock.push_back(inode);
        if (next_node)       nodes_to_lock.push_back(next_node);
        if (candidate_parent) nodes_to_lock.push_back(candidate_parent);
        if (header_above)     nodes_to_lock.push_back(header_above);

        std::vector<std::unique_lock<std::shared_mutex>> acquired_locks;
        acquireLocksInOrder(nodes_to_lock, acquired_locks);

        // 若已被其它线程平衡则退出
        if (inode->hdr.last_index < fanout / 2 - 1) {
            return 0;
        }

        // 验证父节点是否仍覆盖 (candidate_parent -> candidate_next)
        Inode* verified_parent = nullptr;
        if (candidate_parent) {
            if (dramInodePool->at(candidate_parent->hdr.next) == candidate_next) {
                verified_parent = candidate_parent;
            } else {
                continue; // 父的后继变化，重试
            }
        }

        // 如无已验证父，且上一层 head 后为 tail，则可创建新父
        dram_log_entry_t *verified_parent_entry = nullptr;
        dram_log_entry_t *header_above_entry    = nullptr;
        if (!verified_parent && header_above) {
            if (isTail(header_above->hdr.next)) {
                verified_parent = dramInodePool->getNextNode();
                if (!verified_parent) return 0;

                verified_parent->hdr.level = inode->hdr.level + 1;
                verified_parent->hdr.next  = header_above->hdr.next;
                header_above->hdr.next     = verified_parent->getId();

                // 新父接入：首槽指向当前 inode，覆盖 1
                verified_parent->insertAtPos(inode->getMinKey(), inode->getId(), 0, 1);
                inode->setParent(verified_parent->getId());
                increaseLevel();
                created_parent = true;

                // FULL 日志：新父与上一层 header
                verified_parent_entry = new dram_log_entry_t(
                    verified_parent->getId(),
                    verified_parent->hdr.last_index,
                    verified_parent->hdr.next,
                    verified_parent->hdr.level,
                    verified_parent->hdr.parent_id);
                verified_parent_entry->setKeyVal(
                    0,
                    verified_parent->gps[0].key,
                    verified_parent->gps[0].value,
                    verified_parent->gps[0].covered_nodes);

                header_above_entry = new dram_log_entry_t(
                    header_above->getId(),
                    header_above->hdr.last_index,
                    header_above->hdr.next,
                    header_above->hdr.level,
                    header_above->hdr.parent_id);
                header_above_entry->setKeyVal(
                    0,
                    header_above->gps[0].key,
                    header_above->gps[0].value,
                    header_above->gps[0].covered_nodes);
            } else {
                continue; // 上层水平链已改变，重试
            }
        }

        // 分裂当前节点：把 next_node 插到 inode 之后
        next_node->hdr.next = inode->hdr.next;
        inode->hdr.next     = next_node->getId();
        inode->split(next_node);
        //inode->splitWithSGP(next_node);
        const Key_t new_min_key = next_node->getMinKey();

        // 通用 FULL 日志提交
        auto commit_full_logs = [&](){
            auto next_entry  = this->create_log_entry(next_node);
            auto inode_entry = this->create_log_entry(inode);
            ckpt_log->batcher().addFull(next_entry);
            ckpt_log->batcher().addFull(inode_entry);
            if (verified_parent_entry) ckpt_log->batcher().addFull(verified_parent_entry);
            if (header_above_entry)    ckpt_log->batcher().addFull(header_above_entry);
        };

        // 顶层无父：只写两节点 FULL 日志即可
        if (!verified_parent) {
            commit_full_logs();
            ret = 1;
            parent_inode_hint = nullptr;
            break;
        }

        // 有父：定位 pos，并保证父子关系
        int pos = verified_parent->findKeyPos(inode->getMinKey());
        assert(pos >= 0 && pos <= verified_parent->hdr.last_index);
        if (inode->getParent() != verified_parent->getId()) {
            inode->setParent(verified_parent->getId());
        }

        // 情况 A：父在 pos 处平衡 -> 仅 covered_nodes++，完成
        if (!verified_parent->isUnbalanced(pos)) {
            verified_parent->gps[pos].covered_nodes++;
            next_node->setParent(verified_parent->getId());
            commit_full_logs();
#if ENABLE_DELTA_LOG
            ckpt_log_single_slot_delta(ckpt_log, verified_parent, static_cast<int16_t>(pos));
#endif
            ret = 1;
        }
        // 情况 B：父不平衡且已满 -> 不能激活 GP，先落盘，后续慢路径处理
        else if (verified_parent->isFull()) {
            verified_parent->gps[pos].covered_nodes++;
            next_node->setParent(verified_parent->getId());
            commit_full_logs();
#if ENABLE_DELTA_LOG
            ckpt_log_single_slot_delta(ckpt_log, verified_parent, static_cast<int16_t>(pos));
#endif
            ret = 2;
        }
        // 情况 C：父不平衡但可激活 GP
        else {
            verified_parent->gps[pos].covered_nodes++;

            // 计算 relative_pos（在 pos 段内从首子到 inode 的偏移）
            int16_t relative_pos = 0;
            {
                Inode *cur = dramInodePool->at(verified_parent->gps[pos].value);
                int idx = 0;
                Key_t upper_bound_key =
                    (pos + 1 <= verified_parent->hdr.last_index)
                    ? verified_parent->gps[pos + 1].key
                    : dramInodePool->at(verified_parent->hdr.next)->getMinKey();

                while (cur && !isTail(cur->getId())) {
                    if (cur->getId() == inode->getId()) {
                        relative_pos = static_cast<int16_t>(idx);
                        break;
                    }
                    if (cur->getMinKey() >= upper_bound_key) break;
                    cur = dramInodePool->at(cur->hdr.next);
                    ++idx;
                }
            }

            int temp_pos = -1;
            if (verified_parent->activateGP(new_min_key, next_node->getId(), temp_pos, relative_pos)) {
                next_node->setParent(verified_parent->getId());
                commit_full_logs();
#if ENABLE_DELTA_LOG
                auto new_verified_entry = create_log_entry(verified_parent);
                ckpt_log->batcher().addFull(new_verified_entry);
#endif
                ret = 1;
            } else {
                // 激活失败：保守落盘，交给后续慢路径
                next_node->setParent(verified_parent->getId());
                commit_full_logs();
                ret = 2;
            }
        }

        parent_inode_hint = verified_parent;
        break; // 成功路径退出重试循环
    }

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
        ckpt_log->batcher().addFull(entry.release());
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
    assert(inode->getId() >=0);
    auto entry = new dram_log_entry_t(inode->getId(), inode->hdr.last_index, inode->hdr.next, inode->hdr.level, inode->hdr.parent_id);
    for(int j = 0; j <= inode->hdr.last_index; j++) {
        entry->setKeyVal(j, inode->gps[j].key, inode->gps[j].value, inode->gps[j].covered_nodes);
    }
    return entry;
}

// 建议：可在类外增加微型统计（可选）
static std::atomic<uint64_t> g_parent_rd{0}, g_parent_miss{0}, g_parent_tryfail{0}, g_parent_wr{0};

Inode* DramSkiplist::getParentInode(Inode* &child) {
    if(child == nullptr) return nullptr;
    int32_t nodeid = child->getParent();
    if(nodeid != -1)
        return dramInodePool->at(nodeid);
    else
        return nullptr;
}

void DramSkiplist::removeInodeRelation(Inode* &child) {
    if (child == nullptr) return;
    size_t s = parent_shard_of(child);
    std::unique_lock<std::shared_mutex> lk(parent_shards[s].mtx, std::try_to_lock);
    if (!lk.owns_lock()) return; // 非关键路径，拿不到锁直接放弃
    parent_shards[s].map.erase(child);
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



#if ENABLE_L2_SHARD_CACHE
Inode *DramSkiplist::find_start_node_from_cache_shards(Key_t key, int& start_level) 
{
#if DBG_CACHE
    g_total_cache_lookups.fetch_add(1, std::memory_order_relaxed);
#endif
    if (Inode* n = tls_try_match(key, start_level)) return n;
#if DBG_CACHE
    g_l1_misses.fetch_add(1, std::memory_order_relaxed);
#endif
    size_t s = shard_of(key);
    CacheShard& shard = cache_shards[s];
    std::shared_lock<std::shared_mutex> r(shard.mtx);
    if (shard.table.empty()) {
#if DBG_CACHE
        g_l2_misses.fetch_add(1, std::memory_order_relaxed);
#endif
        return nullptr;
    }

    auto it = shard.table.upper_bound(key);
    if (it == shard.table.begin()) {
#if DBG_CACHE
        g_l2_misses.fetch_add(1, std::memory_order_relaxed);
#endif
        return nullptr;
    }
    -- it;
    Inode* start_node = it->second;
    if (!start_node) {
#if DBG_CACHE
        g_l2_misses.fetch_add(1, std::memory_order_relaxed);
#endif
        return nullptr;
    }
    else {
#if DBG_CACHE
        g_l2_hits.fetch_add(1, std::memory_order_relaxed);
#endif
        return start_node;
    }
}
#else
Inode *DramSkiplist::find_start_node_from_cache_shards(Key_t, int&) { return nullptr; }
#endif

#if ENABLE_L2_SHARD_CACHE
void DramSkiplist::populate_cache_shards(Key_t key, Inode* leaf_node, int current_total_level) {
    if (!leaf_node || leaf_node->hdr.level != 0 || leaf_node->isHeader() || current_total_level <= 1) {
        return;
    }
    static thread_local uint32_t pc_counter = 0;
    if ((++pc_counter & L2_BACKFILL_SAMPLE_MASK) != 0) return; // 使用宏控制采样

    int target_cache_level = std::max(1, current_total_level / 2);

    Inode * ancestor = leaf_node;
    Inode * node_to_cache = nullptr;

    while(ancestor != nullptr) {
        Inode *parent =
#if ENABLE_PARENT_RELATION
            getParentInode(ancestor);
#else
            nullptr;
#endif
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
#else
void DramSkiplist::populate_cache_shards(Key_t, Inode*, int) {}
#endif

// 若仍保留旧的单分片 populate_cache 版本，同样替换：
void DramSkiplist::populate_cache(Key_t key, Inode* leaf_node, int current_total_level) {
    if (leaf_node == nullptr || leaf_node->hdr.level != 0 || leaf_node->isHeader() || current_total_level <= 1) {
        return;
    }

    // 采样：把写缓存频率降到约 1/64，显著降低争用
    static thread_local uint32_t pc_counter = 0;
    if ((++pc_counter & L2_BACKFILL_SAMPLE_MASK) != 0) {
        return;
    }

    int target_cache_level = std::max(1, current_total_level / 2);

    Inode* ancestor = leaf_node;
    Inode* node_to_cache = nullptr;

    // 向上追溯，尽量靠近目标层；找不到就取能到达的最高祖先
#ifndef ENABLE_LEAF_ONLY_BACKFILL
    while (ancestor != nullptr) {
        Inode* parent =
#if ENABLE_PARENT_RELATION
            getParentInode(ancestor);
#else
            nullptr;
#endif
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
#else
    node_to_cache = leaf_node;
#endif

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

// filepath: /home/charliecqc/0612/HKV/Prototype/src/dramSkiplist.cpp
// ...existing code (includes)...

#ifndef ENABLE_DELTA_LOG
#define ENABLE_DELTA_LOG 1
#endif

#if ENABLE_DELTA_LOG
void DramSkiplist::ckpt_log_single_slot_delta(CkptLog *log, Inode *inode, int16_t slot) {
    if (!log || !inode) return;
    if (slot < 0 || slot > inode->hdr.last_index) return;
    const auto& gp = inode->gps[slot];
    log->batcher().addDeltaSlot(inode->hdr.id,
                  inode->hdr.last_index,
                  inode->hdr.next,
                  inode->hdr.parent_id,
                  slot, gp.key, gp.value, gp.covered_nodes);
}

void DramSkiplist::ckpt_log_multi_slots_delta(CkptLog *log,
                                       Inode *inode,
                                       const std::vector<int16_t> &slots) {
    if (!log || !inode || slots.empty()) return;
    static thread_local std::array<WalDeltaEntry, fanout> buf;
    size_t n = 0;
    for (int16_t s : slots) {
        if (s < 0 || s > inode->hdr.last_index) continue;
        buf[n++] = WalDeltaEntry{
            .slot    = s,
            .covered = inode->gps[s].covered_nodes,
            .key     = inode->gps[s].key,
            .value   = inode->gps[s].value
        };
        if (n == buf.size()) break;
    }
    if (n == 0) return;
    log->enqDelta(inode->hdr.id,
                  inode->hdr.last_index,
                  inode->hdr.next,
                  inode->hdr.parent_id,
                  buf.data(), n);
}
#endif // ENABLE_DELTA_LOG

Inode* DramSkiplist::nextAtLevel(Inode* n) const {
  int nid = n->hdr.next;
  return (nid >= 0) ? dramInodePool->at(nid) : nullptr;
}

std::vector<Inode*> DramSkiplist::nodesCoveringRangeAtLevel(uint64_t a, uint64_t b, int level) {
  std::vector<Inode*> out;
  Inode* cur = getHeader(level);
  if (!cur) return out;

  // 1) seek to first node that may overlap [a,b)
  while (cur && cur->getMaxKey() <= a) cur = nextAtLevel(cur);

  // 2) collect until we pass b
  while (cur && cur->getMinKey() < b) {
    // TODO? shared-lock while reading bounds
    out.push_back(cur);
    cur = nextAtLevel(cur);
  }
  return out;
}
