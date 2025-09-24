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
    int16_t id; //id of inode that has modifications
    int16_t count; // number of entries in the log
    // **移除：不再需要全局覆盖数**
    // int16_t coveredNodes; 
    int16_t last_index; // last valid gp of the inode
    int16_t next; // next inode id
    int16_t level;
    // **修改构造函数：移除 coveredNodes 参数**
    log_entry_hdr(int16_t id, int16_t last_index, int16_t next, int16_t level) : id(id), last_index(last_index), next(next), level(level) {
        count = 0;
    }

    size_t getPayLoadSize() {
        size_t activated_count = count;
        return sizeof(nvm_log_entry_t) * activated_count;
    }
};

class dram_log_entry_t {
public:
    log_entry_hdr hdr;
    int32_t gp_idx[fanout];
    Key_t key[fanout];
    Val_t value[fanout];
    int16_t covered_nodes[fanout/2]; 

    void initArrays()
    {
        memset(gp_idx, 0, sizeof(gp_idx));
        memset(key, 0, sizeof(key));
        memset(value, 0, sizeof(value));
        memset(covered_nodes, 0, sizeof(covered_nodes));
    }

    // **修改构造函数以匹配新的 log_entry_hdr**
    dram_log_entry_t(int16_t id, int16_t last_index, int16_t next, int16_t level) : hdr(id, last_index, next, level) {
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

    void initHeader(int16_t id, int16_t last_index, int16_t next, int16_t level) {
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

    void setNext(int16_t next) {
        this->hdr.next = next;
    }

    void setId(int16_t id) {
        this->hdr.id = id;
    }

    size_t getPayLoadSize() {
        size_t activated_count = hdr.count;
        return sizeof(nvm_log_entry_t) * activated_count;
        //return sizeof(Key_t) * activated_count + sizeof(Val_t) * activated_count + sizeof(int32_t) * activated_count;
    }
};

class CkptLogNVM {
private:
    std::string fileName = "/mnt/pmem0/ckpt_log";
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
        init(root);
        start = 0;
        end = 0;
        current_update = 0;
        end_persistent = 0;
        start_persistent = 0;
        log_size = maxSize;
        // 要求 log_size 为 2 的幂
        mask = log_size - 1;
    }

    int init(root_obj *root);

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

class CkptLog {
public:
    std::shared_mutex mtx;
    int retry_count;
    CkptLogNVM *ckptlog;

    // 游标
    std::atomic<size_t> a_consumed_start{0};
    std::atomic<size_t> a_produced_end{0};
    std::atomic<size_t> a_durable_end{0};

    std::atomic_flag flush_busy = ATOMIC_FLAG_INIT;

    // === 新增：构造 / 析构 ===
    explicit CkptLog(size_t logSize = MAX_CKP_LOG_ENTRIES);
    ~CkptLog();

    // 便捷封装
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
};


