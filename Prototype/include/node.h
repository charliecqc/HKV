#pragma once
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
#include <algorithm>
#include "common.h"
#ifdef __AVX2__
#include <immintrin.h>
#endif
const int32_t fanout = 28;

class BloomFilter {
public:
    static const size_t FILTER_SIZE = 256;  // 过滤器大小
    static const size_t HASH_FUNCTIONS = 4;  // 哈希函数数量
    alignas(64) uint8_t fingerprints[32];      // 使用指纹数组替代位图
    alignas(64) uint8_t bits[FILTER_SIZE];
    std::shared_mutex mtx;  // 
    // 缓存字段
    mutable Key_t cached_min_key;
    mutable Key_t cached_max_key;
    mutable Key_t cached_mid_key;
    mutable bool min_key_valid;
    mutable bool max_key_valid;
    mutable bool mid_key_valid;
    mutable uint32_t cached_bitmap_version;  // 用于检测位图变化
    
    // 哈希函数，返回位置
    size_t getPosition(Key_t key, int seed) const {
        // 使用更快的哈希函数
        static constexpr uint64_t PRIME1 = 11400714785074694791ULL;
        static constexpr uint64_t PRIME2 = 14029467366897019727ULL;
        
        uint64_t h = key + seed;
        h ^= h >> 33;
        h *= PRIME1;
        h ^= h >> 29;
        h *= PRIME2;
        h ^= h >> 32;
        
        return h % FILTER_SIZE;
    }
    
    // calculate fingerprint for a key
    uint8_t calculateFingerprint(Key_t key) const {
        return static_cast<uint8_t>((key ^ (key >> 32)) & 0xFF);
    }
    
public:
    BloomFilter() {
        std::memset(fingerprints, 0, 32);
        std::memset(bits, 0, FILTER_SIZE);
        invalidateCache();
    }
    
    void invalidateCache() {
        cached_min_key = std::numeric_limits<Key_t>::max();
        cached_max_key = std::numeric_limits<Key_t>::min();
        cached_mid_key = std::numeric_limits<Key_t>::max();
        min_key_valid = false;
        max_key_valid = false;
        mid_key_valid = false;
        cached_bitmap_version = 0;
    }
    
    void add(Key_t key, int pos) {
        uint8_t fp = calculateFingerprint(key);
        fingerprints[pos] = fp;  // store fingerprint at the specified position
        // set bit at all positions determined by the hash functions
        for (size_t i = 0; i < HASH_FUNCTIONS; i++) {
            size_t pos = getPosition(key, i);
            bits[pos] = 1;
        }
        
        // 智能更新缓存
        updateCacheOnAdd(key);
    }

    void remove(Key_t key, int pos) {
        fingerprints[pos] = 0;
        // 删除时缓存失效（因为需要重新计算）
        invalidateCache();
    }

    bool mightContain(Key_t key) const {
    #ifdef __AVX2__
        const int SIMD_WIDTH = 32;
        // collect all positions for the hash functions
        size_t positions[HASH_FUNCTIONS];
        for (size_t i = 0; i < HASH_FUNCTIONS; i++) {
            positions[i] = getPosition(key, i);
            __builtin_prefetch(&bits[positions[i] & ~(SIMD_WIDTH-1)], 0, 0);  // prefetch aligned memory
        }
        
        // check if all positions are set to 1
        for (size_t i = 0; i < HASH_FUNCTIONS; i++) {
            size_t pos = positions[i];
            size_t aligned_pos = pos & ~(SIMD_WIDTH - 1);  // align to 32-byte boundary
            
            //load 32 bytes starting from aligned position
            __m256i data = _mm256_loadu_si256((__m256i*)&bits[aligned_pos]);
            
            // create a target vector with all bytes set to 1
            __m256i target = _mm256_set1_epi8(1);
            
            // compare the data with the target
            __m256i cmp = _mm256_cmpeq_epi8(data, target);
            int mask = _mm256_movemask_epi8(cmp);
            
            // check if the specific bit for this position is set
            if (!(mask & (1 << (pos - aligned_pos)))) {
                return false;  //if any position is not set, return false
            }
        }
        return true;
    #else
        for (size_t i = 0; i < HASH_FUNCTIONS; i++) {
            size_t pos = getPosition(key, i);
            if (bits[pos] != 1) {
                return false;
            }
        }
        return true;
    #endif
    }

    bool checkFingerprint(Key_t key, int pos) const {
        uint8_t fp = calculateFingerprint(key);
        return fingerprints[pos] == fp;  // 检查指定位置的指纹是否匹配
    }
    
    void clear() {
        std::memset(fingerprints, 0, 32);
        std::memset(bits, 0, FILTER_SIZE);
        invalidateCache();
    }

