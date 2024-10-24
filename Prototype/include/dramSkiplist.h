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
    bool insert(Key_t &key, Val_t &val, Inode *inodes[], int newlevel);
    bool insertWhenRebalance(Key_t &key, Val_t &val, Inode* updates[], int count);
    // return the index in gps of the index node that poionts to the vnode
    Inode *lookup(Key_t key, int &idx);
    Inode *getHeader();
    void getPivotNodesForInsert(Key_t key, Inode* updates[]);
    bool linkVnodeToInode(Inode &inode, int idx, Vnode &vnode);
    bool increaseCoveredNodesAndVerifyRebalance(Inode &inode, bool &activeNewGP);
    bool checkForActivateGP(Inode &inode);
    bool checkForRebalance(Inode &inode, bool &activeNewGP);
    bool rebalanceInode(Inode *inode, bool lastLevel);
    bool rebalanceInode(Inode *inode, Key_t key, Val_t node_id, int count);
    int generateRandomLevel();
    void initInodes(Inode* inodes[], int newlevel, Key_t key);
    bool rebalanceInode(Inode &inode);
    bool rebalanceInode(Inode &inode, Vnode &vnode);
    bool activateGP(Inode &inode);
};
