#include <vector>
#include <libpmem.h>
#include <libpmemobj.h>
#include "checkpoint.h"
#include "pmemManager.h"
#include "pmemInodePool.h"
#include "spinLock.h"
#include "common.h"
#include "node.h"
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <tuple>
#include "valuelist.h"

#pragma once

#define MAX_CKP_LOG_ENTR

#ifndef MAX_CKP_LOG_ENTRIES
// 默认日志大小：64MB，可按需调整
#define MAX_CKP_LOG_ENTRIES (64UL * 1024 * 1024)
#endif

class nvm_log_entry_t {
public:
    int32_t gp_idx;
    Key_t key;
    Val_t value;
    // **新增：为持久化条目添加覆盖数**
    int16_t covered_nodes;
};

class log_entry_hdr {
public:
    int32_t id; //id of inode that has modifications
    int16_t count; // number of entries in the log
    // **移除：不再需要全局覆盖数**
    // int16_t coveredNodes; 
    int16_t last_index; // last valid gp of the inode
    int32_t next; // next inode id
    int16_t level;
    int32_t parent_id; // **新增：父节点ID**
    // **修改构造函数：移除 coveredNodes 参数**
    log_entry_hdr(int32_t id, int16_t last_index, int32_t next, int16_t level, int32_t parent_id) : id(id), last_index(last_index), next(next), level(level), parent_id(parent_id) {
        count = 0;
    }

    size_t getPayLoadSize() {
        size_t activated_count = count;
        return sizeof(nvm_log_entry_t) * activated_count;
    }
};

#ifndef ENABLE_DELTA_LOG
#define ENABLE_DELTA_LOG 1
#endif

#ifndef ENABLE_IMMEDIATE_FLUSH
#define ENABLE_IMMEDIATE_FLUSH 0
#endif

#ifndef ENABLE_IMMEDIATE_RECLAIM
#define ENABLE_IMMEDIATE_RECLAIM 0
#endif

#ifndef WAL_DELTA_STRUCTS_DEFINED
#define WAL_DELTA_STRUCTS_DEFINED
enum WalLogType : uint16_t {
    WAL_LOG_TYPE_FULL   = 0,
    WAL_LOG_TYPE_DELTA  = 1,
    WAL_LOG_TYPE_INSERT = 2      // insert-delta: single GP insertion at a position
};
static constexpr int32_t WAL_META_KEEP = -1;

struct WalDeltaHeader {
    uint16_t type;       // WAL_LOG_TYPE_DELTA
    uint16_t count;      // number of WalDeltaEntry
    int32_t  inode_id;
    int32_t  last_index;
    int32_t  next;
    int32_t  parent_id;  // **新增**
};

struct WalDeltaEntry {
    int16_t slot;
    int16_t covered;
    Key_t   key;
    Val_t   value;
};
// Compact insert-delta: records a single GP insertion at a given position.
// Recovery shifts slots [insert_pos .. old_last_index] right by 1 then writes the new slot.
struct WalInsertHeader {
    uint16_t type;           // WAL_LOG_TYPE_INSERT
    int16_t  insert_pos;     // position where the new GP was inserted
    int32_t  inode_id;
    int32_t  last_index;     // new last_index (after insert)
    int32_t  next;           // WAL_META_KEEP if unchanged
    int32_t  parent_id;      // WAL_META_KEEP if unchanged
    Key_t    key;            // inserted key
    Val_t    value;          // inserted value (vnode id)
    int16_t  covered;        // initial covered count
    int16_t  _pad{0};        // alignment padding
};

#endif // WAL_DELTA_STRUCTS_DEFINED

class dram_log_entry_t {
public:
    log_entry_hdr hdr;
    int32_t gp_idx[fanout];
    Key_t key[fanout];
    Val_t value[fanout];
    int16_t covered_nodes[fanout/2]; 

    // delta fields removed: Batcher handles delta logs independently via EvDelta

