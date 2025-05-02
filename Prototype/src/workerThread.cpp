#include "workerThread.h"
#include "tandemIndex.h"


CheckpointThread::CheckpointThread(int tid, CheckpointQueue *cq, CkptLogNVM *cklog, PmemInodePool *pmemInodePool, DramSkiplist *index) {
    this->id = tid;
    this->index = index;
    this->ckptLog = cklog;
    this->cptq = cq;
    this->pmemInodePool = pmemInodePool;
}

bool CheckpointThread::isCheckpointQueueEmpty() {
    return cptq->isEmpty();
}

CheckpointThread::~CheckpointThread() {
    Inode *superNode = pmemInodePool->at(MAX_NODES);
    if(superNode != nullptr) {
        superNode->hdr.last_index = pmemInodePool->getCurrentIdx();  
        PmemManager::flushToNVM(1, reinterpret_cast<char *>(superNode), sizeof(Inode));
    }
    delete cptq;
    delete ckptLog;
}

void CheckpointThread::checkpointOperation() {
#if 0
    CheckpointVector *vec = cptq->pop();
    if(vec != nullptr) {
        ckptLog->enq(vec);
    }
#endif
}

LogMergeThread::LogMergeThread(int tid, CkptLog *cklog, PmemInodePool *pmemInodePool) {
    this->id = tid;
    this->ckptLog = cklog;
    this->pmemInodePool = pmemInodePool;
}

bool LogMergeThread::isCkptLogEmpty() {
    return ckptLog->isLogEmpty();
}

LogMergeThread::~LogMergeThread() {
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
    }catch(std::exception &e) {
        std::cout << "Exception in logMergeOperation: " << e.what() << std::endl;
    }
}