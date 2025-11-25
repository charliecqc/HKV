#include "dramSkiplist.h"
#include "checkpoint.h"
#include <cassert>
#include <mutex>
#include <optional>
#include <map>
#include <tuple>  
#include <shared_mutex>
#include <atomic>
#include <fstream>   // 新增
#include <immintrin.h>

#define numNodesInPool 10000000

#ifndef ENABLE_CACHE_STATS
#define ENABLE_CACHE_STATS 0
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
#define TLS_PIVOT_MAX 2
#endif

#if ENABLE_CACHE_STATS
#define DBG_CACHE 1
#endif

#ifndef ENABLE_TLS_SHADOW_STATS
#define ENABLE_TLS_SHADOW_STATS 1
#endif

#ifndef TLS_SHADOW_ENSURE_FREQ_THRESHOLD
#define TLS_SHADOW_ENSURE_FREQ_THRESHOLD 5   // 同线程访问同一 vnode 达到阈值才复制
#endif

#ifndef TLS_SHADOW_ENSURE_SAMPLE_MASK
#define TLS_SHADOW_ENSURE_SAMPLE_MASK 0xFF   // 1/64 采样复制首次冷 vnode
#endif

#ifndef TLS_SHADOW_MIN_DENSITY
#define TLS_SHADOW_MIN_DENSITY 14             // bitmap popcount 小于此值不缓存
#endif

#if ENABLE_TLS_SHADOW_STATS
// 改为线程本地计数，析构时聚合
thread_local uint64_t tl_attempts = 0;
thread_local uint64_t tl_hits = 0;
std::atomic<uint64_t> g_tlsShadowAttempts{0};
std::atomic<uint64_t> g_tlsShadowHits{0};
#endif

// 线程本地热度计数
thread_local int g_last_vnode_id = -1;
thread_local int g_last_vnode_freq = 0;
thread_local uint64_t g_shadow_sample_counter = 0;

#if ENABLE_VNODE_SHADOW_CACHE
struct TlsLastVnodeSlot {
    int vnode_id{-1};
    uint64_t version{0};
    TLSVnodeShadowSlot* ptr{nullptr};
};
thread_local TlsLastVnodeSlot tls_last_vnode;

inline bool tls_should_ensure(int vnode_id, uint32_t popcnt) {
    if (popcnt < TLS_SHADOW_MIN_DENSITY) return false;
    if (g_last_vnode_id == vnode_id) {
        ++g_last_vnode_freq;
    } else {
        g_last_vnode_id = vnode_id;
        g_last_vnode_freq = 1;
    }
    if (g_last_vnode_freq >= TLS_SHADOW_ENSURE_FREQ_THRESHOLD) return true;
    // 低频阶段采用采样
    return ((++g_shadow_sample_counter & TLS_SHADOW_ENSURE_SAMPLE_MASK) == 0);
}

static inline size_t vnode_cache_hash(Key_t k) {
    constexpr uint64_t A = 11400714819323198485ull; // golden ratio
    return static_cast<size_t>((k * A) >> (64 - 3)) & (TlsVnodeCopyCache::kCap - 1);
}
#endif

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

