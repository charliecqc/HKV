#include "checkpoint.h"
#include "tandemIndex.h"

CheckpointQueue::CheckpointQueue() {
    this->checkpointQueue = &g_checkpointQueue;
    // Initialize the checkpoint's position
    queueLock = new std::mutex();
}

void CheckpointQueue::push(ckp_entry *entry) {
    queueLock->lock();
    checkpointQueue->push(entry);
    queueLock->unlock();
}

ckp_entry *CheckpointQueue::pop() {
    ckp_entry *entry;
    queueLock->lock();
   // if(checkpointQueue->pop(entry)) {
   if((entry = checkpointQueue->front())) {
        checkpointQueue->pop();
        queueLock->unlock();
        return entry;
    }else {
        queueLock->unlock();
        return nullptr;
    }
}
#if 0
void CheckpointQueue::push(CheckpointVector *vec) {
    checkpointQueue->push(vec);
}

CheckpointVector *CheckpointQueue::pop() {
    CheckpointVector *vec; 
    if(checkpointQueue->pop(vec)) {
        return vec;
    }else {
        return nullptr;
    }
}
#endif

bool CheckpointQueue::isEmpty() {
    queueLock->lock();
    bool ret = checkpointQueue->empty();
    queueLock->unlock();
    return ret;
}