#include "skiplist.h"
#include "checkpoint.h"
#include "dramInodePool.h"
#include "pmemVnodePool.h"
#include "ckpt_log.h"
#include "common.h"
#include "valuelist.h"
#include <map>
#include <shared_mutex>
#include <unordered_map>   // 新增
#pragma once

#define TLS_PIVOT_MAX 2   // 扩容：3 -> 8

class CacheShard {
public:
    std::map<Key_t, Inode*> table;
    mutable std::shared_mutex mtx;
};

// 新增：分片父关系表
class ParentShard {
public:
    std::unordered_map<Inode*, Inode*> map;
    mutable std::shared_mutex mtx; // 读多写少：读共享，写独占
};

struct InodeSnapShort {
    int      last_index{-1};
    int      next{-1};
    int16_t  idx{-1};
    Key_t    gp_key{0};
    int      gp_value{-1};
    Key_t    sgp_key{0};
    int      sgp_value{-1};    int16_t  sgp_idx{-1};       // SGP position in inode's SGP array (-1 if no SGP)    
    // 命中槽位区间（用于验证 key 是否仍命中该槽）
    Key_t    lb_key{0};
    Key_t    ub_key{std::numeric_limits<Key_t>::max()};

    // 新增：在“确定 current 的那一瞬间”捕获的版本戳
    uint64_t ver_snap{0};
};

struct CachedVnodeImage {
    uint64_t bitmap;
    vnode_entry records[vnode_fanout];
};

// 线程本地缓存槽位
struct TlsVnodeCopyEntry {
    int           vnode_id{-1};
    BloomFilter*  bf{nullptr};
    uint64_t      bf_ver{0};
    Key_t         lb{0};
    Key_t         ub{std::numeric_limits<Key_t>::max()};
    CachedVnodeImage* img{nullptr};
    uint64_t      last_use{0}; // 新增: LRU 时间戳
};

// 线程本地缓存容器（环形替换）
struct TlsVnodeCopyCache {
    static constexpr size_t kCap = 8;

    // 预分配：每线程固定 kCap 份镜像缓冲，避免运行期 malloc
    alignas(64) CachedVnodeImage images[kCap];

    TlsVnodeCopyEntry slots[kCap];
    size_t hand{0};

    // 构造时把每个 slot 的 img 绑定到预分配缓冲
    TlsVnodeCopyCache() {
        for (size_t i = 0; i < kCap; ++i) {
            slots[i].vnode_id = -1;
            slots[i].bf       = nullptr;
            slots[i].bf_ver   = 0;
            slots[i].lb       = 0;
            slots[i].ub       = std::numeric_limits<Key_t>::max();
            slots[i].img      = &images[i];
            slots[i].last_use = 0;
            images[i].bitmap  = 0;
        }
    }

    // 可选：复位某个槽（不释放内存）
    void reset_slot(size_t i) {
        slots[i].vnode_id = -1;
        slots[i].bf       = nullptr;
        slots[i].bf_ver   = 0;
        slots[i].lb       = 0;
        slots[i].ub       = std::numeric_limits<Key_t>::max();
        slots[i].last_use = 0;
        images[i].bitmap  = 0;
    }
};

struct TLSShadowEntry {
    int        vnode_id{-1};
    uint64_t   version{0};     // BloomFilter::version
    uint64_t   bitmap{0};
    uint64_t   last_use{0};    // LRU 时间戳
    vnode_entry entries[vnode_fanout];
};

struct TLSShadowCache {
    static constexpr size_t kCap = 32; // 可调
    TLSShadowEntry slots[kCap];
    uint64_t clock{1};

    TLSShadowEntry* find(int vid) {
        for (auto &e : slots) if (e.vnode_id == vid) return &e;
        return nullptr;
    }
    TLSShadowEntry* victim() {
        size_t idx = 0; uint64_t oldest = std::numeric_limits<uint64_t>::max();
        for (size_t i = 0; i < kCap; ++i) {
            if (slots[i].vnode_id < 0) return &slots[i];
            if (slots[i].last_use < oldest) { oldest = slots[i].last_use; idx = i; }
        }
        return &slots[idx];
    }
};

class DramSkiplist {
private:
    //全局结构版本（split / rebalance 后 bump）
#ifdef ENABLE_L1_TLS_CACHE
    std::atomic<uint32_t> global_epoch{0};

    struct TlsPivot {
        Inode*   node{nullptr};
        Key_t    min_key{0};
        Key_t    upper_key{0};
        uint32_t epoch{0};
        uint8_t  fail_cnt{0};
        uint16_t hit_cnt{0};          // 新增：命中次数
    };
    static thread_local struct {
        TlsPivot pivots[TLS_PIVOT_MAX];            // 扩容：3 -> 8
        int used;
    } tls_pivot_set_;

    Inode* tls_try_match(Key_t key, int& start_level);
    void   tls_record_pivot(Inode* node);
    void   tls_mark_fail(Key_t min_key);
    void   bump_epoch(); // 在 split / rebalance 成功后调用
#endif

public:
    Inode* header[MAX_LEVEL];
    Inode* tail[MAX_LEVEL];
    DramInodePool *dramInodePool;
    CkptLog *ckpt_log;
    ValueList *valueList;
    std::atomic<int> level{0}; //level is the current max level of the skiplist
    // level_lock removed: replaced by atomic CAS on level
    vector<int> inode_count_on_each_level;