    void initArrays()
    {
        memset(gp_idx, 0, sizeof(gp_idx));
        memset(key, 0, sizeof(key));
        memset(value, 0, sizeof(value));
        memset(covered_nodes, 0, sizeof(covered_nodes));
    }

    dram_log_entry_t(int32_t id, int16_t last_index, int32_t next, int16_t level, int32_t parent_id) : hdr(id, last_index, next, level, parent_id) {
        initArrays();
    }

    // **修改 setKeyVal 以包含 covered_nodes，使其更健壮**
    void setKeyVal(int32_t idx, Key_t key, Val_t value, int16_t covered) {
        gp_idx[hdr.count] = idx;
        this->key[hdr.count] = key;
        this->value[hdr.count] = value;
        this->covered_nodes[hdr.count] = covered; // 直接使用 hdr.count 作为索引
        hdr.count += 1;
    }

    void initHeader(int32_t id, int16_t last_index, int32_t next, int16_t level) {
        this->hdr.id = id;
        //this->hdr.coveredNodes = coveredNodes;
        this->hdr.last_index = last_index;
        this->hdr.next = next;
        this->hdr.level = level;
    }

    size_t getLoadCount() {
        return hdr.count;
    }

    void setLastIndex(int16_t last_index) {
        this->hdr.last_index = last_index;
    }

    void setNext(int32_t next) {
        this->hdr.next = next;
    }

    void setId(int32_t id) {
        this->hdr.id = id;
    }

    size_t getPayLoadSize() {
        size_t activated_count = hdr.count;
        return sizeof(nvm_log_entry_t) * activated_count;
    }
};

#ifndef ENABLE_LOG_REPLAY
#define ENABLE_LOG_REPLAY 1
#endif

#if ENABLE_LOG_REPLAY
// Persistent metadata for crash recovery (stored in NVM via root->ptr[1])
struct CkptLogPersistMeta {
    size_t start_cursor;   // last consumed position
    size_t end_cursor;     // last durable position
    uint64_t magic;        // validity marker
};
static constexpr uint64_t CKPT_LOG_META_MAGIC = 0x434B50544C4F4721ULL; // "CKPTLOG!"
#endif

class CkptLogNVM {
private:
    std::string fileName;
public:
    volatile unsigned char *_buf; // buffer for checkpoint log
    volatile unsigned char *buf; // cacheline alighed buffer for checkpoint log
    size_t maxSize;
    size_t start;
    size_t end;
    size_t current_update;
    size_t end_persistent;
    size_t start_persistent;
    size_t log_size;
    size_t mask;
    bool isFull;
#if ENABLE_LOG_REPLAY
    CkptLogPersistMeta *nvm_meta = nullptr;
    bool wasRecovered = false;
#endif
    

public:
    CkptLogNVM(size_t maxSize,std::string storage_path) : maxSize(maxSize), start(0), end(0), isFull(false) {
        fileName = storage_path + "/ckpt_log";
        init(maxSize);
#if ENABLE_LOG_REPLAY
        if (wasRecovered && nvm_meta &&
            nvm_meta->magic == CKPT_LOG_META_MAGIC &&
            nvm_meta->end_cursor > nvm_meta->start_cursor) {
            start_persistent = nvm_meta->start_cursor;
            end_persistent = nvm_meta->end_cursor;
            start = nvm_meta->start_cursor;
            end = nvm_meta->end_cursor;
            current_update = nvm_meta->end_cursor;
        } else
#endif
        {
            start = 0;
            end = 0;
            current_update = 0;
            end_persistent = 0;
            start_persistent = 0;
        }
        log_size = maxSize;
        // 要求 log_size 为 2 的幂
        mask = log_size - 1;
    }

    int init(size_t maxSize);
#if ENABLE_LOG_REPLAY
    void persistCursors();
#endif

    ~CkptLogNVM() {
        // Deallocate memory blocks
        //delete _buf;
        //delete buf;
    }

    bool isEmpty() {
        if(start == end) {
            return true;
        }else if(start > end) {
            cout << "Log is over empty " << "ckptlog->start: " << start << " ckptlog->end: "<< end<< endl;
            assert(false);
        }
        return false;
    }

