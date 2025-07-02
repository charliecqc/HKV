#include "skiplist.h"
#include "checkpoint.h"
#include "dramInodePool.h"
#include "pmemVnodePool.h"
#include "ckpt_log.h"
#include "common.h"
#include "valuelist.h"
#pragma once// SkipList class
class DramSkiplist  {
private:
    Inode* header[MAX_LEVEL];
    Inode* tail[MAX_LEVEL];
    DramInodePool *dramInodePool;
    CkptLog *ckpt_log;
    ValueList *valueList;
    int level; //level is the current max level of the skiplist
    std::shared_mutex level_lock;
public:
    std::shared_mutex inode_locks[MAX_NODES];
    std::shared_mutex rebalance_lock;
    DramSkiplist(CkptLog *ckp_log, DramInodePool *dramInodePool, ValueList *valuelist);
    ~DramSkiplist();
    bool insert(Key_t &key, Val_t &val);
    bool insert(Key_t &key, Val_t &val, Inode *inodes[], int newlevel);
    bool insert(Vnode *targetVnode);
    bool update(Key_t &oldKey, Key_t &newKey, Val_t &val);
    bool add(Vnode *targetVnode);
    // return the index in gps of the index node that poionts to the vnode
    Inode *lookup(Key_t key, int &idx);
    Inode *lookup(Key_t key, Inode *current, int currentHighestLevelIndex, std::shared_lock<std::shared_mutex> &current_lock, int &idx);
    Inode *lookupForInsert(Key_t key, Inode *current, int currentHighestLevelIndex, std::shared_lock<std::shared_mutex> &current_lock, int &idx, std::vector<Inode *> &updates);
    Inode *getHeader();
    Inode *getHeader(int level);
    void getPivotNodesForInsert(Key_t key, Inode* updates[]);
    bool linkVnodeToInode(Inode &inode, int idx, Vnode &vnode);
    bool checkForActivateGP(Inode &inode);
    bool checkForRebalance(Inode &inode, bool &activeNewGP);
    bool rebalanceInode(Inode *inode, bool lastLevel);
    int generateRandomLevel();
    bool rebalanceInode(Inode &inode);
    bool rebalanceInode(Inode &inode, Vnode &vnode);
    bool rebalanceIndex(Vnode &targetVnode);
    int rebalanceIdx(Vnode &targetVnode, Key_t targetKey);
    bool activateGP(Inode &inode);
    void setLevel(int level);
    int getLevel();
};
