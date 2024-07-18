#include "dramSkiplist.h"
#include "pmemSkiplist.h"
#pragma once
class TandemIndex {
    public:
        TandemIndex() {
            //head is the top layer first node
            mainIndex = new DramSkiplist();
            valueList = new ValueList();
            //shadowIndex = new PmemSkiplist();
        }

        ~TandemIndex() {
        }
        bool insert(int key, int value);
        //void remove(int key);
        //void update(int key, int value);
        //void print();
        //int lookup(int key);

        DramSkiplist *mainIndex;
        ValueList *valueList;
        //PmemSkiplist *shadowIndex;
};