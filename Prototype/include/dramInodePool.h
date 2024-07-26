#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <libpmem.h>
#include <libpmemobj.h>
#include <vector>
#include "dramManager.h"
#include "node.h"
#pragma once

#define LAYOUT_NAME "value_pool"
#define NODE_POOL_SIZE ((30LL*1024*1024*1024))

using namespace std;

class DramInodePool {
private:
    std::vector<Inode*> dramInodePool;
    int nodeSize;
    int numNodes;
    int currentIdx;
public:
    DramInodePool(size_t nodeSize, size_t numNodes) : nodeSize(nodeSize), numNodes(numNodes){
        init();
        currentIdx = 0;
    }

    bool init();

    ~DramInodePool() {
        // Deallocate memory blocks
        for (Inode* node : dramInodePool) {
            delete[] node;
        }
    }

    size_t getCurrentIdx() {
        return currentIdx;
    }

    Inode* getCurrentNode() {
        return dramInodePool[currentIdx];
    }

    Inode* getNextNode() {
        if (currentIdx >= numNodes) {
            return nullptr;
        }
        return dramInodePool[currentIdx++];
    }

    Inode* popNode() {
        if (dramInodePool.empty()) {
            return nullptr;
        }

        Inode* inode = dramInodePool.back();
        dramInodePool.pop_back();
        return inode;
    }

    void push(Inode *inode) {
        dramInodePool.push_back(inode);
    }

    Inode * at(size_t index) {
        if (index >= dramInodePool.size()) {
            return nullptr;
        }
        return dramInodePool[index];
    }

    bool extend(void *indexPool, size_t extendNumNodes);

    int getPoolSize() {
        return dramInodePool.size();
    }
};