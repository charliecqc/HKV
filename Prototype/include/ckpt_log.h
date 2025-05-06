#include <vector>
#include <libpmem.h>
#include <libpmemobj.h>
#include "checkpoint.h"
#include "pmemManager.h"
#include "pmemInodePool.h"
#include "spinLock.h"
#include "common.h"
#include "node.h"

#pragma once

#define MAX_CKP_LOG_ENTR

class log_entry_hdr {
public:
    int16_t id; //id of inode that has modifications
    int16_t count; // number of entries in the log
    int16_t coveredNodes; // number of covered nodes
    int16_t last_index; // last valid gp of the inode
    int16_t next; // next inode id
    log_entry_hdr(int16_t id, int16_t coveredNodes, int16_t last_index, int16_t next) : id(id), coveredNodes(coveredNodes), last_index(last_index), next(next) {
        count = 0;
    }

    size_t getPayLoadSize() {
        size_t activated_count = count;
        return sizeof(Key_t) * activated_count + sizeof(Val_t) * activated_count + sizeof(int32_t) * activated_count;
    }
};

class nvm_log_entry_t {
public:
    int32_t gp_idx;
    Key_t key;
    Val_t value;
};

class dram_log_entry_t {
public:
    log_entry_hdr hdr;
    int32_t gp_idx[fanout];
    Key_t key[fanout];
    Val_t value[fanout];
    void initArrays()
    {
        memset(gp_idx, 0, sizeof(gp_idx));
        memset(key, 0, sizeof(key));
        memset(value, 0, sizeof(value));
    }

    dram_log_entry_t(int16_t id, int16_t coveredNodes, int16_t last_index, int16_t next) : hdr(id, coveredNodes, last_index, next) {
        initArrays();
    }

    void setKeyVal(int32_t idx, Key_t key, Val_t value) {
        gp_idx[hdr.count] = idx;
        this->key[hdr.count] = key;
        this->value[hdr.count] = value;
        hdr.count += 1;
    }

    void initHeader(int16_t id, int16_t coveredNodes, int16_t last_index, int16_t next) {
        this->hdr.id = id;
        this->hdr.coveredNodes = coveredNodes;
        this->hdr.last_index = last_index;
        this->hdr.next = next;
    }

    size_t getLoadCount() {
        return hdr.count;
    }

    void setCoveredNodes(int16_t coveredNodes) {
        this->hdr.coveredNodes = coveredNodes;
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
        return sizeof(Key_t) * activated_count + sizeof(Val_t) * activated_count + sizeof(int32_t) * activated_count;
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
        init(root);
        start = 0;
        end = 0;
        current_update = 0;
        end_persistent = 0;
        start_persistent = 0;
        log_size = maxSize;
        mask = (~(log_size - 1));
    }

    int init(root_obj *root);

    ~CkptLogNVM() {
        // Deallocate memory blocks
        delete _buf;
        delete buf;
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
    SpinLock g_ckptlock;
    std::shared_mutex mtx;
    int retry_count;
    CkptLogNVM *ckptlog;
    CkptLog(size_t maxSize) {
        ckptlog = new CkptLogNVM(maxSize);
        retry_count = 0;
    }
    ~CkptLog() {}
    //void enq(Inode inode);
    void initInodeFromLogEntry(Inode *inode, log_entry_hdr *entry_hdr) {
        inode->hdr.id = entry_hdr->id;
        inode->hdr.coveredNodes = entry_hdr->coveredNodes;
        inode->hdr.last_index = entry_hdr->last_index;
        inode->hdr.next = entry_hdr->next;
    }

    void initLogEntryHeaderFromDramLogEntry(log_entry_hdr *entry_hdr, dram_log_entry_t *entry) {
        entry_hdr->id = entry->hdr.id;
        entry_hdr->count = entry->hdr.count;
        entry_hdr->next = entry->hdr.next;
        entry_hdr->coveredNodes = entry->hdr.coveredNodes;
        entry_hdr->last_index = entry->hdr.last_index;
    }

    void enq(dram_log_entry_t *entry);
    log_entry_hdr *put_log_entry(dram_log_entry_t *entry);
    log_entry_hdr *log_deq();
    log_entry_hdr *nvm_log_at(size_t index);
    log_entry_hdr *nvm_log_enq(size_t obj_size);
    log_entry_hdr *log_peek_head();
    unsigned int nvm_log_index(unsigned long index);
    void reclaim(PmemInodePool *pmemInodePool);
    void forceReclaim(PmemInodePool *pmemInodePool);
    bool isLogEmpty() {
        bool ret = ckptlog->isEmpty();
        return ret;
    }
    size_t getLogQueueSize() {
        return ckptlog->getLogQueueSize();
    }
};


