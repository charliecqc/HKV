#include <iostream>
#include "node.h"
#include "pmemVnodePool.h"

#pragma once
// Value list class on pmem
class ValueList {
private:
    PmemVnodePool *pmemVnodePool;
    Vnode *head;
public:
    ValueList();
    bool insert(int key, int value);
    bool update(int key, int value);
    bool remove(int key);
    int lookup(int key);
    bool recovery();
    Vnode *getNext(Vnode *curNode);
    int getKeyPos(int key);



    // Insert a new node at the beginning of the list
#if 0
    void insert(int key, int value) {
        TOID(Vnode) newNode = PmemVnodePool::getNextNode();
        if (TOID_IS_NULL(newNode)) {
            std::cerr << "Failed to pop nodes from the pool." << std::endl;
            return;
        }
        D_RW(newNode)->value = value;
        D_RW(newNode)->key = key;

        if (TOID_IS_NULL(head) || key < D_RO(head)->key) {
            D_RW(newNode)->next = D_RO(head)->next;
            head = newNode;
        } else {
            TOID(Vnode) currNode = D_RO(head);
        
            while (!TOID_IS_NULL(PmemVnodePool::at((currNode)->next)) && key > PmemVnodePool::at((D_RO(currNode)->next))->key) {
                currNode = PmemVnodePool::at(D_RO(currNode)->next);
            }
            D_RW(newNode)->next = D_RO(currNode)->next;
            D_RW(currNode)->next = newNode->id;
        }

        TX_ADD(newNode);
        TX_COMMIT();
    }

    // Delete a node with the given value from the list
    void remove(int key) {
        TOID(Vnode) currNode = D_RO(head)
        TOID (Vnode) prevNode = nullptr;    
        while(!TOID_IS_NULL(currNode) && D_RO(currNode)->key != key) {
            prevNode = currNode;
            currNode = PmemVnodePool::at((currNode)->next);
        }   
        if (TOID_IS_NULL(currNode)) {
            std::cout << "Node not found in the list." << std::endl;
            return;
        }
        if(TOID_IS_NULL(prevNode)) {
            head = PmemVnodePool::at((currNode)->next);
        } else {
            D_RW(prevNode)->next = D_RO(currNode)->next;
        }
        delete currNode;
    }

    // Lookup a node with the given value in the list
    Vnode* lookup(int key) {
        TOID(Vnode) currNode = D_RO(head);
        while(!TOID_IS_NULL(currNode) && D_RO(currNode)->key != key) {
            currNode = PmemVnodePool::at((currNode)->next);
        }
        if(TOID_IS_NULL(currNode)) {
            std::cout << "Node not found in the list." << std::endl;
            return nullptr;
        }else {
            return currNode;
        }
    }
#endif
};
