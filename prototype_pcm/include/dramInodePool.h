#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <libpmem.h>
#include <libpmemobj.h>
#include <vector>
#include <atomic>
#include "dramManager.h"
#include "common.h"
#include "node.h"


//dramInodePool size
#define NODE_POOL_SIZE ((60LL*1024*1024*1024))

class DramInodePool {
private:
    Inode* pool_base_{nullptr};   // contiguous block base address
    int nodeSize;
    int numNodes;
    std::atomic<int> currentIdx;
public:
    DramInodePool(size_t nodeSize, size_t numNodes) : nodeSize(nodeSize), numNodes(numNodes){
        init();
        currentIdx.store(0);
    }

    bool init();

    ~DramInodePool() {
        // Nodes are placement-new'd in DramManager pool;
        // call destructors but don't free (DramManager owns the memory)
        if (pool_base_) {
            for (int i = 0; i < numNodes; i++) {
                pool_base_[i].~Inode();
            }
        }
    }

    size_t getCurrentIdx() {
        return currentIdx.load();
    }

    void setCurrentIdx(size_t idx) {
        currentIdx.store(idx);
    }

    Inode* getCurrentNode() {
        return &pool_base_[currentIdx.load()];
    }

    Inode* getNextNode() {
        int idx = currentIdx.fetch_add(1);
        if (idx >= numNodes) {
            std::cout << "Exceeding the maximum number of nodes in dramInodePool, idx: " << idx << std::endl;
            return nullptr;
        }
        Inode *node = &pool_base_[idx];
        assert(node->getId() >= 0);
#ifdef DBG
        int id = node->getId();
        if (id == 35)
            cout << "this is node 35" << endl;
        cout << "allocate inode : " << id << endl;
#endif
        return node;
    }

    Inode* __attribute__((always_inline)) at(size_t index) {
        return &pool_base_[index];
    }

    // Debug mode: bounds-checked version, use during development
    Inode* at_checked(size_t index) {
        if (__builtin_expect(index >= static_cast<size_t>(numNodes), 0)) {
            std::cout << "invalid index " << index << " beyond dramInodePool capacity " << numNodes << std::endl;
            return nullptr;
        }
        return &pool_base_[index];
    }

    bool extend(void *indexPool, size_t extendNumNodes);

    int getPoolSize() {
        return numNodes;
    }
};