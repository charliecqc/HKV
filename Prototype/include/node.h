#include <utility>
#include <iostream>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <cstring>
#include <vector>
#include <thread>
#include <queue>
#include <atomic>
#include <shared_mutex>
#include <mutex>
#include <unordered_set>
#include "common.h"

#pragma once

const int32_t fanout = 28;

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
        int16_t id; //2 bytes
        int8_t coveredNodes; // 1 byte
        int8_t level; //1 byte
        int16_t next; //2 bytes 
        int16_t last_index; //2 bytes
        //std::shared_mutex mtx; //8 bytes
    public:
        header() {
            id = 0;
            coveredNodes = 0;
            level = 0;
            next = 0;
            last_index = -1;
        }
    friend class Inode;
};

class vnodeHeader{
    public:
        uint32_t id; //4 bytes
        int next; //4 bytes 
        // used to keep track of the keys are valid or not in the vnode
        uint32_t bitmap; // 4 bytes
        std::shared_mutex mtx;
        vnodeHeader() {
            id = 0;
            next = 0;
            bitmap = 0;
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
        for(int32_t i = 0; i < fanout/2; i++) {
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

    bool isFull()
    {
        return hdr.last_index == fanout/2 - 1;
    }

    bool activateGP(Key_t targetKey, int &pos)
    {
        //check if there is enough space to insert the new GP
        int16_t cur_index = this->hdr.last_index;  
        if(static_cast<int32_t>(cur_index + 1)>= fanout/2) {
            return false;
        }else {
            pos = this->findInsertKeyPos(targetKey);
            if(pos <= hdr.last_index) 
                this->shift(pos); // shift the contents
            this->hdr.last_index = cur_index + 1;
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

    int findInsertKeyPos(Key_t key)
    {
        int idx = 0;
        if(key < this->getMinKey())
            return idx;
        for(int i = 0; i <= this->hdr.last_index; i++) {
            if(key >= this->gps[i].key) {
                if(i + 1 <= this->hdr.last_index) {
                    if(key < this->gps[i+1].key) {
                        idx = i + 1;
                        break;
                    }
                } else {
                    idx = i+1;
                    break;
                }
            }
        }
        return idx;
    }

    int findKeyPos(Key_t key)
    {
        int idx = 0;
        for(int i = 0; i <= this->hdr.last_index; i++) {
            if(key >= this->gps[i].key) {
                if(i + 1 <= this->hdr.last_index) {
                    if(key < this->gps[i+1].key) {
                        idx = i;
                        break;
                    }
                } else {
                    idx = i;
                    break;
                }
            }
        }
        return idx;
    }

    bool shift(int oldIdx) { // shift data from oldIdx to newIdx
        memmove(&gps[oldIdx+1], &gps[oldIdx], sizeof(entry) * (hdr.last_index - oldIdx + 1));
        return true;
    }

    Key_t getMaxKey() {
        return gps[hdr.last_index].key;
    }

    Key_t getMinKey() {
        return gps[0].key;
    }

    Key_t getMidKey() {
        return gps[hdr.last_index / 2].key;
    }

    bool split(Inode *targetInode) {
        memmove(targetInode->gps, &gps[hdr.last_index / 2], sizeof(entry) * (hdr.last_index / 2 + 1));
        int temp_index = hdr.last_index;
        hdr.last_index = hdr.last_index / 2 - 1;
        targetInode->hdr.last_index = temp_index / 2;
        hdr.coveredNodes = hdr.last_index + 1;
        targetInode->hdr.coveredNodes = targetInode->hdr.last_index + 1;
        //int next = hdr.next;
        //targetInode->hdr.next = next;
        //hdr.next = targetInode->getId();
        return true;
    }

    bool insertAtPos(Key_t key, Val_t value, int pos) {
        shift(pos);
        hdr.last_index++;
        gps[pos].key = key;
        gps[pos].value = value;
        hdr.coveredNodes++;
        return true;
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
        for(int32_t i = 0; i < fanout; i++) {
            records[i].key = std::numeric_limits<Key_t>::max();
            records[i].value = std::numeric_limits<Val_t>::max();
        }
    }

    bool lookup(Key_t key, Val_t &value) {
        for(int32_t i = fanout - 1 ; i >= 0; i--) {
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
            if(hdr.isBitSet(i) == false) {
                continue;
            }
            if(records[i].key >= maxKey) {
                maxKey = records[i] .key;
            }
        }
        return maxKey;
    }

    Key_t getMinKey() {
        Key_t minKey = std::numeric_limits<Key_t>::max();
        for(int i = fanout - 1; i >= 0; i--) {
            if(hdr.isBitSet(i) == false) {
                continue;
            }
            if(records[i].key <= minKey) {
                minKey = records[i].key;
            }
        }
        return minKey;
    }

    Key_t getMidKey() {
        std::priority_queue<Key_t, std::vector<Key_t>, std::greater<Key_t>> pq;
        std::unordered_set<Key_t> keySet;
        {
            for(int i = fanout - 1; i >= 0; i--) {
                if(hdr.isBitSet(i) == false) {
                    continue;
                }
                if(records[i].key == std::numeric_limits<Key_t>::max()) {
                    continue;
                }
                if (keySet.find(records[i].key) != keySet.end()) {
                    continue;
                }
                keySet.insert(records[i].key);
            }
        }
        unsigned long size = keySet.size();
        for (const Key_t& key : keySet) {
            pq.push(key);
            if(pq.size() > size / 2 + 1) {
                pq.pop();
            }
        }
        return pq.top();
    }

    bool split(Vnode *targetVnode) {
        std::unique_lock<std::shared_mutex> lock(hdr.mtx);
        Key_t midKey = getMidKey();
        Key_t key = std::numeric_limits<Key_t>::max();
        Val_t value = std::numeric_limits<Val_t>::max();
        for(int32_t i = 0; i < fanout; i++) {
            {
                key = records[i].key;
                value = records[i].value;
            }
            if(key > midKey) {
                targetVnode->insert(key, value);
            }
            hdr.unsetBit(i);
        }
        targetVnode->hdr.next = hdr.next;
        hdr.next = targetVnode->getId();
#ifdef DBG
        std::cout << "split done this: " << this->getId() << " this->max: " <<getMaxKey() << " new: " << targetVnode->getId() << " max: " << targetVnode->getMaxKey()<< std::endl;
#endif
        return true;
    }

//Todo: Implement insert with finger print and bloom filter
//find the first empty slot and insert the key and value
    bool insert(Key_t key, Val_t value) {
        {
            int32_t pos = __builtin_ffs(~hdr.bitmap) - 1;
            if (pos >= 0 && pos < fanout) {
                records[pos].key = key;
                records[pos].value = value;
                hdr.setBit(pos);
#ifdef DBG
                std::cout << "vnode id: " << hdr.id << " insert key: " << key << " value: " << value << " at pos: " << pos << std::endl;
#endif
                return true;
            }
            return false;
        }
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
    
    bool isFull()
    {
        return hdr.bitmap == static_cast<uint32_t>((1 << fanout) - 1);
    }

    bool isEmpty()
    {
        return hdr.bitmap == 0;
    }

    void dump()
    {
        std::cout << "Vnode id: " << hdr.id << " next: " << hdr.next << " bitmap (binary): ";
        for (int i = fanout - 1; i >= 0; i--) {
            std::cout << ((hdr.bitmap >> i) & 1);
        }
        std::cout << std::endl;
        for(int32_t i = 0; i < fanout; i++) {
#if 0
            if(hdr.isBitSet(i)) {
                std::cout << "Key: " << records[i].key << " Value: " << records[i].value << std::endl;
            }
#endif
        }
        std::cout << " min: " << getMinKey() << " max: " << getMaxKey() << std::endl;
    }
};
