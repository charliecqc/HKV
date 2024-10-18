#include <utility>
#include <iostream>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <cstring>
#include <atomic>
#include "common.h"

#pragma once

const int fanout = 32;

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

class header{
    public:
        int id; //4 bytes
        int32_t coveredNodes; // 4 bytes
        int32_t level; //4 bytes
        int next; //4 bytes 
        int16_t last_index; //2 bytes
    public:
        header() {
            id = 0;
            coveredNodes = 0;
            level = 0;
            next = 0;
        }
    friend class Inode;
};

class vnodeHeader{
    public:
        uint32_t id; //4 bytes
        uint32_t next; //4 bytes 
        // used to keep track of the keys are valid or not in the vnode
        uint32_t bitmap; // 4 bytes
        std::atomic<uint32_t> count; // 4 bytes

        vnodeHeader() {
            id = 0;
            next = 0;
            bitmap = 0;
            count = 0;
        }
    public:
        void setBit(int pos) {
            bitmap |= (1 << pos);
        }

        void unsetBit(int pos) {
            bitmap &= ~(1 << pos);
        }

        bool isBitSet(int pos) {
            return (bitmap & (1 << pos)) != 0;
        }
    friend class Vnode;
};

class entry
{
public:
    Key_t key; // 8bytes
    Val_t value;   // 8bytes
    entry() {
        key = std::numeric_limits<Key_t>::max();
        value = std::numeric_limits<Val_t>::max();
    }
    friend class Inode;
    friend class Vnode;
};

class Inode
{
public:
    header hdr;
    entry gps[fanout/2];
    entry sgps[fanout/2];

    Inode(uint32_t level)
    {
        hdr.level = level;
    }

    Inode(int id, uint32_t level, int next = 0)
    {
        hdr.id = id;
        hdr.next = next;
        hdr.level = level;
        hdr.coveredNodes = 0;
        for(int i = 0; i < fanout/2; i++) {
            gps[i].key = std::numeric_limits<Key_t>::max();
            gps[i].value = std::numeric_limits<Val_t>::max();
            sgps[i].key = std::numeric_limits<Key_t>::max();
            sgps[i].value = std::numeric_limits<Val_t>::max();
        }
    }

    int getId()
    {
        return this->hdr.id;
    }

    bool isHeader()
    {
        return (hdr.id >=0 && hdr.id <= MAX_LEVEL - 1)? true : false;   
    }

    bool isTail()
    {
        return (hdr.id >= MAX_LEVEL && hdr.id <= 2 * MAX_LEVEL - 1)? true : false;
    }

    bool activateGP()
    {
        int16_t cur_index = this->hdr.last_index;  
        cur_index = cur_index + 1;
        if(cur_index >= fanout/2) {
            return false;
        }else {
            this->hdr.last_index = cur_index;
            return true;
        }
    }

    bool checkForActivateGP()
    {
        if(this->hdr.coveredNodes > SEARCH_STABLITY_COEFFICIENT * (this->hdr.last_index + 1)) {
            return true;
        }
        return false;
    }
};

class Vnode
{
public:
    vnodeHeader hdr;
    entry records[fanout];
    Vnode(int id, int next = 0)
    {
        hdr.id = id;
        hdr.next = next;
        hdr.bitmap = 0;
        for(int i = 0; i < fanout; i++) {
            records[i].key = std::numeric_limits<Key_t>::max();
            records[i].value = std::numeric_limits<Val_t>::max();
        }
    }

    bool lookup(Key_t key, Val_t &value) {
        for(int i = fanout - 1 ; i >= 0; i--) {
            if(records[i].key == key && hdr.isBitSet(i)) {
                value = records[i].value;
                return true;
            }
        }
        return false;
    }

    Key_t getMaxKey() {
        //Todo:: use figer print to get the max key
        Key_t maxKey = std::numeric_limits<Key_t>::min();
        for(int i = fanout - 1; i >= 0; i--) {
            if(records[i].key == std::numeric_limits<Key_t>::max()) {
                continue;
            }
            if(records[i].key >= maxKey) {
                maxKey = records[i] .key;
            }
        }
        return maxKey;
    }

//Todo: Implement insert with finger print and bloom filter
//find the first empty slot and insert the key and value
    bool insert(Key_t key, Val_t value) {
        for(int i = fanout - 1; i >= 0; i--) {
            if(records[i].key == std::numeric_limits<Key_t>::max()) {
                records[i].key = key;
                records[i].value = value;
                hdr.setBit(i);
                return true;
            }
        }
        return false;
    }

   //Todo: Implement update and remove 
    bool update(Key_t key, Val_t value) {
        return false;
    }
    
    bool remove(Key_t key) {
        return false;
    }
    //Todo: Implement getKeyPos
    int getKeyPos(Key_t key) {
        return -1;
    }

    int getId()
    {
        return this->hdr.id;
    }
};
