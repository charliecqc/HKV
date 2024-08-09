#include <utility>
#include <iostream>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <cstring>
#include "common.h"

#pragma once
class Node {
public:
    int id;
    int next;
    Node() {
        this->id = -1;
        this->next = 0;
    }

    Node(int id, uint32_t next = 0) {
        this->id = id;
        this->next = next;
    }

    int getId()
    {
        return this->id;
    }
};

class Inode : public Node
{
public:
    Key_t min_key;
    Key_t max_key;
    int down;
    int coveredNodes;
    Inode(int id, Key_t min_key, Key_t max_key, int next = 0, int down = -1) : Node(id, next) {
        this->min_key = min_key; 
        this->max_key = max_key;
        this->down = down;
        this->coveredNodes = 0;
    }
};

class Vnode : public Node
{
public:
    Val_t value;
    Key_t key;
    Vnode(int id, Key_t key, Val_t value, int next = 0) : Node(id, next) {
        this->value = value;
        this->key = key;
    }

    int loookup(Key_t key) {
        if(this->key == key) {
            return this->value;
        }
        return -1;
    }
    
    bool update(Key_t key, Val_t value) {
        if(this->key == key) {
            this->value = value;
            return true;
        }
        return false;
    }
    
    bool remove(Key_t key) {
        if(this->key == key) {
            this->key = -1;
            this->value = -1;
            return true;
        }
        return false;
    }

    int lookup(Key_t key) {
        if(this->key == key) {
            return value;
        }
        return -1;
    }

    int getKeyPos(Key_t key) {
        if(this->key == key) {
            return this->id;
        }
        return -1;
    }
};
