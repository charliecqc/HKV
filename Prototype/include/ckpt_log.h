#include <vector>
#include <libpmem.h>
#include <libpmemobj.h>
#include "checkpoint.h"
#include "pmemManager.h"
#include "pmemInodePool.h"
#include "spinLock.h"
#include "common.h"

#pragma once

#define MAX_CKP_LOG_ENTRIES 10000

class log_entry_t {
public:
    Inode inode;
    log_entry_t(Inode inode) : inode(inode){};
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
    size_t log_size;
    size_t mask;
    bool isFull;
    

public:
    CkptLogNVM(size_t maxSize) : maxSize(maxSize), start(0), end(0), isFull(false) {
        root_obj *root = nullptr;
        init(root);
        start = 0;
        end = 0;
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
        return start == end;
    }

    size_t getSize() {
        if (isFull) {
            return maxSize;
        }
        if (end >= start) {
            return end - start;
        }
        return maxSize - start + end;
    }
};

class CkptLog {
    public:
    SpinLock g_ckptlock;
    CkptLogNVM *ckptlog;
    CkptLog(size_t maxSize) {
        ckptlog = new CkptLogNVM(maxSize);
    }
    ~CkptLog() {}
    void enq(Inode inode);
    log_entry_t *put_log_entry(CkptLogNVM *nvm_log, Inode inode);
    void enq(CkptLogNVM *nvm_log, Inode inode);
    log_entry_t *log_deq();
    log_entry_t *nvm_log_at(CkptLogNVM *nvm_log, size_t index);
    log_entry_t *nvm_log_enq(CkptLogNVM *nvm_log, size_t obj_size);
    log_entry_t *log_peek_head(CkptLogNVM *nvm_log);
    unsigned int nvm_log_index(CkptLogNVM *nvm_log, unsigned long index);
    void reclaim(PmemInodePool *pmemInodePool);
    bool isEmpty() {
        //std::unique_lock<std::mutex> lock(mtx);
        g_ckptlock.lock();
        bool ret = ckptlog->isEmpty();
        g_ckptlock.unlock();
        return ret;
    }
};