    // 缓存管理方法
    bool isCacheValid(uint32_t current_bitmap) const {
        return cached_bitmap_version == current_bitmap;
    }
    
    void updateBitmapVersion(uint32_t bitmap) {
        cached_bitmap_version = bitmap;
    }

private:
    void updateCacheOnAdd(Key_t key) {
        // update cached min keys intelligently
        if (!min_key_valid) {
            // if cache is invalid, force recalculation
            // so it can aoid unnecessary updates
        } else if (key < cached_min_key) {
            cached_min_key = key;
        }
        
        // update cached max keys intelligently
        if (!max_key_valid) {
            // if cache is invalid, force recalculation
        } else if (key > cached_max_key) {
            cached_max_key = key;
        }
        
        // invalidate mid key cache
        mid_key_valid = false;
    }
#if 0
    void updateCacheOnAdd(Key_t key) {
        // 智能更新最小值缓存
        if (min_key_valid && key < cached_min_key) {
            cached_min_key = key;
        } else if (!min_key_valid) {
            min_key_valid = false;  // 强制重新计算
        }
        
        // 智能更新最大值缓存
        if (max_key_valid && key > cached_max_key) {
            cached_max_key = key;
        } else if (!max_key_valid) {
            max_key_valid = false;  // 强制重新计算
        }
        
        // 中位数缓存失效（插入后需要重新计算）
        mid_key_valid = false;
    }
#endif
public:
    uint8_t hashKey(Key_t key) const {
        return static_cast<uint8_t>((key ^ (key >> 32)) & 0xFF);
    }

    void updateFingerprint(int pos, Key_t key) {
        fingerprints[pos] = hashKey(key);
    }
};

//extern BloomFilter bf[MAX_VALUE_NODES];

class header{
    public:
        int16_t id; //2 bytes
        int16_t coveredNodes; // 1 byte
        int16_t level; //1 byte
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
        // 边界情况快速处理
        if (hdr.last_index < 0) return 0;
        if (key < gps[0].key) return 0;
        if (key >= gps[hdr.last_index].key) return hdr.last_index + 1;
        
        // 二分查找
        int left = 0, right = hdr.last_index;
        while (left < right) {
            int mid = left + (right - left) / 2;
            if (gps[mid].key <= key) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }
        return left;
    }

    int findKeyPos(Key_t key)
    {
        // 空数组情况
        if (hdr.last_index < 0) return 0;
        
        // 边界情况快速处理
        if (key < gps[0].key) return 0;
        if (key >= gps[hdr.last_index].key) return hdr.last_index;
        
        // 二分查找找到最后一个 <= key 的位置
        int left = 0, right = hdr.last_index;
        int result = 0;
        
        while (left <= right) {
            int mid = left + (right - left) / 2;
            if (gps[mid].key <= key) {
                result = mid;  // 记录当前符合条件的位置
                left = mid + 1;
            } else {
                right = mid - 1;
            }
        }
        
        return result;
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
        if(isHeader() || targetInode->isHeader()) {
            std::cout << " this is also weird" << std::endl;
        }
        memmove(targetInode->gps, &gps[hdr.last_index / 2], sizeof(entry) * (hdr.last_index / 2 + 1));
        int temp_index = hdr.last_index;
        hdr.last_index = hdr.last_index / 2 - 1;
        targetInode->hdr.last_index = temp_index / 2;
        hdr.coveredNodes = hdr.last_index + 1;
        targetInode->hdr.coveredNodes = targetInode->hdr.last_index + 1;
        return true;
    }

    bool insertAtPos(Key_t key, Val_t value, int pos) {
        if(isHeader()) {
            std::cout << "this is weird" << std::endl;
        }
        shift(pos);
        hdr.last_index++;
        gps[pos].key = key;
        gps[pos].value = value;
        hdr.coveredNodes++;
        return true;
    }

    void updateKeyVal(Key_t newKey, int pos) {
        if(isHeader()) {
            std::cout << " this is weird 2" << std::endl;
        }
        gps[pos].key = newKey;
    }
};

class vnodeHeader {
public:
    uint32_t id; //4 bytes
    int next; //4 bytes 
    // used to keep track of the keys are valid or not in the vnode
    uint32_t bitmap; // 4 bytes
    //std::shared_mutex mtx;
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

class Vnode
{
public:
    vnodeHeader hdr;
    entry records[fanout];
    //BloomFilter bloom;
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

    bool lookup(Key_t key, Val_t &value, BloomFilter *bloom) {
        // check bloom filter first
        if (!bloom->mightContain(key)) {
            return false;
        }
        return lookupWithoutFilter(key, value, bloom); // use the existing lookup method
    }

