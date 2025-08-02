#include "valuelist.h"
#include <cassert>

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
    nextNode->hdr.next = curNode->hdr.next;
    curNode->hdr.next = nextNode->getId();
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(nextNode), sizeof(Vnode));
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(curNode), sizeof(Vnode));
    return true;
}

bool ValueList::split(Vnode *curNode, Vnode *nextNode)
{
    assert(nextNode->isEmpty());

    // 1. 收集所有有效记录
    std::vector<vnode_entry> valid_records;
    valid_records.reserve(fanout);

    for (uint32_t i = 0; i < fanout; ++i) {
        if (curNode->hdr.isBitSet(i)) { 
            valid_records.push_back(curNode->records[i]);
        }
    }

    if (valid_records.empty()) return true;

    // 2. 排序以满足 maxKey <= minKey 的要求
    std::sort(valid_records.begin(), valid_records.end(), 
              [](const vnode_entry& a, const vnode_entry& b) { return a.key < b.key; });

    // 3. 确定分裂点
    size_t num_to_keep = valid_records.size() / 2;
    size_t num_to_move = valid_records.size() - num_to_keep;

    // 4. 清理并重新填充节点
    curNode->clear();
    nextNode->clear(); // 确保 nextNode 也是干净的

    // 复制数据回 curNode
    if (num_to_keep > 0) {
        memcpy(curNode->records, valid_records.data(), num_to_keep * sizeof(entry));
        curNode->rebuildMetadata(&bf[curNode->hdr.id], num_to_keep); // 【使用新函数】
    }

    // 复制数据到 nextNode
    if (num_to_move > 0) {
        memcpy(nextNode->records, &valid_records[num_to_keep], num_to_move * sizeof(entry));
        nextNode->rebuildMetadata(&bf[nextNode->hdr.id], num_to_move); // 【使用新函数】
    }

    assert(curNode->getMaxKey() <= nextNode->getMinKey());

    // 5. 更新链表指针
    nextNode->hdr.next = curNode->hdr.next;
    curNode->hdr.next = nextNode->getId();

    // 6. 验证和持久化
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(nextNode), sizeof(Vnode));
    PmemManager::flushToNVM(0, reinterpret_cast<char *>(curNode), sizeof(Vnode));

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