    size_t getLogQueueSize() {
        if (end > start) {
            return end - start;
        }else if(start == end) {
            return 0;
        }else {
            cout << "Log is over empty " << "ckptlog->start: " << start << " ckptlog->end: "<< end<< endl;
            return -1;
        }
    }


};

struct alignas(64) AlignedAtomicSizeT {
    std::atomic<size_t> v;
    char pad[64 - sizeof(std::atomic<size_t>)]{};
};

class CkptLog {
public:
    std::shared_mutex mtx;
    int retry_count;
    uint64_t reclaim_exec_count;
    CkptLogNVM *ckptlog;
    ValueList *valueList;
    int current_highest_level;
    long current_inode_idx;
    vector<int> inode_count_on_each_level;

    // 游标
    AlignedAtomicSizeT a_consumed_start; // use: a_consumed_start.v
    AlignedAtomicSizeT a_alloc_end;      // 已分配但可能未写完的边界
    AlignedAtomicSizeT a_produced_end;   // 已写完并可 CLWB 的边界
    AlignedAtomicSizeT a_durable_end;    // 已 CLWB+SFENCE 的边界

    std::atomic_flag flush_busy = ATOMIC_FLAG_INIT;

    std::atomic<int32_t> active_batchers{0};
    std::string storagePath;

    #if ENABLE_PMEM_STATS
    explicit CkptLog(size_t logSize = MAX_CKP_LOG_ENTRIES, int current_highest_level = 0, ValueList *va_list = nullptr, std::string storage_path = "");
    ~CkptLog();
    #else
    explicit CkptLog(size_t logSize = MAX_CKP_LOG_ENTRIES, std::string storage_path = "");
    ~CkptLog();
    #endif

    // 原有整块写
    void enq(dram_log_entry_t *entry);

    log_entry_hdr *put_log_entry(dram_log_entry_t *entry);
    log_entry_hdr *nvm_log_enq(size_t entry_size);

    bool flushOnce();
    double calculatePmemSearchEfficiency(long vnode_count);
    inline void backgroundFlushLoopStep() { flushOnce(); }

    size_t reclaimBatch(PmemInodePool *pmemInodePool, size_t max_bytes);
    void reclaim(double dramSearchEfficincy, long vnode_count, PmemInodePool *pmemInodePool);
    void forceReclaim(PmemInodePool *pmemInodePool);

    void waitDurable(size_t lsn);

    bool isLogEmpty();
    size_t getLogQueueSize();

    unsigned int nvm_log_index(unsigned long idx);
    log_entry_hdr *nvm_log_at(size_t index);

    void forcePersist();

    void initLogEntryHeaderFromDramLogEntry(log_entry_hdr *dst, const dram_log_entry_t *src);

    void drainAndPerssistBatchers() {
        auto &b = batcher();
        b.flush();
        b.detach();
        while(active_batchers.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }

        forcePersist();
    }

    void drainAndPersistOnce(){
        auto &b = batcher();
        b.flush();
        forcePersist();
    }
#if ENABLE_DELTA_LOG
    bool appendDeltaLog(int32_t inode_id,
                        int32_t last_index,
                        int32_t next,
                        int32_t parent_id,
                        const WalDeltaEntry *entries,
                        size_t entry_count);

    void enqDelta(int32_t inode_id,
                  int32_t last_index,
                  int32_t next,
                  int32_t parent_id,
                  const WalDeltaEntry *entries,
                  size_t entry_count);

    void applyDeltaEntries(Inode *inode,
                           const WalDeltaEntry *entries,
                           size_t entry_count,
                           int32_t new_last_index,
                           int32_t new_next,
                           int32_t new_parent_id);

    bool appendInsertLog(const WalInsertHeader &ih);
    void enqInsertBatch(const std::vector<WalInsertHeader>& hdrs);

    void applyInsertEntry(Inode *inode, const WalInsertHeader *ih);
#endif // ENABLE_DELTA_LOG

