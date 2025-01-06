#ifndef TANDEM_INDEX_H
#define TANDEM_INDEX_H

#include "dramSkiplist.h"
#include "pmemSkiplist.h"
#include "valuelist.h"
#include "common.h"
#include "spinLock.h"
#include "sampler.h"
#include <thread>

#pragma once



class TandemIndex {
    public:
        TandemIndex();
        ~TandemIndex();

        bool insert(Key_t key, Val_t value);
        //void remove(int key);
        //void update(int key, int value);
        //void print();
        Val_t lookup(Key_t key);

        std::thread *workerThread;
        std::thread *checkpointThread;

        void createWorkerThread(); 
        void createCheckpointThread();
        void workerThreadExec();

        DramSkiplist *mainIndex;
        ValueList *valueList;
        //PmemSkiplist *shadowIndex;

        //sampling - moved to value list
        Sampler* sampler;
};

#endif // TANDEM_INDEX_H