#include "valuelist.h"
#include <cassert>
#include <vector>
#include <algorithm>
#include <limits>
#include <cstring>

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
    //std::unique_lock<std::shared_mutex> lock(curNode->hdr.mtx);
    write_start(curNode->version);
    write_start(nextNode->version);
    nextNode->hdr.next = curNode->hdr.next;
    curNode->hdr.next = nextNode->getId();
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(nextNode), sizeof(Vnode));
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(curNode), sizeof(Vnode));
    write_end(nextNode->version);
    write_end(curNode->version);
    return true;
}

bool ValueList::split(Vnode *curNode, Vnode *nextNode)
{
    assert(nextNode->isEmpty());

    // 仅遍历已用槽位
    uint32_t used_mask = curNode->hdr.bitmap;
    int used = __builtin_popcount(used_mask);
    if (used < 2) {

        return true; // 不足以分裂
    }

    struct KI { int pos; Key_t key; };
    std::vector<KI> items;
    items.reserve(used);

    Key_t gmin = std::numeric_limits<Key_t>::max();
    Key_t gmax = std::numeric_limits<Key_t>::min();

    for (uint32_t bm = used_mask; bm; bm &= (bm - 1)) {
        int i = __builtin_ctz(bm);
        Key_t k = curNode->records[i].key;
        items.push_back({ i, k });
        if (k < gmin) gmin = k;
        if (k > gmax) gmax = k;
    }
    // 全部键相同，无法满足 prev.max < next.min
    if (gmin == gmax) return false;

    // 选取“右半起点”的键作为 pivot，构造严格阈值 right_min_key
    const size_t right_begin_rank = items.size() / 2;
    std::vector<Key_t> keys;
    keys.reserve(items.size());
    for (auto &it : items) keys.push_back(it.key);

    std::nth_element(keys.begin(), keys.begin() + right_begin_rank, keys.end());
    Key_t pivot = keys[right_begin_rank];

    Key_t right_min_key = std::numeric_limits<Key_t>::max();
    for (Key_t k : keys) if (k >= pivot && k < right_min_key) right_min_key = k;

    // 确保左侧存在严格小于 right_min_key 的元素
    bool has_left = false;
    for (Key_t k : keys) { if (k < right_min_key) { has_left = true; break; } }
    if (!has_left) {
        Key_t strict_gt = std::numeric_limits<Key_t>::max();
        for (Key_t k : keys) if (k > pivot && k < strict_gt) strict_gt = k;
        if (strict_gt == std::numeric_limits<Key_t>::max()) return false; // 无法形成严格不等式
        right_min_key = strict_gt;
    }

    // 选择要搬移的槽位：key >= right_min_key
    uint32_t move_mask = 0;
    for (auto &it : items) {
        if (it.key >= right_min_key) move_mask |= (1u << it.pos);
    }
    int right_cnt = __builtin_popcount(move_mask);
    int left_cnt  = used - right_cnt;
    if (right_cnt == 0 || left_cnt == 0) return false;

    // 构建 nextNode：写入对应槽位并置位 bitmap；curNode 仅清除位（不清空数据）
    write_start(curNode->version);
    write_start(nextNode->version);

    nextNode->hdr.bitmap = 0;
    for (uint32_t mm = move_mask; mm; mm &= (mm - 1)) {
        int i = __builtin_ctz(mm);
        nextNode->records[i] = curNode->records[i];
        nextNode->hdr.bitmap |= (1u << i);
    }
    curNode->hdr.bitmap &= ~move_mask; // 仅无效化被移除的 bit

    // 重建 Bloom（简单起见全量重建，也可按位增量更新）
    {
        BloomFilter *srcBloom = &bf[curNode->hdr.id];
        BloomFilter *dstBloom = &bf[nextNode->hdr.id];
        srcBloom->clear();
        for (uint32_t bm = curNode->hdr.bitmap; bm; bm &= (bm - 1)) {
            int i = __builtin_ctz(bm);
            srcBloom->add(curNode->records[i].key, i);
        }
        dstBloom->clear();
        for (uint32_t bm = nextNode->hdr.bitmap; bm; bm &= (bm - 1)) {
            int i = __builtin_ctz(bm);
            dstBloom->add(nextNode->records[i].key, i);
        }
    }

    // 链接并持久化
    nextNode->hdr.next = curNode->hdr.next;
    curNode->hdr.next  = nextNode->getId();

    PmemManager::flushToNVM(0, reinterpret_cast<char *>(nextNode), sizeof(Vnode));
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(curNode), sizeof(Vnode));

    write_end(nextNode->version);
    write_end(curNode->version);

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
    while(nextNode != nullptr && nextNode->getMaxKey() <= key) {
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
    //shared_lock<std::shared_mutex> lock(curNode->hdr.mtx);
    BloomFilter *bloom = &bf[curNode->hdr.id];
    std::shared_lock<std::shared_mutex> lock(bloom->vnode_mtx);
    return pmemVnodePool->at(curNode->hdr.next);
}

int ValueList::getKeyPos(Key_t key)
{
    Vnode *curNode = head;
    while(true) {
        if(curNode->getMaxKey() < key) {
            curNode = getNext(curNode);
            continue;
        }
        break;
    }
    return curNode->getKeyPos(key);
}