    bool lookup(Key_t key, int &pos, BloomFilter *bloom) {
        // check bloom filter first
        if (!bloom->mightContain(key)) {
            return false;
        }
        return lookupWithoutFilter(key, pos, bloom); // use the existing lookup method
    }

    bool lookupWithoutFilter(Key_t key, Val_t &value, BloomFilter *bloom) 
    {
           // use SIMD to optimize fingerprint comparison
#ifdef __AVX2__
        uint8_t target_fp = bloom->hashKey(key);
        int SIMD_WIDTH = 32;
        for (int32_t i = 0; i < fanout; i += SIMD_WIDTH) {
            // load 32 fingerprints into a vector
            __m256i fp_vec = _mm256_loadu_si256((__m256i*)&bloom->fingerprints[i]);
            // create a vector with the target fingerprint
            __m256i target_vec = _mm256_set1_epi8(target_fp);
            // compare the fingerprints
            int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(fp_vec, target_vec));
            
            // handle the mask to find matching fingerprints
            while (mask) {
                // get the index of the rightmost set bit
                int idx = i + __builtin_ctz(mask);
                if (idx < fanout && hdr.isBitSet(idx) && records[idx].key == key) {
                    value = records[idx].value;
                    return true;
                }
                // clear the rightmost set bit
                mask &= (mask - 1);
            }
        }
#else
        // non-SIMD version for fingerprint comparison
        for (int32_t i = fanout - 1; i >= 0; i--) {
            if (hdr.isBitSet(i) && bloom->checkFingerprint(key, i)) {
                if (records[i].key == key) {
                    value = records[i].value;
                    return true;
                }
            }
        }
#endif
        return false; 
    }

