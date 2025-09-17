#include "skiplist.h"
#include "checkpoint.h"
#include "dramInodePool.h"
#include "pmemVnodePool.h"
#include "ckpt_log.h"
#include "common.h"
#include "valuelist.h"
#include <map>
#include <shared_mutex>
#pragma once// SkipList class

class CacheShard {
public:
    std::map<Key_t, Inode*> table;
    mutable std::shared_mutex mtx; // 共享互斥锁，允许多个
};

class DramSkiplist {
private:
    // 全局结构版本（split / rebalance 后 bump）
    std::atomic<uint32_t> global_epoch{0};

    struct TlsPivot {
        Inode*  node{nullptr};
        Key_t   min_key{0};
        Key_t   upper_key{0};
        uint32_t epoch{0};
        uint8_t fail_cnt{0};
    };
    static thread_local struct {
        TlsPivot pivots[3];
        int used;
    } tls_pivot_set_;

    Inode* tls_try_match(Key_t key, int& start_level);
    void   tls_record_pivot(Inode* node);
    void   tls_mark_fail(Key_t min_key);
    void   bump_epoch(); // 在 split / rebalance 成功后调用

public:
    Inode* header[MAX_LEVEL];
    Inode* tail[MAX_LEVEL];
    DramInodePool *dramInodePool;
    CkptLog *ckpt_log;
    ValueList *valueList;
    int level; //level is the current max level of the skiplist
    std::shared_mutex level_lock;

    std::mutex inodeRelationMutex;
    std::unordered_map<Inode*, Inode*> childToParentMap; // map to store child-parent relationships for rebalancing

    // **新增：为查找操作设计的快速路径缓存**
    std::map<Key_t, Inode*> lookup_cache;
    std::shared_mutex cache_mutex;

    static constexpr size_t kNumShards = 32;

    std::array<CacheShard, kNumShards> cache_shards;
    std::hash<Key_t> key_hasher;

    // **新增：私有辅助函数**
    inline size_t shard_of(const Key_t key) {
        return key_hasher(key) % kNumShards;
    }
    Inode* find_start_node_from_cache(Key_t key, int& start_level);
    Inode* find_start_node_from_cache_shards(Key_t key, int& start_level);
    void populate_cache(Key_t key, Inode* node);
    void populate_cache(Key_t key, Inode* node, int current_total_level);
    void populate_cache_shards(Key_t key, Inode* node, int current_total_level);


private:
    // 线程本地路标（跨函数共享）
    static thread_local Key_t  tls_pivot_key_;
    static thread_local Inode* tls_pivot_node_;

    // 维护接口
    void invalidate_tls_pivot();
    void update_tls_pivot(Key_t key, Inode* node);

    Key_t get_node_upper_bound(Inode* node);

public:
    std::shared_mutex inode_locks[MAX_NODES];
    std::shared_mutex rebalance_lock;
    DramSkiplist(CkptLog *ckp_log, DramInodePool *dramInodePool, ValueList *valuelist);
    ~DramSkiplist();
    bool insert(Key_t &key, Val_t &val);
    bool insert(Key_t &key, Val_t &val, Inode *inodes[], int newlevel);
    bool insert(Vnode *targetVnode);
    bool update(Key_t &oldKey, Key_t &newKey, Val_t &val);
    bool add(Vnode *targetVnode);
    // return the index in gps of the index node that poionts to the vnode
    Inode *lookup(Key_t key, int &idx);
    Inode *lookup(Key_t key, Inode *current, int currentHighestLevelIndex, std::shared_lock<std::shared_mutex> &current_lock, int &idx);
    Inode *lookupWithSGP(Key_t key, int &idx, bool &sgp_used);
    Inode *lookupWithSGP(Key_t key, Inode *current, int currentHighestLevelIndex, std::shared_lock<std::shared_mutex> &current_lock, int &idx, bool &sgp_used);
    Inode *lookupForInsert(Key_t key, Inode * &current, int currentHighestLevelIndex, std::shared_lock<std::shared_mutex> &current_lock, int &idx, std::vector<Inode *> &updates);
    Inode *lookupForInsertWithSGP(Key_t key, Inode * &current, int currentHighestLevelIndex, std::shared_lock<std::shared_mutex> &current_lock, int &idx, std::vector<Inode *> &updates, bool &sgp_used);
    Inode *getHeader();
    Inode *getHeader(int level);
    void getPivotNodesForInsert(Key_t key, Inode* updates[]);
    bool checkForActivateGP(Inode &inode);
    bool checkForRebalance(Inode &inode, bool &activeNewGP);
    bool rebalanceInode(Inode *inode, bool lastLevel);
    int generateRandomLevel();
    bool rebalanceInode(Inode &inode);
    bool rebalanceInode(Inode &inode, Vnode &vnode);
    bool rebalanceIndex(Vnode &targetVnode);
    int rebalanceIdx(Vnode &targetVnode, Key_t targetKey);
    bool activateGP(Inode &inode);
    void setLevel(int level);
    int getLevel();
    void recordInodeRelation(Inode* &child, Inode* &parent);
    Inode *getParentInode(Inode* &child);
    void removeInodeRelation(Inode* &child);
    void acquireLocksInOrder(std::vector<Inode*>& nodes, std::vector<std::unique_lock<std::shared_mutex>>& locks);
    int fastRebalance(Inode* &inode, Inode* &parent_inode);
    int fastRebalance1(Inode* &inode, Inode* &parent_inode);
    dram_log_entry_t *create_log_entry(Inode *inode);
    bool isTail(int16_t id) {
        return (id >= MAX_LEVEL && id < 2 * MAX_LEVEL);
    }

    bool increaseLevel()
    {
        std::unique_lock<std::shared_mutex> lock(level_lock);
        if (level < MAX_LEVEL - 1) {
            level++;
            return true;
        }
        return false;
    }
    void printStats();
};
