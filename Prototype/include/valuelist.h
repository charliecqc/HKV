#include <iostream>
#include "node.h"
#include "pmemVnodePool.h"

#pragma once
// Value list class on pmem
class ValueList {
public:
    PmemVnodePool *pmemVnodePool;
    Vnode *head;
    //sampling 
    size_t _total_request = 0;     // inc every write to value node
public:
    ValueList();
    bool insert(Key_t key, Val_t value);
    bool insert(Vnode* startNode, Vnode* vnode);
    bool update(Key_t key, Val_t value);
    bool remove(Key_t key);
    bool lookup(Key_t key, Val_t &value);
    bool recovery();
    Vnode *getNext(Vnode *curNode);
    Vnode* getHeader()
    {
        return head;
    }
    int getKeyPos(Key_t key);

    //sampling
    size_t getTotalRequests() { return _total_request; }
    void incTotalRequests() { _total_request++; }
    void resetTotalRequests() { _total_request = 0; }
};
