#include "dramInodePool.h"
#define DRAMINDEXPOOL 1
using namespace std;

bool DramInodePool::init() {
    size_t ip_size = NODE_POOL_SIZE;
    bool ret = DramManager::createPool(DRAMINDEXPOOL, ip_size);
    if (!ret) {
        std::cout << "Failed to create dram inode pool" << std::endl;
        return false;
    }

    // To allocate the vnode pool. 1. allocate memory. 2. cast into vodes 3. pot them into vector.
    void *indexPool = DramManager::getPoolStartAddress(DRAMINDEXPOOL);
    for(int i = 0; i < numNodes; i++) {
        Inode *inode = (Inode *) new (indexPool) Inode(i, 0, 0);
        dramInodePool.push_back(inode);
        indexPool += nodeSize;
    }
    return true;    
}

bool DramInodePool::extend(void *indexPool, size_t extendNumNodes) {
    if ((this->numNodes+ extendNumNodes) * nodeSize > NODE_POOL_SIZE) {
        std::cout << "Exceeding the maximum number of nodes" << std::endl;
        exit(-1);
    }
    void *currentPoolAddr = indexPool + this->numNodes * nodeSize;
    for (size_t i = this->numNodes; i < extendNumNodes; ++i) {
        Inode *inode = (Inode *) new (currentPoolAddr) Inode(i, 0, 0);
        dramInodePool.push_back(inode);
        currentPoolAddr += nodeSize;
    }
    return true;
}
