#include "common.h"
#include "checkpoint.h"
#include "dramSkiplist.h"
#include "pmemInodePool.h"
#include "recoveryManager.h"
#include "spinLock.h"
#include "valuelist.h"
#include "workerThread.h"
#include <boost/lockfree/spsc_queue.hpp>
#include <thread>
#pragma once

//extern std::queue<std::vector<ckp_entry *>*> g_checkpointQueue;
//extern boost::lockfree::spsc_queue<CheckpointVector *, boost::lockfree::capacity<1000000>> g_checkpointQueue;
//extern boost::lockfree::spsc_queue<ckp_entry *, boost::lockfree::capacity<1000000>> g_checkpointQueue;
extern std::queue<ckp_entry *> g_checkpointQueue;

class TandemIndex {
    public:
        TandemIndex();
        ~TandemIndex();

        bool insert(Key_t key, Val_t value);
        //void remove(int key);
        //void update(int key, int value);
        //void print();
        Val_t lookup(Key_t key);

        //std::thread *workerThread;
        std::thread *checkpointThread;

        //void createWorkerThread(); 
        void createCheckpointThread();
        void checkpointThreadExec(int id);
        //void workerThreadExec();

        DramSkiplist *mainIndex;
        DramInodePool *dramInodePool;
        PmemInodePool *pmemRecoveryArray;
        //PmemSkiplist *shadowIndex;
        ValueList *valueList;
        CheckpointQueue *cptq;
        RecoveryManager *recoveryManager;
};