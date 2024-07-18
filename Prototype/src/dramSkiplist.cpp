#include "dramSkiplist.h"
#define numNodesInPool 10000

DramSkiplist::DramSkiplist()
{
    dramInodePool = new DramInodePool(sizeof(Inode), numNodesInPool);
    poolSize = dramInodePool->getPoolSize();
    header[0] = pmemVnodePool->getNextNode();
    for(int i = 1; i < MAX_LEVEL; i++) {
        header[i] = pmemVnodePool->getNextNode();
        header[i-1]->down = header[i]->getId();
        header[i]->min_key = std::numeric_limits<int>::min();
        header[i]->max_key = std::numeric_limits<int>::min();
        header[i]->next = std::numeric_limits<uint32_t>::max();
    }

    tail[0] = pmemVnodePool->getNextNode();
    for(int i = 1; i < MAX_LEVEL; i++) {
        tail[i] = pmemVnodePool->getNextNode();
        tail[i-1]->down = tail[i]->getId();
        tail[i]->min_key = std::numeric_limits<int>::max();
        tail[i]->max_key = std::numeric_limits<int>::max();
        tail[i]->next = std::numeric_limits<uint32_t>::max();
    }

    for(int i = 0; i < MAX_LEVEL; i++) {
        header[i]->next = tail[i]->getId();
    }
}

bool DramSkiplist::insert(Key_t &key, Key_t &val)
{
    Inode *destNode;
    Inode* update[MAX_LEVEL];
    Inode* current = header[0];
    for(int i = MAX_LEVEL-1; i >= 0; i--) {
        while(current->next != tail[i]->getId() && current->next->min_key <= key) {
            current = current->next;
        }
        update[i] = current;
    }
    current = current->next;
    if(current->min_key == key) {
        current->value = value;
        return true;
    }
    int newLevel = 1;
    while(newLevel < MAX_LEVEL && rand() % 2 == 0) {
        newLevel++;
    }
    Inode* newNode = pmemVnodePool->getNextNode();
    newNode->min_key = key;
    newNode->max_key = key;
    newNode->value = value;
    for(int i = 0; i < newLevel; i++) {
        newNode->next = update[i]->next;
        update[i]->next = newNode;
        newNode->down = update[i]->getId();
        update[i] = newNode;
        newNode = pmemVnodePool->getNextNode();
        newNode->min_key = key;
        newNode->max_key = key;
        newNode->value = value;
    }
    return true;
}

std::pair<Vnode*, Vnode*> DramSkiplist::lookup(int key)
{
    Inode* current = header[0];
    for(int i = MAX_LEVEL-1; i >= 0; i--) {
        while(current->next != tail[i]->getId() && current->next->min_key <= key) {
            current = current->next;
        }
    }
    current = current->next;
    if(current->min_key == key) {
        return std::make_pair(current, current);
    }
    return std::make_pair(nullptr, nullptr);
}