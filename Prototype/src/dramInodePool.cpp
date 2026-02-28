#include "dramInodePool.h"
#define DRAMINDEXPOOL 1

bool DramInodePool::init() {
    size_t ip_size = NODE_POOL_SIZE;
    bool ret = DramManager::createPool(DRAMINDEXPOOL, ip_size);
    if (!ret) {
        std::cout << "Failed to create dram inode pool" << std::endl;
        return false;
    }

    // Contiguous allocation: place Inodes directly in the pool
    void *indexPool = DramManager::getPoolStartAddress(DRAMINDEXPOOL);
    pool_base_ = static_cast<Inode*>(indexPool);
    for(int i = 0; i < numNodes; i++) {
        new (&pool_base_[i]) Inode(i, 0, 0);
    }
    return true;    
}

bool DramInodePool::extend(void *indexPool, size_t extendNumNodes) {
    if ((this->numNodes+ extendNumNodes) * nodeSize > NODE_POOL_SIZE) {
        std::cout << "Exceeding the maximum number of nodes" << std::endl;
        exit(-1);
    }
    // pool_base_ already covers the contiguous region;
    // just construct new nodes at the tail
    for (size_t i = this->numNodes; i < this->numNodes + extendNumNodes; ++i) {
        new (&pool_base_[i]) Inode(i, 0, 0);
    }
    this->numNodes += extendNumNodes;
    return true;
}
