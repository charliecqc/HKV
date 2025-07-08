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
#include <queue>
#include <mutex>
#pragma once


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
        void recover(Key_t key);
        bool moveToNextVnode(Vnode*& vnode, BloomFilter*& bloom, std::shared_lock<std::shared_mutex>& current_lock);
        bool insertWithNewInodes(Key_t key, Val_t value);

        bool insertWithoutIndex(Key_t key, Val_t value);
        bool insertWithNewInodes(Key_t key, Val_t value, Vnode* &vnode);
        bool insertInVnodeChain(Vnode *vnode, BloomFilter *bloom, std::unique_lock<std::shared_mutex> &vnode_lock, Key_t key, Val_t value, Inode *parent_inode, int idx);
        bool moveToNextVnodeForInsert(Vnode *&vnode, BloomFilter *&bloom, std::unique_lock<std::shared_mutex> &vnode_lock);
        bool handleNodeFullAndSplit(Vnode *vnode, BloomFilter *bloom, 
                                         std::unique_lock<std::shared_mutex> &vnode_lock, 
                                         Key_t key, Val_t value, Inode *target, Vnode* &newNode);
        bool updateParentInodeAfterSplit(Inode *parent_inode, Vnode *targetVnode, std::vector<Inode *> &updates);

        //std::thread *workerThread;kk
        std::thread *checkpointThread;
        std::thread *logMergeThread;
        std::thread *rebalanceThread[MAX_REBALANCE_THREADS];

        //void createWorkerThread();
        void createCheckpointThread();
        void createLogMergeThread();
        void createRebalanceThread();
        void rebalanceThreadExec(int id);
        void checkpointThreadExec(int id);
        void logMergeThreadExec(int id);
        //void workerThreadExec();
        
        // 重平衡队列相关方法
        void addToRebalanceQueue(Inode* &inode);
        bool getFromRebalanceQueue(Inode* &inode);
        void addToRebalanceMap(Inode *child, Inode *parent);
        bool getFromRebalanceMap(Inode *child, Inode *parent);

    private:
        DramSkiplist *mainIndex;
        DramInodePool *dramInodePool;
        PmemInodePool *pmemRecoveryArray;
        //PmemSkiplist *shadowIndex;
        ValueList *valueList;
        CkptLog *ckptLog;
        RecoveryManager *recoveryManager;
        bool needToRebalance;
        
        // 重平衡队列相关成员
        std::queue<Inode *> rebalanceQueue;
        std::mutex rebalanceQueueMutex;
};