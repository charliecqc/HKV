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
extern std::queue<CheckpointVector *> g_checkpointQueue;

class TandemIndex {
    public:
        TandemIndex();
        ~TandemIndex();

        bool insert(Key_t key, Val_t value);
        //void remove(int key);
        void update(Key_t key, Val_t value);
        void scan(Key_t key, size_t range, std::priority_queue<Key_t, std::vector<Key_t>, std::greater<Key_t>> &result);
        //void print();
        Val_t lookup(Key_t key);
        Val_t lookupInVnodeChain(Vnode *startVnde, Key_t key);
        void recover(Key_t key);

        bool handleExistingInodeInsert(Inode *inode, Key_t key, Val_t value, 
                                          int idx, bool &needToRebalance, Vnode* &targetVnode);
        bool updateInodeAfterSplit(Inode *inode, Vnode *targetVnode, bool &needToRebalance);
        bool handleNewInodeInsert(Key_t key, Val_t value);

        bool findAndUpdateInVnodeChain(Key_t key, Val_t value, Vnode *startVnode, Vnode *&targetVnode);
        bool performUpdateInVnode(Vnode *vnode, Key_t key, Val_t value, int pos, BloomFilter *bloom, Vnode *&targetVnode);
        bool performSimpleUpdate(Vnode *vnode, Key_t key, Val_t value, int pos, BloomFilter *bloom);
        bool relocateAndUpdateAfterSplit(Vnode *originalVnode, Vnode *newVnode, Key_t key, Val_t value, BloomFilter *originalBloom);
        bool updateAfterSplitInOriginal(Vnode *vnode, Key_t key, Val_t value, BloomFilter *bloom);
        bool updateAfterSplitInNew(Vnode *newVnode, Key_t key, Val_t value);
        void handleIndexUpdateAfterSplit(Inode *inode, Vnode *targetVnode, bool &needToRebalance);
        bool performUpdateWithSplit(Vnode *vnode, Key_t key, Val_t value, int pos, 
                                       BloomFilter *bloom, Vnode *&targetVnode);
        Key_t getMinKeyFromVnode(Vnode *vnode);
        void linkVnodeAndCreateLogEntry(Inode *inode, Vnode *targetVnode, int pos, Key_t targetKey);
        


        //std::thread *workerThread;kk
        std::thread *checkpointThread;
        std::thread *logMergeThread;

        //void createWorkerThread(); 
        void createCheckpointThread();
        void createLogMergeThread();
        void checkpointThreadExec(int id);
        void logMergeThreadExec(int id);
        //void workerThreadExec();

        DramSkiplist *mainIndex;
        DramInodePool *dramInodePool;
        PmemInodePool *pmemRecoveryArray;
        //PmemSkiplist *shadowIndex;
        ValueList *valueList;
        CkptLog *ckptLog;
        RecoveryManager *recoveryManager;
};