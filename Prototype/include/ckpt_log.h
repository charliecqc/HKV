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

class log_entry_t {
public:
    Inode inode;
    log_entry_t(Inode inode) : inode(inode){};
};

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

#if 0
    dram_log_entry_t(){
        this->hdr.id = -1;
        this->hdr.coveredNodes = 0;
        this->hdr.last_index = -1;
        this->hdr.next = -1;
        initArrays();
    }
#endif

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

class entry_t {
    public:
    entry_t(int16_t id, int16_t next) : id(id), next(next) {
        this->gp_idx = new int16_t[fanout];
        this->key = new Key_t[fanout];
        this->value = new Val_t[fanout];
        this->coveredNodes = 0;
        this->last_index = -1;
    }

    entry_t(int16_t id) : id(id) {
        this->gp_idx = new int16_t[fanout];
        this->key = new Key_t[fanout];
        this->value = new Val_t[fanout];
        this->coveredNodes = 0;
        this->last_index = -1;
    }
    int16_t id; //2 bytes
    int16_t coveredNodes;
    int16_t last_index;
    int16_t next; // 2 bytes
    size_t count;

    int16_t *gp_idx; //0-fanout/2-1: gp, fanout/2-fanout-1: sgp, fanout:next
    Key_t *key; 
    Val_t *value;

    size_t getObjSize() {
        size_t tem_size = sizeof(Key_t) * count + sizeof(Val_t) * count + sizeof(int16_t) * count;
        if(next != -1) {
            tem_size += sizeof(next);
        }
        return tem_size;
    }

    void setLogKeyVal(int16_t idx, Key_t key, Val_t value) {
        gp_idx[count] = idx;
        this->key[count] = key;
        this->value[count] = value;
        count += 1;
    }

    void setCoveredNodes(int16_t coveredNodes) {
        this->coveredNodes = coveredNodes;
    }

    void setLogNext(int16_t next) {
        this->next = next;
    }

    void setLastIndex(int16_t last_index) {
        this->last_index = last_index;
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
    std::shared_mutex mtx;
    CkptLogNVM *ckptlog;
    CkptLog(size_t maxSize) {
        ckptlog = new CkptLogNVM(maxSize);
    }
    ~CkptLog() {}
    //void enq(Inode inode);
    void enq(dram_log_entry_t *entry);
    //log_entry_t *put_log_entry(Inode inode);
    log_entry_hdr *put_log_entry(dram_log_entry_t *entry);
    //log_entry_t *log_deq();
    log_entry_hdr *log_deq();
    //log_entry_t *nvm_log_at(size_t index);
    log_entry_hdr *nvm_log_at(size_t index);
    //log_entry_t *nvm_log_enq(size_t obj_size);
    log_entry_hdr *nvm_log_enq(size_t obj_size);
    //log_entry_t *log_peek_head();
    log_entry_hdr *log_peek_head();
    unsigned int nvm_log_index(unsigned long index);
    void reclaim(PmemInodePool *pmemInodePool);
    bool isLogEmpty() {
        bool ret = ckptlog->isEmpty();
        return ret;
    }
};


