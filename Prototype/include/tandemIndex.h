#include "dramSkiplist.h"
#include "pmemSkiplist.h"
#pragma once
#if 0
class TandemIndex {
    public:
        TandemIndex() {
            //head is the top layer first node
            mainIndex = new DramSkiplist();
            shadowIndex = new PmemSkiplist();
        }

        ~TandemIndex() {
        }
        void insert(int key, int value);
        void remove(int key);
        void update(int key, int value);
        void print();
        int lookup(int key);

        DramSkiplist *mainIndex;
        PmemSkiplist *shadowIndex;
};
#endif