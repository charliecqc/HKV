#include "skiplist.h"
#include "dramInodePool.h"
#include "pmemVnodePool.h"
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
    bool insertWhenRebalance(Key_t &key, Val_t &val, Inode* updates[], int count);
    Inode *lookup(Key_t key);
    Inode *getPivotNode(Key_t key);
    Inode *getHeader();
    bool getPivotNodesForInsert(Key_t key, Inode* updates[]);
    bool linkVnodeToInode(Inode *inode, Vnode *vnode);
    bool increaseCoveredNodesAndVerifyRebalance(Inode* inode);
    bool rebalanceInode(Inode *inode, bool lastLevel);
    bool rebalanceInode(Inode *inode, Key_t key, Val_t node_id, int count);
    int generateRandomLevel();
};
