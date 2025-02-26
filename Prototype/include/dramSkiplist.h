#include "skiplist.h"
#include "checkpoint.h"
#include "dramInodePool.h"
#include "pmemVnodePool.h"
#include "common.h"
#pragma once// SkipList class
class DramSkiplist  {
private:
    Inode* header[MAX_LEVEL];
    Inode* tail[MAX_LEVEL];
    DramInodePool *dramInodePool;
    CheckpointQueue *ckpq;
    int level; //level is the current max level of the skiplist
    std::shared_mutex level_lock;
public:
    std::shared_mutex inode_locks[MAX_NODES];
    std::shared_mutex rebalance_lock;
    DramSkiplist(CheckpointQueue *q, DramInodePool *dramInodePool);
    ~DramSkiplist();
    bool insert(Key_t &key, Val_t &val);
    bool insert(Key_t &key, Val_t &val, Inode *inodes[], int newlevel);
    bool update(Key_t &oldKey, Key_t &newKey, Val_t &val);
    // return the index in gps of the index node that poionts to the vnode
    Inode *lookup(Key_t key, int &idx);
    Inode *getHeader();
    void getPivotNodesForInsert(Key_t key, Inode* updates[]);
    bool linkVnodeToInode(Inode &inode, int idx, Vnode &vnode);
    bool checkForActivateGP(Inode &inode);
    bool checkForRebalance(Inode &inode, bool &activeNewGP);
    bool rebalanceInode(Inode *inode, bool lastLevel);
    void rebalanceInodeImp(Inode *target, Inode *&prev_target, int &prev_pos, Key_t targetKey, bool is_current_top);    
    int generateRandomLevel();
    void initInodes(Inode* inodes[], int newlevel, Key_t key);
    bool rebalanceInode(Inode &inode);
    bool rebalanceInode(Inode &inode, Vnode &vnode);
    bool activateGP(Inode &inode);
    void setLevel(int level);
    int getLevel();
};