bool DramSkiplist::add(Vnode *targetVnode) 
{
    Key_t targetKey = std::numeric_limits<Key_t>::max();
    Inode* updates[MAX_LEVEL];
    {
        BloomFilter *bloom = &valueList->bf[targetVnode->hdr.id];
        targetKey = read_consistent(bloom->version, [&]() {
            return bloom->getMinKey();
        });
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

    // 记录已加锁的前驱，失败时统一回滚
    std::vector<Inode*> locked_updates;
    locked_updates.reserve(predecessors.size());

    for (Inode *update: predecessors) {
        write_lock(update->version);
        locked_updates.push_back(update);

        Inode *next = dramInodePool->at(update->hdr.next);
        if (targetKey >= next->getMinKey()) {
            // 失败：需释放此前所有加过的锁
            for (auto it = locked_updates.rbegin(); it != locked_updates.rend(); ++it) {
                write_unlock((*it)->version);
            }
            return false; // 未完成结构修改，不推进 epoch
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

        if(is_locked(current_update->version)) {
            write_unlock(current_update->version);
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

static inline __attribute__((always_inline)) uint64_t spin_load_version(const std::atomic<uint64_t>& ver_atomic) {
    uint64_t v = ver_atomic.load(std::memory_order_acquire);
    if ((v & 1) == 0) [[likely]] return v; // 快速路径：无锁
    
    // 慢速路径：自旋等待
    do {
        _mm_pause(); // 提示 CPU 这是一个自旋循环，节省功耗并减少流水线清空惩罚
        v = ver_atomic.load(std::memory_order_acquire);
    } while (v & 1);
    return v;
}

Inode *DramSkiplist::lookup(Key_t key, Inode *current, int currentHighestLevelIndex, int &idx, InodeSnapShort &snap)
{
    int start_level = -1;

    // 1. L1 Cache 逻辑 (保留你现有的极简版 L1 逻辑)
    // 假设 tls_try_match 已经优化为极简版
    if (Inode* n = tls_try_match(key, start_level)) {
        current = n;
    } else {
        start_level = currentHighestLevelIndex;
        current = header[start_level];
    }

    // 2. 主循环：手动内联 OCC，移除 Lambda
    for (int lvl = start_level; lvl >= 0; --lvl) {
        
        // --- 水平搜索 (Horizontal Search) ---
        while (true) {
            uint32_t next_id;
            uint64_t v_cur;
            
            // [优化] 读取 next 指针 (手动 OCC)
            // 替代: next_id = read_consistent(current->version, [&]{ return current->hdr.next; });
            while (true) {
                v_cur = spin_load_version(current->version);
                next_id = current->hdr.next;
                std::atomic_thread_fence(std::memory_order_acquire); // 确保读取在版本检查前完成
                if (current->version.load(std::memory_order_relaxed) == v_cur) [[likely]] break;
            }

            if (isTail(next_id)) break;

            // 获取下一个节点
            Inode* next_node = dramInodePool->at(next_id);
            
            // [优化] 软件预取：提前将 next_node 加载到 L1 Cache
            __builtin_prefetch(next_node, 0, 1);

            Key_t next_min;
            uint64_t v_next;

            // [优化] 读取 next_min (手动 OCC)
            // 替代: next_min = read_consistent(next_node->version, [&]{ return next_node->getMinKey(); });
            while (true) {
                v_next = spin_load_version(next_node->version);
                next_min = next_node->getMinKey(); 
                std::atomic_thread_fence(std::memory_order_acquire);
                if (next_node->version.load(std::memory_order_relaxed) == v_next) [[likely]] break;
            }

            if (key >= next_min) {
                current = next_node;
            } else {
                break;
            }
        }

        // --- 垂直下降 (Vertical Descent) ---
        if (lvl > 0) {
            uint32_t child_id = 0;
            uint64_t v_cur;
            
            // [优化] 读取子节点指针 (手动 OCC)
            while (true) {
                v_cur = spin_load_version(current->version);
                
                if (current->isHeader()) {
                    // Header 节点通常结构固定，直接取第0个
                    child_id = current->gps[0].value; 
                } else {
                    // 在节点内查找 key 的位置
                    int pos = current->findKeyPos(key);
                    if (pos >= 0) {
                        child_id = current->gps[pos].value;
                    } else {
                        // 异常保护：如果 key 没找到（理论上不应发生），回退到第一个子节点或报错
                        child_id = current->gps[0].value; 
                    }
                }
                
                std::atomic_thread_fence(std::memory_order_acquire);
                if (current->version.load(std::memory_order_relaxed) == v_cur) [[likely]] break;
            }

            if (child_id != 0) {
                current = dramInodePool->at(child_id);
                // [优化] 预取子节点
                __builtin_prefetch(current, 0, 1);
            }
        }
    }

    // --- 叶子节点处理 (Leaf Node) ---
    // 此时 lvl == -1，current 指向叶子节点
    
    uint64_t v_leaf;
    while (true) {
        v_leaf = spin_load_version(current->version);
        
        idx = current->findKeyPos(key);
        
        // 填充快照信息 (用于后续验证或乐观读)
        snap.ver_snap = v_leaf;
        snap.idx = static_cast<int16_t>(idx);
        snap.gp_key = current->gps[idx].key;
        snap.gp_value = current->gps[idx].value;
        snap.last_index = current->hdr.last_index;
        
        std::atomic_thread_fence(std::memory_order_acquire);
        if (current->version.load(std::memory_order_relaxed) == v_leaf) [[likely]] break;
    }

    // L1 Cache 记录 (如果启用且命中)
    if (idx >= 0) {
        tls_record_pivot(current);
    } else {
        // 如果没找到，可能需要失效 L1
        // invalidate_tls_pivot(); 
    }

    return current;
}

Inode *DramSkiplist::lookup_lambda(Key_t key, Inode *current, int currentHighestLevelIndex, int &idx, InodeSnapShort &snap)
{
    int  start_level = -1;
    bool cache_hit_and_verified = false;
    int  current_total_level = currentHighestLevelIndex + 1;

    // 步骤 1: 尝试从缓存获取起点
#if ENBALE_L1_TLS_CACHE
    Inode *start_node = tls_try_match(key, start_level);
#else
    Inode *start_node = nullptr;
#endif

    if (start_node != nullptr) {
        Key_t min_k = read_consistent(start_node->version, [&] {
        return start_node->getMinKey();
        });

        if (key >= min_k) {
            current = start_node;
            start_level = read_consistent(start_node->version, [&] {
                return start_node->hdr.level;
            });
            cache_hit_and_verified = true;
            tls_record_pivot(start_node);
        } else {
            tls_mark_fail(min_k);
        }
    }
    // 步骤 2: 若缓存未命中或未被接受，则从最高层重新开始

    if (!cache_hit_and_verified) {
        start_level = currentHighestLevelIndex;
    }

    for (int lvl = start_level; lvl >= 0; --lvl) {
        struct Decision {
            bool move_right{false};
            uint32_t next_id{std::numeric_limits<uint32_t>::max()};
            uint32_t child_id{std::numeric_limits<uint32_t>::max()};
            int leaf_pos{-1};
            bool ok{false};
            int64_t snap_version{0};
        };

        Decision dec{};
        uint64_t snap_parent{0};

        // 新增：将提交动作封装为 helper，确保“提交前最后一次验证”
        auto commit_right = [&](Inode* parent, uint64_t snap_p, uint32_t expected_next_id) -> bool {
            // 1) 父下一指针必须在一个稳定快照下等于期望
            uint32_t observed_next{std::numeric_limits<uint32_t>::max()};
            observed_next = read_consistent(parent->version, [&]() { return parent->hdr.next; });

            if (observed_next != expected_next_id) {
                cout << "observe_next != expected_next_id" << endl;
                return false;
            }

            // 2) 在 next 的稳定快照下确认仍可右移
            Inode* nxt = dramInodePool->at(observed_next);
            if (!nxt) return false;
            Key_t next_min{std::numeric_limits<Key_t>::max()};
            next_min = read_consistent(nxt->version, [&]() { return nxt->getMinKey(); });
            if (next_min == std::numeric_limits<Key_t>::max() || key < next_min) return false;

            current = nxt; //commit
            return true;
        };

        auto commit_down = [&](Inode* parent, uint64_t snap_p,
                               uint32_t child_id, int leaf_pos) -> bool {
            Inode* child = dramInodePool->at(child_id);
            if (!child) {
                return false;
            }
            if (!validate_snapshot(parent->version, snap_p)) {
                Key_t range_start = parent->gps[leaf_pos].key;
                Key_t range_end =(leaf_pos == parent->hdr.last_index)
                                    ? std::numeric_limits<Key_t>::max()
                                    : parent->gps[leaf_pos + 1].key;
                bool still_in_range = (range_start <= key) && (key < range_end);
                if(!still_in_range) {
                    // 父已变化且 key 不在新范围内
                    //cout << "parent changed and key out of range for key: " << key << " range_start :" << range_start << " range_end: "<< range_end << endl;
                    return false;
                }
            }

            current = child; // 提交
            return true;
        };

        while (true) {
            auto decide = [&](Inode* cur) -> Decision {
                Decision d;
                std::tie(d, d.snap_version) = read_consistent_with_snap(cur->version, [&]() -> Decision {
                    Decision x;
                    uint32_t next_id = cur->hdr.next;
                    if (!isTail(next_id)) {
                        Inode* next = dramInodePool->at(next_id);
                        bool move = read_consistent(next->version, [&]() -> bool {
                            Key_t next_min = next->getMinKey();
                            return next_min != std::numeric_limits<Key_t>::max() && key >= next_min;
                        });
                        if (move) {
                            x.move_right = true;
                            x.next_id = next_id;
                            x.ok = true;
                            return x;
                        }
                    }
                    if (lvl == 0) {
                        x.leaf_pos = cur->findKeyPos(key);
                        x.ok = true;
                        return x;
                    }
                    int pos = cur->isHeader() ? 0 : cur->findKeyPos(key);
                    x.child_id = cur->gps[pos].value;
                    x.leaf_pos = pos;
                    x.ok = true;
                    return x;
                });
                return d;
            };

            std::tie(dec, snap_parent) = read_consistent_with_snap(current->version, [&]() {
                return decide(current);
            });
            if (!dec.ok) continue;
            if (dec.snap_version != snap_parent) {
                cout << "snap_version != snap_parent for key: " << key << endl;
                continue;
            }

            if (dec.move_right) {
                if (!commit_right(current, snap_parent, dec.next_id)) {
                    // 父或 next 被改，重试本层
                    continue;
                }
                // 成功右移后，继续本层横移
                continue;
            } else {
                break; // 准备向下
            }
        }

        if (lvl == 0) {
            // 叶层：一次性采样 idx + 快照
            uint64_t token = 0;
            std::tie(std::ignore, token) = read_consistent_with_snap(current->version, [&]() {
                idx = (dec.leaf_pos >= 0) ? dec.leaf_pos : current->findKeyPos(key);
                snap.last_index = current->hdr.last_index;
                snap.next       = current->hdr.next;
                snap.idx        = static_cast<int16_t>(idx);
                if (idx >= 0 && idx <= current->hdr.last_index) {
                    snap.gp_key   = current->gps[idx].key;
                    snap.gp_value = current->gps[idx].value;
                    snap.lb_key   = current->gps[idx].key;
                    snap.ub_key   = (idx < current->hdr.last_index)
                                    ? current->gps[idx + 1].key
                                    : std::numeric_limits<Key_t>::max();
                } else {
                    snap.gp_key = 0; snap.gp_value = -1;
                    snap.lb_key = 0; snap.ub_key = std::numeric_limits<Key_t>::max();
                }
                return 0;
            });
            snap.ver_snap = token;
            break;
        }

        // 下探提交（带二次验证）
        if (!commit_down(current, snap_parent, dec.child_id, dec.leaf_pos)) {
            // 父或子失效，重试该层
            ++lvl; // no-op trick: 继续 while(true)
            //cout << "retry for key: " << key << " at level " << lvl << endl;
            continue;
        }
    }

    if (current->isHeader()) {
        idx = -1;
        return nullptr;
    }

    assert(current->hdr.level == 0);
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
                                        Inode*& header_above) 
{
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

        //std::vector<std::unique_lock<std::shared_mutex>> acquired_locks;
        //acquireLocksInOrder(nodes_to_lock, acquired_locks);
        acquireWriteLocksInOrderByVersion(nodes_to_lock);

        // 若已被其它线程平衡则退出
        if (inode->hdr.last_index < fanout / 2 - 1) {
            releaseWriteLocksInOrderByVersion(nodes_to_lock);
            return 0;
        }

        // 验证父节点是否仍覆盖 (candidate_parent -> candidate_next)
        Inode* verified_parent = nullptr;
        if (candidate_parent) {
            if (dramInodePool->at(candidate_parent->hdr.next) == candidate_next) {
                verified_parent = candidate_parent;
            } else {
                releaseWriteLocksInOrderByVersion(nodes_to_lock);
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
                releaseWriteLocksInOrderByVersion(nodes_to_lock);
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
            releaseWriteLocksInOrderByVersion(nodes_to_lock);
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
        releaseWriteLocksInOrderByVersion(nodes_to_lock);
        break; // 成功路径退出重试循环
    }

    if (created_parent) {
        bump_epoch();
    }
    return ret;
}

#if 0
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
#endif

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

void DramSkiplist::acquireWriteLocksInOrderByVersion(std::vector<Inode*>& nodes) 
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
            write_lock(node->version);
        }
    }
}

void DramSkiplist::releaseWriteLocksInOrderByVersion(std::vector<Inode*>& nodes)
{
    for(Inode* node : nodes) {
        if(node != nullptr) {
            write_unlock(node->version);
        }
    }
}

// 线程本地缓存定义（类内静态成员的实体）
thread_local TlsVnodeCopyCache DramSkiplist::tls_vnode_copy_cache_{};
thread_local uint64_t DramSkiplist::tls_vnode_copy_lru_clock_{};

thread_local decltype(DramSkiplist::tls_pivot_set_) DramSkiplist::tls_pivot_set_ = { { }, 0 };
thread_local Key_t  DramSkiplist::tls_pivot_key_  = 0;
thread_local Inode* DramSkiplist::tls_pivot_node_ = nullptr;

#if ENABLE_VNODE_SHADOW_CACHE
thread_local TLSVnodeShadowCache DramSkiplist::tls_shadow_cache_{};
#endif

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


Inode* DramSkiplist::lookupForInsertWithSnap(Key_t key, Inode* &current, int currentHighestLevelIndex,
                                             int &idx, std::vector<Inode*> &updates,
                                             InodeSnapShort &snap)
{
    int  start_level = -1;
    bool cache_hit_and_verified = false;
    int  current_total_level = currentHighestLevelIndex + 1;

    // 步骤 1: 尝试从缓存获取起点
    //Inode *start_node = find_start_node_from_cache_shards(key, start_level);
    Inode *start_node = nullptr;

    if (start_node != nullptr) {
        // 软前推允许的最大横移步数（固定 4，不做动态调整）
        constexpr int SOFT_MAX_STEPS = 3;

        auto load_bounds = [&](Inode* n, Key_t& lb, Key_t& ub) {
            // lb：n 的最小键（在 n 的一致快照下读取）
            lb = read_consistent(n->version, [&]() -> Key_t {
                return n->getMinKey();
            });

            // ub：n 的真实上界（next 的最小键；如无 next 或 next 为 tail，则为 +inf）
            ub = std::numeric_limits<Key_t>::max();
            uint32_t next_id = read_consistent(n->version, [&]() -> uint32_t {
                return n->hdr.next;
            });
            if (!isTail(next_id)) {
                Inode* nxt = dramInodePool->at(next_id);
                if (nxt) {
                    Key_t next_min = read_consistent(nxt->version, [&]() -> Key_t {
                        return nxt->getMinKey();
                    });
                    if (next_min != std::numeric_limits<Key_t>::max()) {
                        ub = next_min;
                    }
                }
            }
        };

        Key_t lower_bound{0}, upper_bound{0};
        load_bounds(start_node, lower_bound, upper_bound);

        if (key < lower_bound) {
        // Hard fail：缓存起点在 key 右侧 -> 视为严重失效
#ifdef DBG_CACHE
            g_lfi_start_node_verify_failures.fetch_add(1, std::memory_order_relaxed);
#endif
            //tls_mark_fail(start_node->getMinKey());
            tls_mark_fail(lower_bound);
        // 回退到原始 temp_start，不采用该缓存节点
        } else {
        // key >= lower_bound：可以把 start_node 当作一个“候选起点”
            Inode *probe = start_node;

            if (key >= upper_bound) {
                int soft_steps = 0;
                while (soft_steps < SOFT_MAX_STEPS && key >= upper_bound) {
                // 读取 probe->next，并绑定父快照
                    uint32_t next_id = std::numeric_limits<uint32_t>::max();
                    uint64_t snap_parent = 0;
                    std::tie(next_id, snap_parent) = read_consistent_with_snap(probe->version, [&]() -> uint32_t {
                            return probe->hdr.next;
                        });

                    if (isTail(next_id)) break;

                    Inode *next_node = dramInodePool->at(next_id);
                    if (!next_node) break;

                // 在 next 的一致性快照下读取 next_min
                    Key_t next_min = std::numeric_limits<Key_t>::max();
                    uint64_t snap_next = 0;
                    std::tie(next_min, snap_next) = read_consistent_with_snap(next_node->version, [&]() -> Key_t {
                            return next_node->getMinKey();
                        });

                // tail 或 next_min 上跳则无法右移
                    if (next_min == std::numeric_limits<Key_t>::max() || key < next_min) {
                        break;
                    }

                // 提交前验证父快照仍然有效
                    if (!validate_snapshot(probe->version, snap_parent)) {
                        // 父已变化，重新加载当前边界并重试
                        load_bounds(probe, lower_bound, upper_bound);
                        continue;
                    }

                // 提交：前推到 next_node
                    probe = next_node;

                // 更新新的上界
                    Key_t dummy_lb{0};
                    load_bounds(probe, dummy_lb, upper_bound);

                    ++soft_steps;
                }

            // 若软前推后仍未覆盖 key，则放弃此次缓存使用（不记失败，不惩罚 pivot）
                if (key >= upper_bound) {
                    probe = nullptr;
                }
            }

            if (probe) {
            // 接受（无论是直接 exact 还是 soft 前推后）
                current = probe;
                start_level = read_consistent(probe->version, [&]() -> int {
                    return probe->hdr.level;
                });
                cache_hit_and_verified = true;
                tls_record_pivot(probe); // 仅记录起点，不在此调用 findKeyPos
                // 注意：不再操作 current_lock（lock-free 风格）
            }
        }
    }
    // 步骤 2: 若缓存未命中或未被接受，则从最高层重新开始

    if (!cache_hit_and_verified) {
        start_level = currentHighestLevelIndex;
    }

    for (int lvl = start_level; lvl >= 0; --lvl) {
        struct Decision {
            bool move_right{false};
            uint32_t next_id{std::numeric_limits<uint32_t>::max()};
            uint32_t child_id{std::numeric_limits<uint32_t>::max()};
            int leaf_pos{-1};
            bool ok{false};
            int64_t snap_version{0};
        };

        Decision dec{};
        uint64_t snap_parent{0};

        // 新增：将提交动作封装为 helper，确保“提交前最后一次验证”
        auto commit_right = [&](Inode* parent, uint64_t snap_p, uint32_t expected_next_id) -> bool {
            // 1) 父下一指针必须在一个稳定快照下等于期望
            uint32_t observed_next{std::numeric_limits<uint32_t>::max()};
            observed_next = read_consistent(parent->version, [&]() { return parent->hdr.next; });

            if (observed_next != expected_next_id) {
                cout << "observe_next != expected_next_id" << endl;
                return false;
            }

            // 2) 在 next 的稳定快照下确认仍可右移
            Inode* nxt = dramInodePool->at(observed_next);
            if (!nxt) return false;
            Key_t next_min{std::numeric_limits<Key_t>::max()};
            next_min = read_consistent(nxt->version, [&]() { return nxt->getMinKey(); });
            if (next_min == std::numeric_limits<Key_t>::max() || key < next_min) return false;

            current = nxt; //commit
            return true;
        };

        auto commit_down = [&](Inode* parent, uint64_t snap_p,
                               uint32_t child_id, int leaf_pos) -> bool {
            Inode* child = dramInodePool->at(child_id);
            if (!child) {
                return false;
            }
            if (!validate_snapshot(parent->version, snap_p)) {
                Key_t range_start = parent->gps[leaf_pos].key;
                Key_t range_end =(leaf_pos == parent->hdr.last_index)
                                    ? std::numeric_limits<Key_t>::max()
                                    : parent->gps[leaf_pos + 1].key;
                bool still_in_range = (range_start <= key) && (key < range_end);
                if(!still_in_range) {
                    // 父已变化且 key 不在新范围内
                    //cout << "parent changed and key out of range for key: " << key << " range_start :" << range_start << " range_end: "<< range_end << endl;
                    return false;
                }
            }

            current = child; // 提交
            return true;
        };

        while (true) {
            auto decide = [&](Inode* cur) -> Decision {
                Decision d;
                std::tie(d, d.snap_version) = read_consistent_with_snap(cur->version, [&]() -> Decision {
                    Decision x;
                    uint32_t next_id = cur->hdr.next;
                    if (!isTail(next_id)) {
                        Inode* next = dramInodePool->at(next_id);
                        bool move = read_consistent(next->version, [&]() -> bool {
                            Key_t next_min = next->getMinKey();
                            return next_min != std::numeric_limits<Key_t>::max() && key >= next_min;
                        });
                        if (move) {
                            x.move_right = true;
                            x.next_id = next_id;
                            x.ok = true;
                            return x;
                        }
                    }
                    if (lvl == 0) {
                        x.leaf_pos = cur->findKeyPos(key);
                        x.ok = true;
                        return x;
                    }
                    int pos = cur->isHeader() ? 0 : cur->findKeyPos(key);
                    x.child_id = cur->gps[pos].value;
                    x.leaf_pos = pos;
                    x.ok = true;
                    return x;
                });
                return d;
            };

            std::tie(dec, snap_parent) = read_consistent_with_snap(current->version, [&]() {
                return decide(current);
            });
            if (!dec.ok) continue;
            if (dec.snap_version != snap_parent) {
                cout << "snap_version != snap_parent for key: " << key << endl;
                continue;
            }

            if (dec.move_right) {
                if (!commit_right(current, snap_parent, dec.next_id)) {
                    // 父或 next 被改，重试本层
                    continue;
                }
                // 成功右移后，继续本层横移
                continue;
            } else {
                break; // 准备向下
            }
        }

        // 记录父链用于插入
        if (lvl > 0) updates.push_back(current);

        if (lvl == 0) {
            // 叶层：一次性采样 idx + 快照
            uint64_t token = 0;
            std::tie(std::ignore, token) = read_consistent_with_snap(current->version, [&]() {
                idx = (dec.leaf_pos >= 0) ? dec.leaf_pos : current->findKeyPos(key);
                snap.last_index = current->hdr.last_index;
                snap.next       = current->hdr.next;
                snap.idx        = static_cast<int16_t>(idx);
                if (idx >= 0 && idx <= current->hdr.last_index) {
                    snap.gp_key   = current->gps[idx].key;
                    snap.gp_value = current->gps[idx].value;
                    snap.lb_key   = current->gps[idx].key;
                    snap.ub_key   = (idx < current->hdr.last_index)
                                    ? current->gps[idx + 1].key
                                    : std::numeric_limits<Key_t>::max();
                } else {
                    snap.gp_key = 0; snap.gp_value = -1;
                    snap.lb_key = 0; snap.ub_key = std::numeric_limits<Key_t>::max();
                }
                return 0;
            });
            snap.ver_snap = token;
            break;
        }

        // 下探提交（带二次验证）
        if (!commit_down(current, snap_parent, dec.child_id, dec.leaf_pos)) {
            // 父或子失效，重试该层
            updates.pop_back();
            ++lvl; // no-op trick: 继续 while(true)
            //cout << "retry for key: " << key << " at level " << lvl << endl;
            continue;
        }
    }

    if (current->isHeader()) {
        idx = -1;
        return nullptr;
    }
    for (int i = 0; i+1 < (int)updates.size(); ++i) {
        assert(updates[i]->hdr.id != updates[i+1]->hdr.id);
    }
    assert(current->hdr.level == 0);
    return current;
}

// 仍保留：短快照校验
bool DramSkiplist::validateSnapShort(Inode* n, const InodeSnapShort& s, Key_t key) const
{
    if (!n) return false;

    // 1.verify snapshot token first (fast path)
    if (s.ver_snap != 0 && validate_snapshot(n->version, s.ver_snap)) {
        bool ret = (s.idx >= 0 && s.idx <= s.last_index);
        if (!ret) {
            cout << "validateSnapShort: invalid idx " << s.idx << " for last_index " << s.last_index << endl;
        }
        return ret;
    }

    // 2) 若版本戳已失效，退化为字段/区间的严格比对（较慢，但安全）
    return read_consistent(n->version, [&]() -> bool {
        Key_t cur_lb = n->gps[0].key;
        Key_t cur_ub = std::numeric_limits<Key_t>::max();
        int next_id = n->hdr.next;
        Inode *next_inode = dramInodePool->at(next_id);
        if(!next_inode->isTail()) {
            cur_ub = read_consistent(next_inode->version, [&]() -> Key_t {
                return next_inode->getMinKey();
            });
        }
        if(key < cur_lb || key >= cur_ub) {
            //cout << "validateSnapShort: key " << key << " out of bounds [" << cur_lb << ", " << cur_ub << ")" << endl;
            return false;
        }

        return true;
#if 0
        if (n->hdr.last_index != s.last_index) {
            cout << "validateSnapShort: last_index mismatch " << n->hdr.last_index << " vs " << s.last_index << endl;
            return false;
        }
        if (n->hdr.next != s.next){       
            cout << "validateSnapShort: next mismatch " << n->hdr.next << " vs " << s.next << endl;
            return false;
        }
        if (s.idx < 0 || s.idx > n->hdr.last_index) {
            cout << "validateSnapShort: idx out of bounds " << s.idx << " for last_index " << s.last_index << endl;
            return false;
        }


        // 当前边界
        Key_t cur_lb = n->gps[s.idx].key;
        Key_t cur_ub = (s.idx < n->hdr.last_index)
                       ? n->gps[s.idx + 1].key
                       : std::numeric_limits<Key_t>::max();

        // 边界与快照一致，且 key 仍命中该槽位
        if (cur_lb != s.lb_key) {
            cout << "validateSnapShort: lb_key mismatch " << cur_lb << " vs " << s.lb_key << endl;
            return false;
        }
        if ((s.idx < s.last_index) && (cur_ub != s.ub_key)) {
            cout << "validateSnapShort: ub_key mismatch " << cur_ub << " vs " << s.ub_key << endl;
            return false;
        }
        if (!(key >= cur_lb && key < cur_ub)) {
            cout << "validateSnapShort: key " << key << " out of bounds [" << cur_lb << ", " << cur_ub << ")" << endl;
            return false;
        }

        // 槽位内容一致（尤其是 value=vnode id）
        if (n->gps[s.idx].key   != s.gp_key){   
            cout << "validateSnapShort: gp_key mismatch " << n->gps[s.idx].key << " vs " << s.gp_key << endl;
            return false;
        }
        if (n->gps[s.idx].value != s.gp_value){ 
            cout << "validateSnapShort: gp_value mismatch " << n->gps[s.idx].value << " vs " << s.gp_value << endl;
            return false;
        }

        return true;
#endif
    });
}

// 查找：版本匹配 + bitmap 遍历
bool DramSkiplist::tlsShadowLookup(int vnode_id, Key_t key, Val_t &out,
                                   BloomFilter* bloom, Vnode* vnode)
{
#if !ENABLE_VNODE_SHADOW_CACHE
    return false;
#else
    if (!bloom || !vnode) return false;
    uint64_t ver = bloom->version.load(std::memory_order_acquire);
    TLSVnodeShadowSlot* slot = tls_shadow_cache_.probe(vnode_id, ver);
    if (!slot) return false;

    // 紧凑数组快速匹配：tag 先过滤，再比 key
    const uint8_t tag = static_cast<uint8_t>(key);
    auto* arr = slot->packed;
    const uint16_t n = slot->count;
    for (uint16_t i = 0; i < n; ++i) {
        if (arr[i].tag != tag) continue;
        if (arr[i].key == key) {
            out = arr[i].value;
            slot->last_use = ++tls_shadow_cache_.clock;
            return true;
        }
    }
    slot->last_use = ++tls_shadow_cache_.clock;
    return false;
#endif
}

void DramSkiplist::tlsShadowEnsure(int vnode_id, BloomFilter* bloom, Vnode* vnode)
{
#if !ENABLE_VNODE_SHADOW_CACHE
    return;
#else
    if (!bloom || !vnode) return;
    const uint64_t ver_snap = bloom->version.load(std::memory_order_acquire);
    if (auto* ex = tls_shadow_cache_.probe(vnode_id, ver_snap)) {
        ex->last_use = ++tls_shadow_cache_.clock;
        return;
    }

    // 复制前先拿快照位图，用于紧凑复制
    const uint32_t bm_snapshot = vnode->hdr.bitmap & VNODE_FULL_MASK;
    TLSVnodeShadowSlot* slot = tls_shadow_cache_.victim(vnode_id);

    const uint64_t v1 = bloom->version.load(std::memory_order_acquire);
    __builtin_prefetch(vnode->records, 0, 3);

    // 仅拷贝 bitmap=1 的条目，构建 tag
    uint16_t cnt = 0;
    uint32_t bm = bm_snapshot;
    while (bm) {
        const int idx = 31 - __builtin_clz(bm);
        bm &= ~(1u << idx);
        const auto& r = vnode->records[idx];
        slot->packed[cnt].key   = r.key;
        slot->packed[cnt].value = r.value;
        slot->packed[cnt].tag   = static_cast<uint8_t>(r.key);
        ++cnt;
    }

    const uint64_t v2 = bloom->version.load(std::memory_order_acquire);
    if (v1 != v2) return; // 版本跳变，放弃

    slot->vnode_id = vnode_id;
    slot->version  = v2;
    slot->bitmap   = bm_snapshot;
    slot->count    = cnt;
    slot->last_use = ++tls_shadow_cache_.clock;

    // 成功后回填 L0
    tls_last_vnode.vnode_id = vnode_id;
    tls_last_vnode.version  = slot->version;
    tls_last_vnode.ptr      = slot;
#endif
}