    // **新增：为查找操作设计的快速路径缓存**
    std::map<Key_t, Inode*> lookup_cache;
    std::shared_mutex cache_mutex;

    static constexpr size_t kNumShards = 64;

    std::array<CacheShard, kNumShards> cache_shards;
    std::hash<Key_t> key_hasher;

    // **新增：私有辅助函数**
    inline size_t shard_of(const Key_t key) {
        return key_hasher(key) % kNumShards;
    }
    Inode* find_start_node_from_cache_shards(Key_t key, int& start_level);
    void populate_cache_shards(Key_t key, Inode* node, int current_total_level);

    std::vector<Inode*> nodesCoveringRangeAtLevel(uint64_t a, uint64_t b, int level);
    Inode* nextAtLevel(Inode* n) const;


private:
    // 线程本地路标（跨函数共享）
    static thread_local Key_t  tls_pivot_key_;
    static thread_local Inode* tls_pivot_node_;
    // 新增：线程本地 vnode 深拷贝缓存
    static thread_local TlsVnodeCopyCache tls_vnode_copy_cache_;
    static thread_local uint64_t          tls_vnode_copy_lru_clock_; // 新增: LRU 时钟
    // 维护接口
    void invalidate_tls_pivot();
    void update_tls_pivot(Key_t key, Inode* node);

    Key_t get_node_upper_bound(Inode* node);

public:
    std::shared_mutex inode_locks[MAX_NODES];
    std::shared_mutex rebalance_lock;
    DramSkiplist(CkptLog *ckp_log, DramInodePool *dramInodePool, ValueList *valuelist);
    ~DramSkiplist();
    bool update(Key_t &oldKey, Key_t &newKey, Val_t &val);
    bool add(Vnode *targetVnode);
    // return the index in gps of the index node that poionts to the vnode
    //Inode *lookup(Key_t key, Inode *current, int currentHighestLevelIndex, std::shared_lock<std::shared_mutex> &current_lock, int &idx);
    Inode *lookup(Key_t key, Inode *current, int currentHighestLevelIndex, int &idx, InodeSnapShort &snap);
    Inode* lookupForInsert(Key_t key, Inode* &start, int level, int& idx, std::vector<Inode*>& updates);
    Inode* lookupForInsertWithSnap(Key_t key, Inode* &start,
                                   int currentHighestLevelIndex,
                                   int &idx, bool &is_sgp,
                                   std::vector<Inode*> &updates,
                                   InodeSnapShort &snap);

    // 新：强校验，带 key，且优先用 ver_snap 快速判定
    bool validateSnapShort(Inode* n, const InodeSnapShort& s, Key_t key) const;

    Inode *getHeader();
    Inode *getHeader(int level);
    void getPivotNodesForInsert(Key_t key, Inode* updates[]);
    bool rebalanceInode(Inode *inode, bool lastLevel);
    int generateRandomLevel();
    bool rebalanceIndex(Vnode &targetVnode);
    int rebalanceIdx(Vnode &targetVnode, Key_t targetKey);
    bool activateGP(Inode &inode);
    void setLevel(int level);
    int getLevel();

        // 新增：父关系表分片（2 的幂，便于位运算取模）
    static constexpr size_t kNumParentShards = 64;
    static_assert((kNumParentShards & (kNumParentShards - 1)) == 0, "kNumParentShards must be power of two");
    ParentShard parent_shards[kNumParentShards];

    // 快速分片函数：基于指针位做哈希，掐掉低位，避免邻近地址聚集
    inline size_t parent_shard_of(const Inode* child) const {
        uintptr_t x = reinterpret_cast<uintptr_t>(child);
        return (x >> 6) & (kNumParentShards - 1); // 跳过低 6 位
    }

    Inode* getParentInode(Inode* &child);
    //void removeInodeRelation(Inode* &child);
    void acquireLocksInOrder(std::vector<Inode*>& nodes, std::vector<std::unique_lock<std::shared_mutex>>& locks);
    void acquireWriteLocksInOrderByVersion(std::vector<Inode*>& nodes);
    void releaseWriteLocksInOrderByVersion(std::vector<Inode*>& nodes);
    int fastRebalance(Inode* &inode, Inode* &parent_inode);
    dram_log_entry_t *create_log_entry(Inode *inode);
    bool isTail(uint32_t id) {
        return (id >= MAX_LEVEL && id < 2 * MAX_LEVEL);
    }

    bool increaseLevel()
    {
        int cur = level.load(std::memory_order_acquire);
        while (cur < MAX_LEVEL - 1) {
            if (level.compare_exchange_weak(cur, cur + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return true;
        }
        return false;
    }
    void printStats();
    double calculateSearchEfficiency(long count);
    void fillInodeCountEachLevel(int level);

    bool find_and_verify_candidate_parent(Inode* inode, Inode* parent_hint, 
                              Inode*& candidate_parent, Inode*& candidate_next, 
                              Inode*& header_above);

#if ENABLE_DELTA_LOG
    void ckpt_log_single_slot_delta(CkptLog *log, Inode *inode, int16_t slot);
    void ckpt_log_multi_slots_delta(CkptLog *log, Inode *inode, const std::vector<int16_t> &slots);
#endif

    // 校验短快照是否仍然匹配当前 inode 状态（返回 true 表示未被并发修改）
    bool validateSnapShort(Inode* n, const InodeSnapShort& s) const;
};
