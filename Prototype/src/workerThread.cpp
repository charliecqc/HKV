#include "workerThread.h"
#include "tandemIndex.h"


LogFlushThread::LogFlushThread(int tid, CkptLog *cklog, PmemInodePool *pmemInodePool) {
    this->id = tid;
    this->ckptLog = cklog;
    this->pmemInodePool = pmemInodePool;
}

LogFlushThread::~LogFlushThread() {
    // 析构前确保把 [durable, produced) 全部刷盘
    ckptLog->forcePersist();
}

void LogFlushThread::LogFlushOperation() {
    try {
        ckptLog->tryFlushOnce();
    } catch (std::exception &e) {
        std::cout << "Exception in LogFlushOperation: " << e.what() << std::endl;
    }
}

LogMergeThread::LogMergeThread(int tid, CkptLog *cklog, PmemInodePool *pmemInodePool) {
    this->id = tid;
    this->ckptLog = cklog;
    this->pmemInodePool = pmemInodePool;
}

LogMergeThread::~LogMergeThread() {
    if(!ckptLog->isLogEmpty()) {
        ckptLog->forceReclaim(pmemInodePool);
    }
    assert(ckptLog->isLogEmpty());
    Inode *superNode = pmemInodePool->at(MAX_NODES);
    if(superNode != nullptr) {
        superNode->hdr.last_index = pmemInodePool->getCurrentIdx();  
        PmemManager::flushToNVM(1, reinterpret_cast<char *>(superNode), sizeof(Inode));
    }
    // 删除 ckptLog 的职责移到 TandemIndex 析构里统一处理
    // delete ckptLog;  // <-- 移除
}

void LogMergeThread::logMergeOperation() {
    try {
        ckptLog->reclaim(pmemInodePool);
    } catch (std::exception &e) {
        std::cout << "Exception in logMergeOperation: " << e.what() << std::endl;
    }
}