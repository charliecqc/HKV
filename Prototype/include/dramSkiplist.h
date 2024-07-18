#include "skiplist.h"
#pragma once// SkipList class
class DramSkiplist  {
private:
    Inode* header[MAX_LEVEL];
    Inode* tail[MAX_LEVEL];
    DramInodePool *dramInodePool;
    int level;
public:
    DramSkiplist();
    ~DramSkiplist();
    bool insert(Key_t &key, Val_t &val);
    std::pair<Vnode*, Vnode*> lookup(Key_t key);
};
