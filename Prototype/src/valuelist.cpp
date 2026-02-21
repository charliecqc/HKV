#include "valuelist.h"
#include <cassert>
#include <vector>
#include <algorithm>
#include <limits>
#include <cstring>

ValueList::ValueList(string storagePath, PmemBFPool *bfPool) {
    pmemVnodePool = new PmemVnodePool(sizeof(Vnode), MAX_VALUE_NODES, storagePath);
    pmemBFPool = bfPool;
#if ENABLE_DRAM_BLOOM_FULL
    for (auto &slot : dramBloomChunks) {
        slot.store(nullptr, std::memory_order_relaxed);
    }
#endif
    fileName = storagePath;
    if(pmemVnodePool->getCurrentIdx() != 0) {
        head = pmemVnodePool->at(0);
    }else {
        head = pmemVnodePool->getNextNode();
        head->hdr.next = std::numeric_limits<uint32_t>::max();
    }

#if ENABLE_DRAM_BLOOM_FULL
    size_t loaded = pmemVnodePool->getCurrentIdx();
    ensureBloomForCount(loaded + 1);
    for (size_t i = 0; i <= loaded; ++i) {
        BloomFilter *dst = getBloom(i);
        BloomFilter *src = pmemBFPool->at(i);
        dst->version.store(src->version.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dst->next_id = src->next_id;
        dst->min_key = src->min_key;
        std::memcpy(dst->fingerprints, src->fingerprints, sizeof(dst->fingerprints));
    }
#endif

}   

ValueList::~ValueList()
{
#if ENABLE_DRAM_BLOOM_FULL
    for (auto &slot : dramBloomChunks) {
        BloomFilter *base = slot.load(std::memory_order_relaxed);
        if (base) {
            delete[] base;
            slot.store(nullptr, std::memory_order_relaxed);
        }
    }
#endif
}

size_t ValueList::getAllocatedBloomCount() const
{
#if ENABLE_DRAM_BLOOM_FULL
    size_t chunks = 0;
    for (const auto &slot : dramBloomChunks) {
        if (slot.load(std::memory_order_relaxed) != nullptr) {
            ++chunks;
        }
    }
    return chunks * kBloomChunkSize;
#else
    return pmemVnodePool ? (pmemVnodePool->getCurrentIdx() + 1) : 0;
#endif
}

size_t ValueList::getAllocatedBloomBytes() const
{
    return getAllocatedBloomCount() * sizeof(BloomFilter);
}

#if ENABLE_DRAM_BLOOM_FULL
void ValueList::ensureBloomForCount(size_t count)
{
    if (count == 0) {
        return;
    }
    size_t neededChunks = (count + kBloomChunkSize - 1) / kBloomChunkSize;
    if (neededChunks > kMaxBloomChunks) {
        neededChunks = kMaxBloomChunks;
    }

    for (size_t chunk = 0; chunk < neededChunks; ++chunk) {
        if (dramBloomChunks[chunk].load(std::memory_order_acquire) != nullptr) {
            continue;
        }
        std::lock_guard<std::mutex> lock(bloomAllocMutex);
        if (dramBloomChunks[chunk].load(std::memory_order_relaxed) == nullptr) {
            BloomFilter *newChunk = new BloomFilter[kBloomChunkSize];
            dramBloomChunks[chunk].store(newChunk, std::memory_order_release);
        }
    }
}
#endif

BloomFilter *ValueList::getBloom(size_t vnodeId)
{
#if ENABLE_DRAM_BLOOM_FULL
    if (vnodeId >= MAX_VALUE_NODES) {
        return nullptr;
    }

    size_t chunk = vnodeId / kBloomChunkSize;
    size_t offset = vnodeId % kBloomChunkSize;
    BloomFilter *base = dramBloomChunks[chunk].load(std::memory_order_acquire);
    if (base == nullptr) {
        ensureBloomForCount(vnodeId + 1);
        base = dramBloomChunks[chunk].load(std::memory_order_acquire);
        if (base == nullptr) {
            return nullptr;
        }
    }
    return &base[offset];
#else
    return pmemBFPool->at(vnodeId);
#endif
}

void ValueList::syncBloomToPMEM(size_t count)
{
#if ENABLE_DRAM_BLOOM_FULL
    if (count > MAX_VALUE_NODES) {
        count = MAX_VALUE_NODES;
    }
    for (size_t i = 0; i < count; ++i) {
        BloomFilter *dst = pmemBFPool->at(i);
        BloomFilter *src = getBloom(i);
        if (dst == nullptr || src == nullptr) {
            continue;
        }
        dst->version.store(src->version.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dst->next_id = src->next_id;
        dst->min_key = src->min_key;
        std::memcpy(dst->fingerprints, src->fingerprints, sizeof(src->fingerprints));
    }
#else
    (void)count;
#endif
}

//This function will only be called once upon the first insert, curNode is always
//header vnode and write lock is hold in the caller
bool ValueList::append(Vnode *curNode, Vnode *nextNode)
{
    nextNode->hdr.next = curNode->hdr.next;
    curNode->hdr.next = nextNode->getId();
    unsigned long hdr_flush = PmemManager::align_uint_to_cacheline(sizeof(vnodeHeader));
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(&nextNode->hdr), hdr_flush);
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(&curNode->hdr), hdr_flush);
    return true;
}

