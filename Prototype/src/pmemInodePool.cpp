#include "pmemInodePool.h"
#define INDEXPOOL 1
using namespace std;

bool PmemInodePool::init(root_obj *root) {
    size_t vp_size = 10UL * 1024UL * 1024UL * 1024UL; 
    bool isCreate;
    bool ret = PmemManager::createOrOpenPool(INDEXPOOL, fileName.c_str(), vp_size, (void **)&root, isCreate);
    if (!ret) {
        std::cout << "Failed to create or open pool: " << fileName << std::endl;
        return false;
    }

    // To allocate the vnode pool. 1. allocate memory. 2. cast into vodes 3. pot them into vector.
    PMEMobjpool *pop = (PMEMobjpool *)PmemManager::getPoolStartAddress(INDEXPOOL);
    nodeSize = sizeof(Inode);
    if(isCreate) {
        int ret_val = pmemobj_alloc(pop, &root->ptr[0], nodeSize * MAX_NODES, 0, NULL, NULL);
        if (ret_val) {
            std::cout << "Failed to allocate memory for root->ptr[0]" << std::endl;
            return false;
        }
        void *inodePool = pmemobj_direct(root->ptr[0]);
        for(int i = 0; i < numNodes; i++) {
            Inode *inode = (Inode *) new (inodePool) Inode(i,0,0);
            pmemInodePool.push_back(inode);
            inodePool = static_cast<char *>(inodePool) + nodeSize;
        }
        PmemManager::flushToNVM(0, (char *)inodePool, nodeSize * numNodes);
    }else {
        void *inodePool = pmemobj_direct(root->ptr[0]);
        for(int i = 0; i < numNodes; i++) {
            Inode *inode = (Inode *)inodePool;
            pmemInodePool.push_back(inode);
            inodePool = static_cast<char *>(inodePool) + nodeSize;
        }
    }
    return true;    
}

bool PmemInodePool::extend(PMEMobjpool *pop, size_t extendNumNodes) {
    if (this->numNodes + extendNumNodes > MAX_NODES) {
        std::cout << "Exceeding the maximum number of nodes" << std::endl;
        exit(-1);
    }
    PMEMoid root = pmemobj_root(pop, sizeof(PMEMoid));
    root_obj *rootObj = (root_obj *)pmemobj_direct(root);
    void *inodePool = pmemobj_direct(rootObj->ptr[0]);
    void *currentPoolAddr = static_cast<char *>(inodePool) + this->numNodes * nodeSize;
    for (size_t i = this->numNodes; i < extendNumNodes; ++i) {
        Inode *inode = (Inode *) new (currentPoolAddr) Inode(i);
        pmemInodePool.push_back(inode);
        currentPoolAddr = static_cast<char *>(currentPoolAddr) + nodeSize;
    }
    return true;
}
