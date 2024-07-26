#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <libpmem.h>
#include <libpmemobj.h>
#include <vector>
#include "pmemManager.h"
#include "node.h"
#pragma once

#define LAYOUT_NAME "value_pool"
#define MAX_NODES 5000
#define NODE_POOL_SIZE ((30LL*1024*1024*1024))

using namespace std;

class PmemVnodePool {
private:
    string fileName = "/mnt/pmem0/pmemVnodePool";
    std::vector<Vnode*> pmemVnodePool;
    int nodeSize;
    int numNodes;
    int currentIdx;
public:
    PmemVnodePool(size_t nodeSize, size_t numNodes) : nodeSize(nodeSize), numNodes(numNodes){
        root_obj *root = nullptr;
        init(root);
        currentIdx = 0;
    }

    bool init(root_obj *root);

    ~PmemVnodePool() {
        // Deallocate memory blocks
        for (Vnode* node : pmemVnodePool) {
            delete[] node;
        }
    }

    size_t getCurrentIdx() {
        return currentIdx;
    }

    bool resetCurrentIdx(int newIdx) {
        currentIdx = newIdx;
    }

    Vnode* getCurrentNode() {
        return pmemVnodePool[currentIdx];
    }

    Vnode *getNextNode() {
        if (currentIdx >= numNodes) {
            return nullptr;
        }
        return pmemVnodePool[currentIdx++];
    }

    Vnode * popNode() {
        if (pmemVnodePool.empty()) {
            return nullptr;
        }

        Vnode* vnode = pmemVnodePool.back();
        pmemVnodePool.pop_back();
        return vnode;
    }

    void push(Vnode *vnode) {
        pmemVnodePool.push_back(vnode);
    }

    Vnode * at(size_t index) {
        if (index >= pmemVnodePool.size()) {
            return nullptr;
        }
        return pmemVnodePool[index];
    }

    bool extend(PMEMobjpool *pop, size_t extendNumNodes);
};