    bool lookupWithoutFilter(Key_t key, int &pos, BloomFilter *bloom) 
    {
#ifdef __AVX2__
        uint8_t target_fp = bloom->hashKey(key);
        // use SIMD to optimize fingerprint comparison
        const int SIMD_WIDTH = 32;
        for (int32_t i = 0; i < fanout; i += SIMD_WIDTH) {
            // load 32 fingerprints into a vector
            __m256i fp_vec = _mm256_loadu_si256((__m256i*)&bloom->fingerprints[i]);
            // create a vector with the target fingerprint
            __m256i target_vec = _mm256_set1_epi8(target_fp);
            // compare the fingerprints
            int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(fp_vec, target_vec));
            
            // handle the mask to find matching fingerprints
            while (mask) {
                // get the index of the rightmost set bit
                int idx = i + __builtin_ctz(mask);
                if (idx < fanout && hdr.isBitSet(idx) && records[idx].key == key) {
                    pos = idx;
                    return true;
                }
                // clear the rightmost set bit
                mask &= (mask - 1);
            }
        }
#else
        // non-SIMD version for fingerprint comparison
        for (int32_t i = fanout - 1; i >= 0; i--) {
            if (hdr.isBitSet(i) && bloom->checkFingerprint(key, i)) {
                if (records[i].key == key) {
                    pos = i;
                    return true;
                }
            }
        }
#endif
        return false;
    }
    Key_t getMaxKey()
    {
        Key_t maxKey = std::numeric_limits<Key_t>::min();
        uint32_t bitmap = hdr.bitmap;
        while(bitmap) {
            int idx = __builtin_ctz(bitmap);
            if(records[idx].key > maxKey) {
                maxKey = records[idx].key;
            }
            bitmap &= (bitmap - 1);
        }
        return maxKey;
    }

    Key_t getMaxKey(BloomFilter *bloom)
    {
        if(bloom && bloom->max_key_valid && bloom->isCacheValid(hdr.bitmap)) {
            return bloom->cached_max_key; // return cached max key if valid
        }
        Key_t maxKey = std::numeric_limits<Key_t>::min();
        uint32_t bitmap = hdr.bitmap;
        while(bitmap) {
            int idx = __builtin_ctz(bitmap);
            if(records[idx].key > maxKey) {
                maxKey = records[idx].key;
            }
            bitmap &= (bitmap - 1);
        }
        //update the bloom filter cache
        if(bloom) {
            bloom->cached_max_key = maxKey;
            bloom->max_key_valid = true;
            bloom->updateBitmapVersion(hdr.bitmap);
        }
        return maxKey;
    }

    Key_t getMinKey() {
        Key_t minKey = std::numeric_limits<Key_t>::max();
        uint32_t bitmap = hdr.bitmap;
        while(bitmap) {
            int idx = __builtin_ctz(bitmap);  // find the lowest set bit
            if(records[idx].key < minKey) {
                minKey = records[idx].key;
            }
            bitmap &= (bitmap - 1);  // clear the lowest set bit
        }
        return minKey;
    }

    Key_t getMinKey(BloomFilter *bloom) {
        if(bloom && bloom->min_key_valid && bloom->isCacheValid(hdr.bitmap)) {
            return bloom->cached_min_key; // return cached min key if valid
        }
        Key_t minKey = std::numeric_limits<Key_t>::max();
        uint32_t bitmap = hdr.bitmap;
        while(bitmap) {
            int idx = __builtin_ctz(bitmap);  // find the lowest set bit
            if(records[idx].key < minKey) {
                minKey = records[idx].key;
            }
            bitmap &= (bitmap - 1);  // clear the lowest set bit
        }
        //update the bloom filter cache
        if(bloom) {
            bloom->cached_min_key = minKey;
            bloom->min_key_valid = true;
            bloom->updateBitmapVersion(hdr.bitmap);
        }
        return minKey;
    }

    Key_t getMidKey() 
    {
        std::vector<Key_t> validKeys;
        validKeys.reserve(fanout);

        uint32_t bitmap = hdr.bitmap;
        while(bitmap) {
            int idx = __builtin_ctz(bitmap);  // find the lowest set bit
            if(records[idx].key != std::numeric_limits<Key_t>::max()) {
                validKeys.push_back(records[idx].key);
            }
            bitmap &= (bitmap - 1);  // clear the lowest set bit
        }

        if (validKeys.empty()) {
            return std::numeric_limits<Key_t>::max(); // or some other sentinel value
        }
        size_t mid = validKeys.size() / 2;
        std::nth_element(validKeys.begin(), validKeys.begin() + mid, validKeys.end());
        return validKeys[mid]; // return the median key
    }

    Key_t getMidKey(BloomFilter *bloom) 
    {
        if(bloom && bloom->mid_key_valid && bloom->isCacheValid(hdr.bitmap)) {
            return bloom->cached_mid_key; // return cached mid key if valid
        }
        std::vector<Key_t> validKeys;
        validKeys.reserve(fanout);

        uint32_t bitmap = hdr.bitmap;
        while(bitmap) {
            int idx = __builtin_ctz(bitmap);  // find the lowest set bit
            if(records[idx].key != std::numeric_limits<Key_t>::max()) {
                validKeys.push_back(records[idx].key);
            }
            bitmap &= (bitmap - 1);  // clear the lowest set bit
        }

        if (validKeys.empty()) {
            return std::numeric_limits<Key_t>::max(); // or some other sentinel value
        }
        size_t mid = validKeys.size() / 2;
        std::nth_element(validKeys.begin(), validKeys.begin() + mid, validKeys.end());
        
        //update the bloom filter cache
        if(bloom) {
            bloom->cached_mid_key = validKeys[mid];
            bloom->mid_key_valid = true;
            bloom->updateBitmapVersion(hdr.bitmap);
        }
        return validKeys[mid]; // return the median key
    }

    //return remaining number of keys need to be scanned
    int scan(Key_t key, size_t range, std::priority_queue<Key_t, std::vector<Key_t>, std::greater<Key_t>> &pq) {
        size_t remaining_range = range;
        for(int32_t i = fanout - 1; i >= 0; i--) {
            if(hdr.isBitSet(i) == false) {
                continue;
            }
            if(records[i].key == std::numeric_limits<Key_t>::max()) {
                continue;
            }
            if(records[i].key < key) {
                continue;
            }
            pq.push(records[i].key);
            remaining_range--;
            if(pq.size() > remaining_range) {
                break;
            }
        }
        return remaining_range;
    }

//Todo: Implement insert with finger print and bloom filter
//find the first empty slot and insert the key and value
    bool insert(Key_t key, Val_t value, BloomFilter *bloom) {
        int32_t pos = __builtin_ffs(~hdr.bitmap) - 1;
        if (pos >= 0 && pos < fanout) {
            records[pos].key = key;
            records[pos].value = value;
            hdr.setBit(pos);
            bloom->add(key, pos);  // 添加到布隆过滤器
#ifdef DBG
            std::cout << "vnode id: " << hdr.id << " insert key: " << key << " value: " << value << " at pos: " << pos << std::endl;
#endif
            return true;
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
    
    bool isFull()
    {
        //return hdr.bitmap == static_cast<uint32_t>((1 << fanout) - 1);
        return __builtin_popcount(hdr.bitmap) == fanout; // check if all bits are set
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
#if 1
            if(hdr.isBitSet(i)) {
                std::cout << "Key: " << records[i].key << " Value: " << records[i].value << std::endl;
            }
#endif
        }
        std::cout << " min: " << getMinKey() << " max: " << getMaxKey() << std::endl;
    }
};
