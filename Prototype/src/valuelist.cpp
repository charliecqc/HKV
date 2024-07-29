#include "valuelist.h"

ValueList::ValueList() {
    pmemVnodePool = new PmemVnodePool(sizeof(Vnode), 1000);
    head = pmemVnodePool->getNextNode();
    head->key = std::numeric_limits<Key_t>::min();
    head->value = std::numeric_limits<Key_t>::min();
    head->next = std::numeric_limits<uint32_t>::max();
}   

bool ValueList::insert(Key_t key, Val_t value)
{
    Vnode *curNode = head;
    Vnode *newNode = pmemVnodePool->getNextNode();
    if(newNode == nullptr) {
        return false;
    }
    newNode->key = key;
    newNode->value = value;
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
}

bool ValueList::insert(Vnode *startNode, Vnode *newNode)
{
    Vnode *curNode = startNode;
    Vnode *nextNode = getNext(curNode);
    while(nextNode != nullptr && nextNode->key < newNode->key) {
        curNode = nextNode;
        nextNode = getNext(curNode);
    }
    newNode->next = curNode->next;
    curNode->next = newNode->getId();
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(newNode), sizeof(Vnode));
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(curNode), sizeof(Vnode));
    return true;
}

bool ValueList::update(Key_t key, Val_t value)
{
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
}

bool ValueList::remove(Key_t key)
{
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
}   

int ValueList::lookup(Key_t key)
{
    Vnode *curNode = head;
    Vnode *nextNode = getNext(curNode);
    while(nextNode != nullptr && nextNode->key <= key) {
        curNode = nextNode;
        nextNode = getNext(curNode);
    }
    int value = curNode->lookup(key);
    return value;
}

bool ValueList::recovery()
{
    return true;
}

Vnode *ValueList::getNext(Vnode *curNode)
{
    return pmemVnodePool->at(curNode->next);
}

int ValueList::getKeyPos(Key_t key)
{
    Vnode *curNode = head;
    while(true) {
        if(curNode->key < key) {
            curNode = getNext(curNode);
            continue;
        }
        break;
    }
    return curNode->getKeyPos(key);
}

