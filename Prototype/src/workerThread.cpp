#include "workerThread.h"
#include "tandemIndex.h"


CheckpointThread::CheckpointThread(int tid, CheckpointQueue *cq, PmemInodePool *pmemInodePool) {
    this->id = tid;
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
}

void CheckpointThread::checkpointOperation() {
    ckp_entry *entry = cptq->pop();
    if(entry != nullptr) {
        Inode *inode = static_cast<Inode *>(entry->content);
        if(inode != nullptr) {
            std::shared_lock<std::shared_mutex> lock(inode->hdr.mtx);
            int id = inode->getId();
            Inode *pmemInode = pmemInodePool->at(id);
            PmemManager::memcpyToNVM(1, reinterpret_cast<char *>(pmemInode), reinterpret_cast<char *>(inode), sizeof(Inode));
            //PmemManager::memcpyToNVM(1,)
        }
    }
}