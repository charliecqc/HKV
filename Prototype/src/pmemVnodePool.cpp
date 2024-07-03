#include "pmemVnodePool.h"
#define VALUEPOOL 0
using namespace std;

bool PmemVnodePool::init(root_obj *root) {
    size_t vp_size = 10UL * 1024UL * 1024UL * 1024UL; 
    bool ret = PmemManager::createPool(VALUEPOOL, fileName.c_str(), vp_size, (void **)&root);
    if (!ret) {
        return false;
    }

    // To allocate the vnode pool. 1. allocate memory. 2. cast into vodes 3. pot them into vector.
    PMEMobjpool *pop = (PMEMobjpool *)PmemManager::getPoolStartAddress(VALUEPOOL);
    int ret_val = pmemobj_alloc(pop, &root->ptr[0], nodeSize * MAX_NODES, 0, NULL, NULL);
    if (ret_val) {
        std::cout << "Failed to allocate memory for root->ptr[0]" << std::endl;
        return false;
    }
    void *vnodePool = pmemobj_direct(root->ptr[0]);
    for(int i = 0; i < numNodes; i++) {
        Vnode *vnode = (Vnode *) new (vnodePool) Vnode(i, 0, 0);
        pmemVnodePool.push_back(vnode);
        vnodePool += nodeSize;
    }
    PmemManager::flushToNVM(0, (char *)vnodePool, nodeSize * numNodes);
    return true;    
}

bool PmemVnodePool::extend(PMEMobjpool *pop, size_t extendNumNodes) {
    if (this->numNodes + extendNumNodes > MAX_NODES) {
        std::cout << "Exceeding the maximum number of nodes" << std::endl;
        exit(-1);
    }
    PMEMoid root = pmemobj_root(pop, sizeof(PMEMoid));
    root_obj *rootObj = (root_obj *)pmemobj_direct(root);
    void *vnodePool = pmemobj_direct(rootObj->ptr[0]);
    void *currentPoolAddr = vnodePool + this->numNodes * nodeSize;
    for (size_t i = this->numNodes; i < extendNumNodes; ++i) {
        Vnode *vnode = (Vnode *) new (currentPoolAddr) Vnode(i, 0, 0);
        pmemVnodePool.push_back(vnode);
        currentPoolAddr += nodeSize;
    }
    return true;
}
