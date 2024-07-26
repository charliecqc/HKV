#include "skiplist.h"
#include "dramInodePool.h"
#include "common.h"
#pragma once// SkipList class
class DramSkiplist  {
private:
    Inode* header[MAX_LEVEL];
    Inode* tail[MAX_LEVEL];
    DramInodePool *dramInodePool;
    int level; //level is the current max level of the skiplist
public:
    DramSkiplist();
    ~DramSkiplist();
    bool insert(Key_t &key, Val_t &val);
    Inode *lookup(Key_t key);
    Inode *getPivotNode(Key_t key);
    bool getPivotNodesForInsert(Key_t key, Inode* updates[]);
    bool linkVnodeToInode(Vnode *vnode, Inode *inode);
    int generateRandomLevel();
};
