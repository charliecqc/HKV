#include "valuelist.h"

ValueList::ValueList() {
    pmemVnodePool = new PmemVnodePool(sizeof(Vnode), 10000000);
    head = pmemVnodePool->getNextNode();
    head->hdr.next = std::numeric_limits<uint32_t>::max();
}   

bool ValueList::insert(Key_t key, Val_t value)
{
#if 0
    Vnode *curNode = head;
    Vnode *newNode = pmemVnodePool->getNextNode();
    if(newNode == nullptr) {
        return false;
    }
    ret = newNode->insert(key, value);
    if(ret == false) {
        return false;
    }
    Vnode *nextNode = getNext(curNode);
    while(nextNode != nullptr && nextNode->key < key) {
        curNode = nextNode;
        nextNode = getNext(curNode);
    }
    newNode->next = curNode->next;
    curNode->next = newNode->getId();
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(newNode), sizeof(Vnode));
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(curNode), sizeof(Vnode));
    return true;
#endif
    return false;
}

bool ValueList::append(Vnode *curNode, Vnode *nextNode)
{
    std::unique_lock<std::shared_mutex> lock(curNode->hdr.mtx);
    nextNode->hdr.next = curNode->hdr.next;
    curNode->hdr.next = nextNode->getId();
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(nextNode), sizeof(Vnode));
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(curNode), sizeof(Vnode));
    return true;
}

bool ValueList::split(Vnode *curNode, Vnode *nextNode)
{
    Key_t midKey = curNode->getMidKey();
    Key_t maxKey = curNode->getMaxKey();
    if(midKey != maxKey) {
        for(uint32_t i = 0; i < fanout; i++) {
            Key_t key = curNode->records[i].key;
            Val_t value = curNode->records[i].value;
            if(key > midKey) {
                nextNode->insert(key, value);
                curNode->hdr.unsetBit(i);
            }
        }
    }else {
        for(uint32_t i = 0; i < fanout / 2; i++) {
            Key_t key = curNode->records[i].key;
            Val_t value = curNode->records[i].value;
            nextNode->insert(key, value);
            curNode->hdr.unsetBit(i);
        }
    }
    nextNode->hdr.next = curNode->hdr.next;
    curNode->hdr.next = nextNode->getId();
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(nextNode), sizeof(Vnode));
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(curNode), sizeof(Vnode));
    return true;
}

bool ValueList::update(Key_t key, Val_t value)
{
#if 0
    Vnode *curNode = head;
    while(true) {
        if(curNode->key < key) {
            curNode = getNext(curNode);
            continue;
        }
        break;
    }
    bool ret = curNode->update(key, value);
    return ret;
#endif
return true;
}

bool ValueList::remove(Key_t key)
{
#if 0
    Vnode *curNode = head;
    while(true) {
        if(curNode->key < key) {
            curNode = getNext(curNode);
            continue;
        }
        break;
    }
    bool ret = curNode->remove(key);
    return ret;
#endif
return true;
}   

bool ValueList::lookup(Key_t key, Val_t &value)
{
    Vnode *curNode = head;
    Vnode *nextNode = getNext(curNode);
    while(nextNode != nullptr && nextNode->getMaxKey() <= key) {
        curNode = nextNode;
        nextNode = getNext(curNode);
    }
    bool ret = curNode->lookup(key, value);
    return ret;
}

bool ValueList::recovery()
{
    return true;
}

Vnode *ValueList::getNext(Vnode *curNode)
{
    shared_lock<std::shared_mutex> lock(curNode->hdr.mtx);
    return pmemVnodePool->at(curNode->hdr.next);
}

int ValueList::getKeyPos(Key_t key)
{
    Vnode *curNode = head;
    while(true) {
        if(curNode->getMaxKey() < key) {
            curNode = getNext(curNode);
            continue;
        }
        break;
    }
    return curNode->getKeyPos(key);
}

