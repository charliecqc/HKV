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
const int32_t vnode_fanout =28;

class BloomFilter {
public:
    static const size_t FILTER_SIZE = 256;
    static const size_t HASH_FUNCTIONS = 4;
    alignas(64) uint8_t fingerprints[32];
    alignas(64) uint8_t bits[FILTER_SIZE];

    alignas(64) std::atomic<uint64_t> version{0}; // 独占 cacheline
    alignas(64) std::shared_mutex vnode_mtx;      // 与 version 分离，避免伪共享
public:
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
    }
    
    void add(Key_t key, int pos) {
        uint8_t fp = calculateFingerprint(key);
        fingerprints[pos] = fp;  // store fingerprint at the specified position
        // set bit at all positions determined by the hash functions
        for (size_t i = 0; i < HASH_FUNCTIONS; i++) {
            size_t pos = getPosition(key, i);
            bits[pos] = 1;
        }
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
    }
public:
    uint8_t hashKey(Key_t key) const {
        return static_cast<uint8_t>((key ^ (key >> 32)) & 0xFF);
    }

    void updateFingerprint(int pos, Key_t key) {
        fingerprints[pos] = hashKey(key);
    }
};

//extern BloomFilter bf[MAX_VALUE_NODES];

class header {
public:
    int32_t id;
    int16_t level;
    int32_t next;
    int16_t last_index;
    int32_t parent_id;              // 新增
public:
    header() : id(0), level(0), next(0), last_index(-1), parent_id(-1) {}
    friend class Inode;
};

class entry
{
public:
    Key_t key; // 8bytes
    Val_t value;   // 8bytes
    int16_t covered_nodes; // 2 bytes, 记录此GP覆盖的子节点数

    entry() {
        key = std::numeric_limits<Key_t>::max();
        value = std::numeric_limits<Val_t>::max();
        covered_nodes = 0; // 初始化为0
    }
    friend class Inode;
    friend class Vnode;
};

class vnode_entry
{
public:
    Key_t key; // 8bytes
    Val_t value;   // 8bytes
    vnode_entry() {
        key = std::numeric_limits<Key_t>::max();
        value = std::numeric_limits<Val_t>::max();
    }
    friend class Vnode;
    friend class Inode;
};

class Inode
{
public:
    header hdr;
    entry gps[fanout/2];
    entry sgps[fanout/2];
	std::atomic<uint64_t> version{0};
    

    Inode(uint32_t level)
    {
        hdr.level = level;
    }

