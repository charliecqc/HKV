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

#ifndef WAL_DELTA_STRUCTS_DEFINED
#define WAL_DELTA_STRUCTS_DEFINED
enum WalLogType : uint16_t {
    WAL_LOG_TYPE_FULL  = 0,
    WAL_LOG_TYPE_DELTA = 1
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
#endif // WAL_DELTA_STRUCTS_DEFINED

class dram_log_entry_t {
public:
    log_entry_hdr hdr;
    int32_t gp_idx[fanout];
    Key_t key[fanout];
    Val_t value[fanout];
    int16_t covered_nodes[fanout/2]; 

    WalDeltaHeader delta_hdr{};                  // 新增：增量日志头
    WalDeltaEntry  delta_entries[fanout]{};      // 新增：增量槽位集合

    void initArrays()
    {
        memset(gp_idx, 0, sizeof(gp_idx));
        memset(key, 0, sizeof(key));
        memset(value, 0, sizeof(value));
        memset(covered_nodes, 0, sizeof(covered_nodes));
    }

    // **修改构造函数以匹配新的 log_entry_hdr**
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
        //return sizeof(Key_t) * activated_count + sizeof(Val_t) * activated_count + sizeof(int32_t) * activated_count;
    }

    inline void initDelta(int32_t inode_id,
                          int32_t last_index,
                          int32_t next) {
        delta_hdr.type = WAL_LOG_TYPE_DELTA;
        delta_hdr.count = 0;
        delta_hdr.inode_id = inode_id;
        delta_hdr.last_index = last_index;
        delta_hdr.next = next;
    }

    inline void pushDeltaSlot(int16_t slot,
                              const Key_t &k,
                              const Val_t &v,
                              int16_t covered) {
        auto idx = delta_hdr.count++;
        delta_entries[idx].slot = slot;
        delta_entries[idx].key = k;
        delta_entries[idx].value = v;
        delta_entries[idx].covered = covered;
    }

    inline size_t deltaPayloadSize() const {
        return sizeof(WalDeltaEntry) * delta_hdr.count;
    }
};

class CkptLogNVM {
private:
    std::string fileName = "/mnt/pmem1/ckpt_log";
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
    

public:
    CkptLogNVM(size_t maxSize) : maxSize(maxSize), start(0), end(0), isFull(false) {
        root_obj *root = nullptr;
        init(root,maxSize);
        start = 0;
        end = 0;
        current_update = 0;
        end_persistent = 0;
        start_persistent = 0;
        log_size = maxSize;
        // 要求 log_size 为 2 的幂
        mask = log_size - 1;
    }

    int init(root_obj *root, size_t maxSize);

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
    CkptLogNVM *ckptlog;

    // 游标
    AlignedAtomicSizeT a_consumed_start; // use: a_consumed_start.v
    AlignedAtomicSizeT a_produced_end;   // use: a_produced_end.v
    AlignedAtomicSizeT a_durable_end;    // use: a_durable_end.v

    std::atomic_flag flush_busy = ATOMIC_FLAG_INIT;

    std::atomic<int32_t> active_batchers{0};

    // === 新增：构造 / 析构 ===
    explicit CkptLog(size_t logSize = MAX_CKP_LOG_ENTRIES);
    ~CkptLog();

    // 原有整块写
    void enq(dram_log_entry_t *entry);

    log_entry_hdr *put_log_entry(dram_log_entry_t *entry);
    log_entry_hdr *nvm_log_enq(size_t entry_size);

    bool flushOnce();
    inline void backgroundFlushLoopStep() { flushOnce(); }

    size_t reclaimBatch(PmemInodePool *pmemInodePool, size_t max_bytes);
    void reclaim(PmemInodePool *pmemInodePool);
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
    // 新增：写入增量日志
#if ENABLE_DELTA_LOG
    // **修改 appendDeltaLog 签名**
    bool appendDeltaLog(int32_t inode_id,
                        int32_t last_index,
                        int32_t next,
                        int32_t parent_id,
                        const WalDeltaEntry *entries,
                        size_t entry_count);

    // **修改 enqDelta 签名**
    void enqDelta(int32_t inode_id,
                  int32_t last_index,
                  int32_t next,
                  int32_t parent_id,
                  const WalDeltaEntry *entries,
                  size_t entry_count);

    // **修改 applyDeltaEntries 签名**
    void applyDeltaEntries(Inode *inode,
                           const WalDeltaEntry *entries,
                           size_t entry_count,
                           int32_t new_last_index,
                           int32_t new_next,
                           int32_t new_parent_id);
#endif // ENABLE_DELTA_LOG

    // 查询队列间隙
    size_t getDurableGap() const;   // durable - consumed，可回放的持久字节
    size_t getProducedGap() const;  // produced - durable，已写未持久的小尾巴
    size_t getBacklogGap() const;   // produced - consumed，总积压

    // 尝试刷一次（按阈值），仅在有小尾巴时推进 durable_end
    bool tryFlushOnce();            // 封装内部 flushOnce()

    // 建议的单批回放字节（结合内部阈值）
    size_t suggestReclaimBatchBytes() const;

    // 已有：forcePersist(), reclaimBatch(...), forceReclaim(...) 等

    // 批量预留/提交（FULL/DELTA 通用）
    unsigned char* reserveChunk(size_t total_bytes_aligned);
    void commitChunk(size_t total_bytes_aligned);
    void enqBatch(const std::vector<dram_log_entry_t*>& entries);
    struct DeltaPack {
        WalDeltaHeader                 hdr;
        std::vector<WalDeltaEntry>    entries;
    };
    void enqDeltaBatch(const std::vector<DeltaPack>& packs);

    // 线程本地批处理器：保证“捕获顺序”->“写入顺序”
    class Batcher {
    public:
        explicit Batcher(CkptLog* owner)
            : owner_(owner), last_flush_(Clock::now()) {
            owner_->active_batchers.fetch_add(1, std::memory_order_acq_rel);
            }

        // 捕获 FULL（保持最小改动）
        void addFull(dram_log_entry_t* e);

        // 捕获单槽 DELTA（保持最小改动）
        void addDeltaSlot(int32_t inode_id,
                          int32_t last_index,
                          int32_t next,
                          int32_t parent_id,
                          int16_t slot,
                          const Key_t& key,
                          const Val_t& value,
                          int16_t covered);

        void flush();              // 确保使用 move 并清空本地缓冲
        void detach() noexcept { 
            if(!detached_) {
                detached_ = true; 
                owner_->active_batchers.fetch_sub(1, std::memory_order_acq_rel);
            }
        }  // 禁止析构期再 flush
        ~Batcher() {
            if (!detached_) {
                flush();
                owner_->active_batchers.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
    private:
        using Clock = std::chrono::steady_clock;
        bool detached_{false};     // 新增
        // 假设持有指向所属 CkptLog 的指针（已有）
        // CkptLog* owner_;
        // std::vector<dram_log_entry_t*> buf_;
        // 阈值：可按压测调整
        static constexpr size_t  kMaxEntries   = 256;         // 事件条数阈值（FULL+DELTA）
        static constexpr size_t  kMaxBytes     = 512 * 1024; // 估算字节阈值
        static constexpr int64_t kMaxDelayNs   = 400000;     // 400us

        enum class Kind : uint8_t { Full, Delta };

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
        struct Event {
            Kind    kind;
            uint64_t seq; // 本线程捕获序号
            // 简单变体
            EvFull  f;
            EvDelta d;
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


