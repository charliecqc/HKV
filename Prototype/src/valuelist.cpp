#include "valuelist.h"
#include <cassert>

ValueList::ValueList() {
    pmemVnodePool = new PmemVnodePool(sizeof(Vnode), MAX_VALUE_NODES);
#if 0
    head = pmemVnodePool->getNextNode();
    head->hdr.next = std::numeric_limits<uint32_t>::max();
#endif
    if(pmemVnodePool->getCurrentIdx() != 0) {
        head = pmemVnodePool->at(0);
    }else {
        head = pmemVnodePool->getNextNode();
        head->hdr.next = std::numeric_limits<uint32_t>::max();
    }

}   

bool ValueList::append(Vnode *curNode, Vnode *nextNode)
{
    nextNode->hdr.next = curNode->hdr.next;
    curNode->hdr.next = nextNode->getId();
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(nextNode), sizeof(Vnode));
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(curNode), sizeof(Vnode));
    return true;
}

bool ValueList::split(Vnode *curNode, Vnode *nextNode)
{
    assert(nextNode->isEmpty());
    Key_t midKey = curNode->getMidKey(&bf[curNode->hdr.id]);
    Key_t maxKey = curNode->getMaxKey(&bf[curNode->hdr.id]);
    if(midKey != maxKey) {
        for(uint32_t i = 0; i < fanout; i++) {
            Key_t key = curNode->records[i].key;
            Val_t value = curNode->records[i].value;
            if(key > midKey) {
                nextNode->insert(key, value, &bf[nextNode->hdr.id]);
                curNode->hdr.unsetBit(i);
            }
        }
    }else {// all keys are the same
        for(uint32_t i = 0; i < fanout / 2; i++) {
            Key_t key = curNode->records[i].key;
            Val_t value = curNode->records[i].value;
            nextNode->insert(key, value, &bf[nextNode->hdr.id]);
            curNode->hdr.unsetBit(i);
        }
    }
    nextNode->hdr.next = curNode->hdr.next;
    curNode->hdr.next = nextNode->getId();
    assert(curNode->getMaxKey(&bf[curNode->hdr.id]) <= nextNode->getMinKey(&bf[nextNode->hdr.id]));
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
    while(nextNode != nullptr && nextNode->getMaxKey(&bf[nextNode->hdr.id]) <= key) {
        curNode = nextNode;
        nextNode = getNext(curNode);
    }
    bool ret = curNode->lookup(key, value, &bf[curNode->hdr.id]);
    return ret;
}

bool ValueList::recovery()
{
    return true;
}

Vnode *ValueList::getNext(Vnode *curNode)
{
    BloomFilter *bloom = &bf[curNode->hdr.id];
    shared_lock<std::shared_mutex> lock(bloom->mtx);
    return pmemVnodePool->at(curNode->hdr.next);
}