    size_t getDurableGap() const;   // durable - consumed, persisted bytes that can be reclaimed
    size_t getProducedGap() const;  // produced - durable， bytes waiting to be persisted
    size_t getBacklogGap() const;   // produced - consumed， bytes waiting to be processed

    //try to flush once to move the durable edge forward
    bool tryFlushOnce();          

    size_t suggestReclaimBatchBytes() const;

#if ENABLE_LOG_REPLAY
    struct LogReplayStats {
        size_t total_bytes       = 0;
        size_t full_entries      = 0;
        size_t delta_entries     = 0;
        size_t insert_entries    = 0;
        size_t total_entries     = 0;
        double replay_time_ms    = 0.0;
        bool   was_replay_needed = false;
    };

    LogReplayStats replayLog(PmemInodePool *pmemInodePool);
    bool wasLogRecovered() const { return ckptlog->wasRecovered; }
#endif

    unsigned char* reserveChunk(size_t total_bytes_aligned, size_t& out_alloc_start);
    void commitChunk(size_t alloc_start, size_t total_bytes_aligned);
    void enqBatch(const std::vector<dram_log_entry_t*>& entries);
    struct DeltaPack {
        WalDeltaHeader                 hdr;
        std::vector<WalDeltaEntry>    entries;
    };
    void enqDeltaBatch(const std::vector<DeltaPack>& packs);

    class Batcher {
    public:
        explicit Batcher(CkptLog* owner)
            : owner_(owner), last_flush_(Clock::now()) {
            owner_->active_batchers.fetch_add(1, std::memory_order_acq_rel);
            }

        void addFull(dram_log_entry_t* e);

        void addInsertSlot(int32_t inode_id,
                           int32_t last_index,
                           int32_t next,
                           int32_t parent_id,
                           int16_t insert_pos,
                           const Key_t& key,
                           const Val_t& value,
                           int16_t covered);

        void addDeltaSlot(int32_t inode_id,
                          int32_t last_index,
                          int32_t next,
                          int32_t parent_id,
                          int16_t slot,
                          const Key_t& key,
                          const Val_t& value,
                          int16_t covered);

        void flush();
        void detach() noexcept { 
            if(!detached_) {
                detached_ = true; 
                owner_->active_batchers.fetch_sub(1, std::memory_order_acq_rel);
            }
        }

        ~Batcher() {
            if (!detached_) {
                flush();
                owner_->active_batchers.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
    private:
        using Clock = std::chrono::steady_clock;
        bool detached_{false}; 

        static constexpr size_t  kMaxEntries   = 4096;         //threshold of number of entries
        static constexpr size_t  kMaxBytes     = 4 * 1024 * 1024; //threshold of bytes
        static constexpr int64_t kMaxDelayNs   = 2000000;     //threshold of delay in nanoseconds

        enum class Kind : uint8_t { Full, Delta, Insert };

        struct EvFull {
            dram_log_entry_t* e;
            size_t aligned; // 预计算对齐后大小
        };
        struct EvDelta {
            // 一个“单槽”增量条目
            WalDeltaHeader hdr;
            WalDeltaEntry  entry;
            size_t aligned; // 预估对齐大小（用于阈值控制，最终会在 run 内聚合）
        };
        struct EvInsert {
            WalInsertHeader hdr;
            size_t aligned;
        };
        struct Event {
            Kind    kind;
            uint64_t seq; // 本线程捕获序号
            // Only one of these is valid, determined by 'kind'
            union {
                EvFull   f;
                EvDelta  d;
                EvInsert ins;
            };
        };

        void maybeFlush();

        CkptLog* owner_;
        inline static thread_local uint64_t s_seq_; // 本线程单调递增序号

        std::vector<Event> events_;          // 按捕获顺序追加
        size_t             bytes_est_{0};    // 粗略估计（run 聚合后会更小）
        Clock::time_point  last_flush_;
    };

    Batcher& batcher() { thread_local Batcher b(this); return b; }
};


