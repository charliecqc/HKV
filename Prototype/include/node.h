#include <utility>
#include <iostream>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <cstring>

#pragma once
class Node {
public:
    int id;
    uint32_t next;
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
    int min_key;
    int max_key
    uint32_t down;
    Inode(int id, int min_key, int max_key, uint32_t next = 0, uint32_t down = 0) : Node(id, next) {
        this->min_key = min_key; 
        this->max_key = max_key;
        this->down = down;
    }
};

class Vnode : public Node
{
public:
    int value;
    Vnode(int id, int key, int value, uint32_t next = 0) : Node(id, key, next) {
        this->value = value;
    }

    int loookup(int key) {
        if(this->key == key) {
            return this->value;
        }
        return -1;
    }
    
    bool update(int key, int value) {
        if(this->key == key) {
            this->value = value;
            return true;
        }
        return false;
    }
    
    bool remove(int key) {
        if(this->key == key) {
            this->key = -1;
            this->value = -1;
            return true;
        }
        return false;
    }

    int lookup(int key) {
        if(this->key == key) {
            return value;
        }
        return -1;
    }

    int getKeyPos(int key) {
        if(this->key == key) {
            return this->id;
        }
        return -1;
    }
};
