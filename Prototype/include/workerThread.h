#include "common.h"
#include "checkpoint.h"
#include "pmemInodePool.h"
#include <queue>
#include <boost/lockfree/spsc_queue.hpp>
#pragma once

enum Operation {
    INSERT = 0,
    DELETE = 1,
    UPDATE = 2,
    LOOKUP = 3
};

class wq_entry{
    Key_t key;
    Val_t value;
    int ops;
    wq_entry(Key_t _key, Val_t _value, int _ops) {
        this->key = _key;
        this->value = _value;
        this->ops = _ops;
    }
};

class WorkerThread {
public:
    WorkerThread();
    ~WorkerThread();
    void workerOperation();
};

class CheckpointThread {
private:
    //std::queue<std::vector<ckp_entry *>*> *checkpointQueue;
    CheckpointQueue *cptq;
    PmemInodePool *pmemInodePool;
    int id;
public:
    CheckpointThread(int tid, CheckpointQueue *cq, PmemInodePool *pmemInodePool);
    ~CheckpointThread();
    void checkpointOperation();
    bool isCheckpointQueueEmpty();
};

