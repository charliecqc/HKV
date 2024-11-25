#include "dramSkiplist.h"
#include "pmemSkiplist.h"
#include "valuelist.h"
#pragma once
class TandemIndex {
    public:
        TandemIndex() {
            //head is the top layer first node
            mainIndex = new DramSkiplist();
            valueList = new ValueList();
            Inode *index_header = mainIndex->getHeader();
            Vnode *value_header = valueList->getHeader();
            index_header->gps[0].value = value_header->getId();
            //shadowIndex = new PmemSkiplist();
        }

        ~TandemIndex() {
        }
        bool insert(Key_t key, Val_t value);
        //void remove(int key);
        //void update(int key, int value);
        //void print();
        Val_t lookup(Key_t key);
        

        DramSkiplist *mainIndex;
        ValueList *valueList;
        //PmemSkiplist *shadowIndex;
};