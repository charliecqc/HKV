#include <iostream>
#include "node.h"
#include "pmemVnodePool.h"

#pragma once
// Value list class on pmem
class ValueList {
private:
    PmemVnodePool *pmemVnodePool;
    Vnode *head;
public:
    ValueList();
    bool insert(int key, int value);
    bool update(int key, int value);
    bool remove(int key);
    int lookup(int key);
    bool recovery();
    Vnode *getNext(Vnode *curNode);
    int getKeyPos(int key);
};