bool ValueList::split(Vnode* &curNode, Vnode* &nextNode)
{
    //assert(nextNode->isEmpty());

    // 仅遍历已用槽位
    uint64_t used_mask = curNode->hdr.bitmap;
    int used = __builtin_popcountll(used_mask);
    if (used < 2) {

        return true; // 不足以分裂
    }

    struct KI { int pos; Key_t key; };
    std::vector<KI> items;
    items.reserve(used);

    Key_t gmin = std::numeric_limits<Key_t>::max();
    Key_t gmax = std::numeric_limits<Key_t>::min();

    for (uint64_t bm = used_mask; bm; bm &= (bm - 1)) {
        int i = __builtin_ctzll(bm);
        Key_t k = curNode->records[i].key;
        items.push_back({ i, k });
        if (k < gmin) gmin = k;
        if (k > gmax) gmax = k;
    }
    //all keys are the same, no need to split, unset all but one bit
    if (gmin == gmax) {
        curNode->hdr.bitmap &= 1ull;
        return true;
    }

    // 选取“右半起点”的键作为 pivot，构造严格阈值 right_min_key
    const size_t right_begin_rank = items.size() / 2;
    std::vector<Key_t> keys;
    keys.reserve(items.size());
    for (auto &it : items) keys.push_back(it.key);

    std::nth_element(keys.begin(), keys.begin() + right_begin_rank, keys.end());
    Key_t pivot = keys[right_begin_rank];

#if 0
    Key_t right_min_key = std::numeric_limits<Key_t>::max();
    for (Key_t k : keys) {
        if (k >= pivot && k < right_min_key) {
            right_min_key = k;
        }
    }
#endif

    Key_t right_min_key = pivot;

    // 确保左侧存在严格小于 right_min_key 的元素
    bool has_left = false;
    for (Key_t k : keys) { 
        if (k < right_min_key) { 
            has_left = true; 
            break; 
        } 
    }
    if (!has_left) {
        Key_t strict_gt = std::numeric_limits<Key_t>::max();
        for (Key_t k : keys) if (k > pivot && k < strict_gt) strict_gt = k;
        if (strict_gt == std::numeric_limits<Key_t>::max()) return false; // 无法形成严格不等式
        right_min_key = strict_gt;
    }

    Key_t left_min_key = gmin;

    // 选择要搬移的槽位：key >= right_min_key
    uint64_t move_mask = 0;
    for (auto &it : items) {
        if (it.key >= right_min_key) move_mask |= (1ull << it.pos);
    }
    int right_cnt = __builtin_popcountll(move_mask);
    int left_cnt  = used - right_cnt;
    if (right_cnt == 0 || left_cnt == 0) return false;

    nextNode = pmemVnodePool->getNextNode();
    // 构建 nextNode：写入对应槽位并置位 bitmap；curNode 仅清除位（不清空数据）
    BloomFilter *srcBloom = getBloom(curNode->hdr.id);
    BloomFilter *dstBloom = getBloom(nextNode->hdr.id);

    nextNode->hdr.bitmap = 0;
    for (uint64_t mm = move_mask; mm; mm &= (mm - 1)) {
        int i = __builtin_ctzll(mm);
        nextNode->records[i] = curNode->records[i];
        nextNode->hdr.bitmap |= (1ull << i);
    }
    curNode->hdr.bitmap &= ~move_mask; // 仅无效化被移除的 bit
    srcBloom->setMinKey(left_min_key);
    dstBloom->setMinKey(right_min_key);

    // 重建 Bloom（简单起见全量重建，也可按位增量更新）
    {
        srcBloom->clear();
        for (uint64_t bm = curNode->hdr.bitmap; bm; bm &= (bm - 1)) {
            int i = __builtin_ctzll(bm);
            srcBloom->add(curNode->records[i].key, i);
        }
        dstBloom->clear();
        for (uint64_t bm = nextNode->hdr.bitmap; bm; bm &= (bm - 1)) {
            int i = __builtin_ctzll(bm);
            dstBloom->add(nextNode->records[i].key, i);
        }
    }

    // 链接
    nextNode->hdr.next = curNode->hdr.next;
    curNode->hdr.next  = nextNode->getId();
    

     // 仅持久化被改动的区域
    const unsigned long rec_flush = PmemManager::align_uint_to_cacheline(sizeof(vnode_entry));
    for (uint64_t mm = move_mask; mm; mm &= (mm - 1)) {
        int i = __builtin_ctzll(mm);
        PmemManager::flushToNVM(0, reinterpret_cast<char*>(&nextNode->records[i]), rec_flush);
    }
    const unsigned long hdr_flush = PmemManager::align_uint_to_cacheline(sizeof(vnodeHeader));
    PmemManager::flushToNVM(0, reinterpret_cast<char*>(&nextNode->hdr), hdr_flush);
    PmemManager::flushToNVM(0, reinterpret_cast<char*>(&curNode->hdr),  hdr_flush);

    dstBloom->setNextId(nextNode->hdr.next);
    srcBloom->setNextId(nextNode->getId());

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

bool ValueList::recovery()
{
    return true;
}

