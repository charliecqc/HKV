#include "workerThread.h"
#include "tandemIndex.h"


LogFlushThread::LogFlushThread(int tid, CkptLog *cklog, PmemInodePool *pmemInodePool) {
    this->id = tid;
    this->ckptLog = cklog;
    this->pmemInodePool = pmemInodePool;
}

LogFlushThread::~LogFlushThread() {
    if(!ckptLog->isLogEmpty()) {
        ckptLog->forcePersist();
    }
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
        ckptLog->forcePersist();
        ckptLog->forceReclaim(pmemInodePool);
    }
    assert(ckptLog->isLogEmpty());
    Inode *superNode = pmemInodePool->at(MAX_NODES);
    if(superNode != nullptr) {
        superNode->hdr.last_index = pmemInodePool->getCurrentIdx();  
        PmemManager::flushToNVM(1, reinterpret_cast<char *>(superNode), sizeof(Inode));
    }
    delete ckptLog;
}

void LogMergeThread::logMergeOperation() {
    try {
        ckptLog->reclaim(pmemInodePool);
    } catch (std::exception &e) {
        std::cout << "Exception in logMergeOperation: " << e.what() << std::endl;
    }
}