    Inode(int32_t id, uint32_t level, int next = 0)
    {
        hdr.id = id;
        hdr.next = next;
        hdr.level = level;
        for(int32_t i = 0; i < fanout/2; i++) {
            gps[i].key = std::numeric_limits<Key_t>::max();
            gps[i].value = std::numeric_limits<Val_t>::max();
            sgps[i].key = std::numeric_limits<Key_t>::max();
            sgps[i].value = std::numeric_limits<Val_t>::max();
			version.store(0, std::memory_order_relaxed);
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

    bool activateGP(Key_t targetKey, Val_t value, int &pos, int16_t relative_pos)
    {
        //check if there is enough space to insert the new GP
        int16_t cur_index = this->hdr.last_index;  
        if(static_cast<int32_t>(cur_index + 1)>= fanout/2) {
            return false;
        }else {
            pos = this->findInsertKeyPos(targetKey);
            if(pos < 0 || pos > cur_index + 1) {
                std::cout << "Invalid position for inserting GP: " << pos << std::endl;
                return false;
            }
            assert(pos != 0);
            int old_covered_nodes = gps[pos-1].covered_nodes;
            assert(old_covered_nodes >= 1);
            assert(old_covered_nodes - relative_pos - 1 >= 0);
            this->insertAtPos(targetKey, value, pos, old_covered_nodes - relative_pos - 1);
            this->gps[pos-1].covered_nodes = relative_pos + 1; // set new covered nodes for the previous GP
            assert(this->gps[pos-1].covered_nodes >= 1);
            return true;
        }
    }

    bool activateGPForVnode(Key_t targetKey, int vnode_id, int &pos, int16_t initial_covered_nodes)
    {
        //check if there is enough space to insert the new GP
        int16_t cur_index = this->hdr.last_index;  
        if(static_cast<int32_t>(cur_index + 1)>= fanout/2) {
            return false;
        }else {
            pos = this->findInsertKeyPos(targetKey);
            if(pos < 0 || pos > cur_index + 1) {
                std::cout << "Invalid position for inserting GP: " << pos << std::endl;
                return false;
            }
            assert(pos != 0);
            this->insertAtPos(targetKey, vnode_id, pos, initial_covered_nodes);
            return true;
        }
    }

    bool checkForActivateNextGP(int idx)
    {
#if 0
        int current_level = this->hdr.level;
        double coefficient = (current_level < MAX_LEVEL) ? 
                             SEARCH_STABILITY_COEFFICIENT_BY_LEVEL[current_level] : 
                             SEARCH_STABILITY_COEFFICIENT_BY_LEVEL[MAX_LEVEL - 1];
        if(this->hdr.coveredNodes == 0)
            return true;
        if(this->hdr.coveredNodes > coefficient * (this->hdr.last_index + 1)) {
            return true;
        }
        return false;
#endif
        return isUnbalanced(idx);
    }

    int findInsertKeyPos(Key_t key)
    {
        //handle the boundary cases
        if (hdr.last_index < 0) return 0;
        if (key < gps[0].key) return 0;
        if (key >= gps[hdr.last_index].key) return hdr.last_index + 1;
        
        // binary search for the position
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
        // empty inode
        if (hdr.last_index < 0) return 0;
        
        // handle the boundary cases
        if (key < gps[0].key) return 0;
        if (key >= gps[hdr.last_index].key) return hdr.last_index;
        
        // binary search for the position
        int left = 0, right = hdr.last_index;
        int result = 0;
        
        while (left <= right) {
            int mid = left + (right - left) / 2;
            if (gps[mid].key <= key) {
                result = mid;  // record the last position where gps[mid].key <= key
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
        int total_entries = hdr.last_index + 1;
        int first_half_count = total_entries / 2;
        int second_half_count = total_entries - first_half_count;
        int split_point_index = first_half_count;

        // memmove 会将整个 entry 结构体（包括 key, value, 和 covered_nodes）一起移动
        // 负载信息被正确地分区到新的节点，无需额外操作
        memmove(targetInode->gps, &gps[split_point_index], sizeof(entry) * second_half_count);
        
        // 更新各自的 last_index
        hdr.last_index = first_half_count - 1;
        targetInode->hdr.last_index = second_half_count - 1;
        
        assert(this->getMaxKey() <= targetInode->getMinKey());
    #if 0
        for(int i = 0; i <= hdr.last_index; i++) {
            std::cout << "After split, left inode id: " << this->getId() << " pos: " << i << " key: "<<this->gps[i].key << " covered_nodes: "<< this->gps[i].covered_nodes<< std::endl;
        }
        for(int i = 0; i <= targetInode->hdr.last_index; i++) {
            std::cout << "After split, right inode id: " << targetInode->getId() << " pos: " << i << " key: "<<targetInode->gps[i].key << " covered_nodes: "<< targetInode->gps[i].covered_nodes<< std::endl;
        }
    #endif
        return true;
    }

    // **修改签名，增加 initial_covered_nodes 参数**
    bool insertAtPos(Key_t key, Val_t value, int pos, int16_t initial_covered_nodes) {
        if(isHeader()) {
            std::cout << "this is weird" << std::endl;
        }
        if(pos <= hdr.last_index) {
            shift(pos);
        }
        gps[pos].key = key;
        gps[pos].value = value;
        // **为新GP的 covered_nodes 赋初始值**
        gps[pos].covered_nodes = initial_covered_nodes;
        assert(gps[pos].covered_nodes >= 1);

        hdr.last_index++;
        return true;
    }

    void updateKeyVal(Key_t newKey, int pos) {
        if(isHeader()) {
            std::cout << " this is weird 2" << std::endl;
        }
        gps[pos].key = newKey;
    }

    bool isUnbalanced() {
        int current_level = this->hdr.level;
        double coefficient = (current_level < MAX_LEVEL) ? 
                             SEARCH_STABILITY_COEFFICIENT_BY_LEVEL[current_level] : 
                             SEARCH_STABILITY_COEFFICIENT_BY_LEVEL[MAX_LEVEL - 1];
        
        // 检查是否有任何一个GP的负载过高
        for (int i = 0; i <= this->hdr.last_index; ++i) {
            // 每个GP至少应该覆盖1个节点，如果它覆盖的节点数远超这个基数，则认为不平衡
            if (this->gps[i].covered_nodes > coefficient) {
                 return true;
            }
        }
        return false;
    }

    bool isUnbalanced(int idx) {
        int current_level = this->hdr.level;
        double coefficient = (current_level < MAX_LEVEL) ? 
                             SEARCH_STABILITY_COEFFICIENT_BY_LEVEL[current_level] : 
                             SEARCH_STABILITY_COEFFICIENT_BY_LEVEL[MAX_LEVEL - 1];
#if 0
        int16_t temp_covered_nodes = this->gps[idx].covered_nodes; 
        if(temp_covered_nodes == 4 && this->hdr.level == 1) {
            std::cout << "GP at index " << idx << " has exactly 4 covered nodes." << std::endl;
        }
#endif
        if (this->gps[idx].covered_nodes > coefficient) {
            return true;
        }
        return false;
    }

    inline int32_t getParent() const {
        return hdr.parent_id;
    }

    inline void setParent(int32_t parent_id) {
        hdr.parent_id = parent_id;
    }

    static bool parentCoversChild(Inode* parent, Inode* parent_next, Inode* child) {
        if (!parent || !child) return false;
        Key_t low = parent->getMinKey();
        Key_t high = (parent_next && !parent_next->isTail())
                   ? parent_next->getMinKey()
                   : std::numeric_limits<Key_t>::max();
        Key_t cmk = child->getMinKey();
        return (cmk >= low && cmk < high);
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
    vnode_entry records[vnode_fanout];
    //BloomFilter bloom;
    Vnode(int id, int next = 0)
    {
        hdr.id = id;
        hdr.next = next;
        hdr.bitmap = 0;
        for(int32_t i = 0; i < vnode_fanout; i++) {
            records[i].key = std::numeric_limits<Key_t>::max();
            records[i].value = std::numeric_limits<Val_t>::max();
        }
		//version.store(0, std::memory_order_relaxed);
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
        for (int32_t i = 0; i < vnode_fanout; i += SIMD_WIDTH) {
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
                if (idx < vnode_fanout && hdr.isBitSet(idx) && records[idx].key == key) {
                    value = records[idx].value;
                    return true;
                }
                // clear the rightmost set bit
                mask &= (mask - 1);
            }
        }
#else
        // non-SIMD version for fingerprint comparison
        constexpr uint32_t FULL_MASK = (vnode_fanout >= 32u)
            ? 0xFFFFFFFFu
            : ((1u << vnode_fanout) - 1u);
        uint32_t bm = hdr.bitmap & FULL_MASK;

        // 可选的指纹预过滤（bloom 可能为 nullptr）
        uint8_t fp = 0;
        const bool use_fp = (bloom != nullptr);
        if (use_fp) fp = bloom->hashKey(key);

        while (bm) {
            int idx = __builtin_ctz(bm);
            bm &= (bm - 1);
            if (!use_fp || bloom->fingerprints[idx] == fp) {
                if (records[idx].key == key) {
                    value = records[idx].value;
                    return true;
                }
            }
        }
        return false;
#endif
        return false; 
    }

    bool lookupWithoutFilter(Key_t key, int &pos, BloomFilter *bloom) 
    {
#ifdef __AVX2__
        uint8_t target_fp = bloom->hashKey(key);
        // use SIMD to optimize fingerprint comparison
        const int SIMD_WIDTH = 32;
        for (int32_t i = 0; i < vnode_fanout; i += SIMD_WIDTH) {
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
                if (idx < vnode_fanout && hdr.isBitSet(idx) && records[idx].key == key) {
                    pos = idx;
                    return true;
                }
                // clear the rightmost set bit
                mask &= (mask - 1);
            }
        }
#else
        // non-SIMD version for fingerprint comparison
        for (int32_t i = vnode_fanout - 1; i >= 0; i--) {
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

    Key_t getMidKey() 
    {
        std::vector<Key_t> validKeys;
        validKeys.reserve(vnode_fanout);

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

    //return remaining number of keys need to be scanned
    int scan(Key_t key, size_t range, std::priority_queue<Key_t, std::vector<Key_t>, std::greater<Key_t>> &pq) {
        size_t remaining_range = range;
        for(int32_t i = vnode_fanout - 1; i >= 0; i--) {
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
    inline bool insert(Key_t key, Val_t value, BloomFilter* bloom) {
        constexpr uint32_t FULL_MASK = (vnode_fanout >= 32u)
            ? 0xFFFFFFFFu
            : ((1u << vnode_fanout) - 1u);
    uint32_t free_mask = (~hdr.bitmap) & FULL_MASK;
    if (free_mask == 0) return false;
    int pos = __builtin_ctz(free_mask);
    records[pos].key   = key;
    records[pos].value = value;
    hdr.setBit(pos);
    if (bloom) bloom->add(key, pos);
    return true;
}

    void clear() {
        hdr.bitmap = 0;
    }

    void rebuildMetadata(BloomFilter *bloom, int rebuild_count) {
        bloom->clear();  // 清空布隆过滤器
        hdr.bitmap = 0;
        for (int32_t i = 0; i < rebuild_count; i++) {
            if (records[i].key != std::numeric_limits<Key_t>::max()) {
                hdr.setBit(i);
                bloom->add(records[i].key, i);  // 添加到布隆过滤器
            }
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
        return hdr.bitmap == static_cast<uint32_t>((1 << vnode_fanout) - 1);
    }

    bool isEmpty()
    {
        return hdr.bitmap == 0;
    }

    void dump()
    {
        std::cout << "Vnode id: " << hdr.id << " next: " << hdr.next << " bitmap (binary): ";
        for (int i = vnode_fanout - 1; i >= 0; i--) {
            std::cout << ((hdr.bitmap >> i) & 1);
        }
        std::cout << std::endl;
        for(int32_t i = 0; i < vnode_fanout; i++) {
#if 0
            if(hdr.isBitSet(i)) {
                std::cout << "Key: " << records[i].key << " Value: " << records[i].value << std::endl;
            }
#endif
        }
        std::cout << " min: " << getMinKey() << " max: " << getMaxKey() << std::endl;
    }
};

inline void write_start(std::atomic<uint64_t>& v) {
    v.fetch_add(1, std::memory_order_acq_rel); // 奇数：进入写区间
}
inline void write_end(std::atomic<uint64_t>& v) {
    v.fetch_add(1, std::memory_order_release); // 偶数：退出写区间
}

template<class Fn>
auto read_consistent(const std::atomic<uint64_t>& v, Fn&& fn) -> decltype(fn()) {
    for (uint32_t spins = 0;; ++spins) {
        uint64_t a = v.load(std::memory_order_acquire);
        if (a & 1u) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
            if ((spins & 0x7FFF) == 0x7FFF) std::this_thread::yield();
            continue;
        }
        auto out = fn();
        std::atomic_thread_fence(std::memory_order_acquire);
        uint64_t b = v.load(std::memory_order_acquire);
        if (a == b) return out;
    }
}
