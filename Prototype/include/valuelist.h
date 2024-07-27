#include <iostream>
#include "node.h"
#include "pmemVnodePool.h"

#pragma once
// Value list class on pmem
class ValueList {
public:
    PmemVnodePool *pmemVnodePool;
    Vnode *head;
public:
    ValueList();
    bool insert(Key_t key, Val_t value);
    bool insert(Vnode* startNode, Vnode* vnode);
    bool update(Key_t key, Val_t value);
    bool remove(Key_t key);
    int lookup(Key_t key);
    bool recovery();
    Vnode *getNext(Vnode *curNode);
    Vnode* getHeader()
    {
        return head;
    }
    int getKeyPos(Key_t key);
};
