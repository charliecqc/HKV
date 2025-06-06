#include <queue>
#include <vector>
#include "tandemIndex.h"
#include "valuelist.h"
#include "spinLock.h"
#include "workerThread.h"
#include "checkpoint.h"
#include "common.h"

std::queue<CheckpointVector *> g_checkpointQueue;
bool wqReady[WORKERQUEUE_NUM] = {false};
volatile bool wtInitialized = false;
volatile bool mgInitialized = false;
std::atomic<bool> g_endTandem;
SpinLock g_spinLock;

#define LOG_SIZE 1UL*1024UL*1024UL*1024UL

TandemIndex::TandemIndex() {
    g_endTandem = false;
    valueList = new ValueList();
    pmemRecoveryArray = new PmemInodePool(sizeof(Inode), MAX_NODES);
    recoveryManager = new RecoveryManager(pmemRecoveryArray); 
    int level = recoveryManager->recoveryOperation();
    dramInodePool = recoveryManager->getDramInodePool();
    ckptLog = new CkptLog(LOG_SIZE);
    PmemManager::flushToNVM(3, reinterpret_cast<char *>(ckptLog), sizeof(ckptLog));
    mainIndex = new DramSkiplist(ckptLog, dramInodePool,valueList);
    mainIndex->setLevel(level);
    createLogMergeThread();
    Inode *index_header = mainIndex->getHeader();
    Vnode *value_header = valueList->getHeader();
    index_header->gps[0].value = value_header->getId();
    dram_log_entry_t *header_entry = new dram_log_entry_t(index_header->getId(), index_header->hdr.coveredNodes,index_header->hdr.last_index,index_header->hdr.next);
    header_entry->setKeyVal(0, index_header->gps[0].key, index_header->gps[0].value);
    ckptLog->enq(header_entry);
}

TandemIndex::~TandemIndex() {
   g_endTandem = true; 
   if (logMergeThread->joinable()) {
       logMergeThread->join();
       delete logMergeThread;
   }
   
   Inode *superNode = pmemRecoveryArray->at(MAX_NODES - 1);
    if(superNode != nullptr) {
         superNode->hdr.last_index = dramInodePool->getCurrentIdx();
         superNode->hdr.level = mainIndex->getLevel();
         PmemManager::flushToNVM(1, reinterpret_cast<char *>(superNode), sizeof(Inode));
    } 

    Vnode *metaVnode = valueList->pmemVnodePool->at(MAX_VALUE_NODES - 1);
    if(metaVnode != nullptr) {
        metaVnode->hdr.next = valueList->pmemVnodePool->getCurrentIdx();
        PmemManager::flushToNVM(0, reinterpret_cast<char *>(metaVnode), sizeof(Vnode));
    }
}
#if 0
bool TandemIndex::insert(Key_t key, Val_t value)
{
    int idx = -1;
    bool ret = false;
    bool needToRebalance = false;
    Inode *inode = mainIndex->lookup(key, idx);
    Vnode *targetVnode = nullptr;
    
    if(inode != nullptr) {
        return handleExistingInodeInsert(inode, key, value, idx, needToRebalance, targetVnode);
    } else {
        return handleNewInodeInsert(key, value);
    }
    
    // 处理重平衡 - 在函数最后统一处理
    if(needToRebalance && inode && targetVnode) {
        std::unique_lock<std::shared_mutex> rebalance_lock(mainIndex->rebalance_lock);
        ret = mainIndex->rebalanceInode(*inode, *targetVnode);
    }
    return ret;
}
#endif
// 处理已存在inode的插入逻辑
bool TandemIndex::handleExistingInodeInsert(Inode *inode, Key_t key, Val_t value, 
                                          int idx, bool &needToRebalance, Vnode* &targetVnode)
{
      // 初始化引用参数
    needToRebalance = false;
    targetVnode = nullptr;
    
    // 1. 获取初始vnode - 缩小锁范围
    Vnode *valueNode = nullptr;
    {
        std::shared_lock<std::shared_mutex> inode_lock(mainIndex->inode_locks[inode->getId()]);
        valueNode = valueList->pmemVnodePool->at(inode->gps[idx].value);
    } // inode_lock 在这里自动释放
    
    // 2. 遍历vnode链寻找插入位置
    while(true) {
        // 预先获取bloom filter指针，避免重复计算
        BloomFilter *bloom = &valueList->bf[valueNode->hdr.id];
        
        // 使用RAII管理锁，缩小锁范围
        {
            std::unique_lock<std::shared_mutex> lock_value(bloom->mtx);
            
            // 缓存maxKey避免重复计算
            Key_t maxKey = valueNode->getMaxKey(bloom);
            
            if(key > maxKey && valueNode->hdr.next != -1) {
                // 移动到下一个节点 - 先获取下一个节点再释放锁
                Vnode *nextNode = valueList->pmemVnodePool->at(valueNode->hdr.next);
                lock_value.unlock(); // 手动释放当前锁
                valueNode = nextNode;
                continue;
            }
            
            // 尝试直接插入
            if(valueNode->insert(key, value, &valueList->bf[valueNode->getId()])) {
                return true; // 插入成功，锁自动释放
            }
            
            // 节点已满，需要分裂
            targetVnode = valueList->pmemVnodePool->getNextNode();
            if(!targetVnode) {
                return false;
            }
            
            // 执行分裂操作
            valueList->split(valueNode, targetVnode);
            
            // 根据分裂后的maxKey决定插入位置
            Key_t splitMaxKey = valueNode->getMaxKey(bloom);
            if(key <= splitMaxKey) {
                // 插入到原节点
                if(!valueNode->insert(key, value, &valueList->bf[valueNode->getId()])) {
                    std::cout << "Failed to insert after split. key: " << key << std::endl;
                    return false;
                }
            } else {
                // 插入到新节点 - 需要获取新节点的锁
                BloomFilter *target_bloom = &valueList->bf[targetVnode->getId()];
                std::unique_lock<std::shared_mutex> lock_target(target_bloom->mtx);
                if(!targetVnode->insert(key, value, target_bloom)) {
                    std::cout << "Failed to insert into new vnode." << std::endl;
                    return false;
                }
            }
            break; // 退出循环
        } // lock_value 在这里自动释放
    }
    
    // 3. 更新inode元数据 - 独立的锁作用域
    return updateInodeAfterSplit(inode, targetVnode, needToRebalance);
}

// 更新inode元数据的独立函数
bool TandemIndex::updateInodeAfterSplit(Inode *inode, Vnode *targetVnode, bool &needToRebalance)
{
    std::unique_lock<std::shared_mutex> inode_wlock(mainIndex->inode_locks[inode->getId()]);
    inode->hdr.coveredNodes++;
    
    if(!inode->checkForActivateGP()) {
        return true; // 不需要激活GP
    }
    
    // 获取目标键 - 使用独立的锁作用域
    Key_t targetKey;
    {
        BloomFilter *bloom = &valueList->bf[targetVnode->hdr.id];
        std::shared_lock<std::shared_mutex> lock_target(bloom->mtx);
        targetKey = targetVnode->getMinKey(bloom);
    } // target锁在这里释放
    
    int pos = -1;
    if(inode->activateGP(targetKey, pos)) {
        // 执行链接操作
        bool ret = mainIndex->linkVnodeToInode(*inode, pos, *targetVnode);
        if(!ret) {
            std::cout << "Failed to link vnode to inode." << std::endl;
            return false;
        }
        
        inode->gps[pos].key = targetKey;
        
        // 创建日志条目 - 使用智能指针避免内存泄漏
        auto entry = std::make_unique<dram_log_entry_t>(
            inode->getId(), inode->hdr.coveredNodes, 
            inode->hdr.last_index, inode->hdr.next);
            
        for(int i = 0; i <= inode->hdr.last_index; i++) {
            entry->setKeyVal(i, inode->gps[i].key, inode->gps[i].value);
        }
        entry->setCoveredNodes(inode->hdr.coveredNodes);
        entry->setLastIndex(inode->hdr.last_index);
        
        ckptLog->enq(entry.release()); // 转移所有权
        return true;
    } else {
        needToRebalance = true;
        return true;
    }
}

// 处理新inode插入的优化版本
bool TandemIndex::handleNewInodeInsert(Key_t key, Val_t value)
{
    Vnode *headVnode = valueList->getHeader();
    Vnode *targetVnode = nullptr;
    
    // 方案1：使用更大粒度的锁保护整个操作
    BloomFilter *head_bloom = &valueList->bf[headVnode->getId()];
    std::unique_lock<std::shared_mutex> head_lock(head_bloom->mtx);
    
    // 1. 尝试插入到现有的第一个vnode
    if(headVnode->hdr.next != -1) {
        targetVnode = valueList->pmemVnodePool->at(headVnode->hdr.next);
        
        // 尝试插入到第一个数据节点
        BloomFilter *bloom = &valueList->bf[targetVnode->getId()];
        std::unique_lock<std::shared_mutex> lock_target(bloom->mtx);
        
        if(!targetVnode->isFull()) {
            Key_t old_min_key = targetVnode->getMinKey(bloom);
            if(targetVnode->insert(key, value, bloom)) {
                // 传播新的最小键
                return mainIndex->update(old_min_key, key, value);
            }
        }
    }
    
    // 2. 创建新节点并插入
    targetVnode = valueList->pmemVnodePool->getNextNode();
    if(!targetVnode) {
        std::cout << "Failed to get new node from pool." << std::endl;
        return false;
    }
    
    // 插入到新节点
    bool ret = targetVnode->insert(key, value, &valueList->bf[targetVnode->getId()]);
    if(!ret) {
        std::cout << "Failed to insert into new vnode. Key: " << key << std::endl;
        return false;
    }
    
    // 添加到值列表 - 此时仍持有头节点锁
    ret = valueList->append(headVnode, targetVnode);
    if(!ret) {
        std::cout << "Failed to append to value list." << std::endl;
        return false;
    }
    
    head_lock.unlock(); // 在所有操作完成后才释放头节点锁
    
    // 插入到主索引
    ret = mainIndex->insert(targetVnode);
    if(!ret) {
        std::cout << "Failed to insert into main index." << std::endl;
    }
    
    return ret;
}

bool TandemIndex::insert(Key_t key, Val_t value)
{
//0. find the start of the vnode chain in the value list
    int idx = -1;
    bool ret = false;
    bool needToRebalance = false;
    Inode *inode = mainIndex->lookup(key, idx);
    Vnode *targetVnode = nullptr; // new vnode to be inserted
    if(inode != nullptr) {
        ret = handleExistingInodeInsert(inode, key, value, idx, needToRebalance, targetVnode);
    }else {
        ret = handleNewInodeInsert(key, value);
    }

     if(needToRebalance && inode && targetVnode) {
        std::unique_lock<std::shared_mutex> rebalance_lock(mainIndex->rebalance_lock);
        ret = mainIndex->rebalanceInode(*inode, *targetVnode);
     }
    return ret;
}

Val_t TandemIndex::lookup(Key_t key)
{
    int idx = -1;
    Vnode *vnode = nullptr;
    Val_t value;
    Inode *inode = mainIndex->lookup(key, idx);
    if(inode == nullptr) {
        return -1;
    }
    
    // get the start of the vnode chain
    {
        std::shared_lock<std::shared_mutex> lock(mainIndex->inode_locks[inode->getId()]);
        vnode = valueList->pmemVnodePool->at(inode->gps[idx].value);
    }
    
    // get the bloom filter for the vnode
    BloomFilter *bloom = &valueList->bf[vnode->hdr.id];
    std::shared_lock<std::shared_mutex> current_lock(bloom->mtx);
    
    while(true) {
        bool mightContain = bloom->mightContain(key);
        
        if(vnode->hdr.next != -1 && !mightContain) {
            // hand over lock to the next vnode
            Vnode *next = valueList->pmemVnodePool->at(vnode->hdr.next);
            BloomFilter *next_bloom = &valueList->bf[next->hdr.id];
            std::shared_lock<std::shared_mutex> next_lock(next_bloom->mtx);
            
            // unlock the current vnode
            current_lock.unlock();
            vnode = next;
            bloom = next_bloom;
            current_lock = std::move(next_lock);
            continue;
        }
        
        if(mightContain) {
            if(vnode->lookupWithoutFilter(key, value, bloom)) {
                return value;
            } else {
                // false positive, check next vnode
                if (key > vnode->getMaxKey(bloom) && vnode->hdr.next != -1) {
                    Vnode *next = valueList->pmemVnodePool->at(vnode->hdr.next);
                    BloomFilter *next_bloom = &valueList->bf[next->hdr.id];
                    std::shared_lock<std::shared_mutex> next_lock(next_bloom->mtx);
                    
                    current_lock.unlock();
                    vnode = next;
                    bloom = next_bloom;
                    current_lock = std::move(next_lock);
                    continue;
                } else {
                    return -1;
                }
            }
        } else {
            return -1;
        }
    }
}

void TandemIndex::createLogMergeThread()
{
    g_spinLock.lock();
    logMergeThread = new std::thread(&TandemIndex::logMergeThreadExec, this, 0);
    mgInitialized = true;
    g_spinLock.unlock();
}

void TandemIndex::logMergeThreadExec(int id)
{
    LogMergeThread lmt(id, this->ckptLog, this->pmemRecoveryArray);
    while(true)
    {
        g_spinLock.lock();
        if(!mgInitialized) {
            g_spinLock.unlock();
            usleep(500);
            continue;
        }else {
            g_spinLock.unlock();
            break;
        }
        g_spinLock.unlock();
    }
    while(!g_endTandem) {
        usleep(200);
        lmt.logMergeOperation();
    }
}

void TandemIndex::recover(Key_t key)
{
    //recoveryManager->recoveryOperation(key);
}


void TandemIndex::update(Key_t key, Val_t value)
{
    int idx = -1;
    bool needToRebalance = false;
    Vnode *targetVnode = nullptr; // new vnode to be inserted
    Inode *inode = mainIndex->lookup(key, idx);
    if(inode == nullptr) {
        std::cout << "Failed to find the inode for the key: " << key << std::endl;
        return;
    }
    std::shared_lock<std::shared_mutex> inode_lock(mainIndex->inode_locks[inode->getId()]);
    Vnode *vnode = valueList->pmemVnodePool->at(inode->gps[idx].value);
    inode_lock.unlock();
    if(vnode == nullptr) {
        std::cout << "Failed to find the vnode for the key: " << key << std::endl;
        return;
    }
    while(true) {
        BloomFilter *bloom = &valueList->bf[vnode->hdr.id];
        std::unique_lock<std::shared_mutex> lock(bloom->mtx);
        if(key > vnode->getMaxKey(bloom) && vnode->hdr.next != -1) {
        //if(vnode->hdr.next != -1 && !valueList->bf[vnode->hdr.id].mightContain(key)) {
            vnode = valueList->pmemVnodePool->at(vnode->hdr.next);
        }else {
            int pos = -1;
            if(vnode->lookup(key,pos, &valueList->bf[vnode->hdr.id])) {
                if(!vnode->isFull()) {
                    vnode->insert(key, value, &valueList->bf[vnode->hdr.id]);
                    vnode->hdr.unsetBit(pos);
                    return;
                }else {
                    targetVnode = valueList->pmemVnodePool->getNextNode();  // get a new vnode;
                    valueList->split(vnode, targetVnode); //redisribute the keys between the two vnodes
                    if(key <= vnode->getMaxKey(&valueList->bf[vnode->hdr.id])) { // key is smaller than the max key of the previous value node after split
                        if(!vnode->lookup(key, pos, &valueList->bf[vnode->hdr.id])) {
                            vnode->insert(key, value, &valueList->bf[vnode->hdr.id]);
                            vnode->hdr.unsetBit(pos);
                        }
                    }else{
                        BloomFilter *target_bloom = &valueList->bf[targetVnode->hdr.id];
                        std::unique_lock<std::shared_mutex> lock_target(target_bloom->mtx);
                        if(!targetVnode->lookup(key, pos, target_bloom)) {
                            targetVnode->insert(key, value, target_bloom);
                            targetVnode->hdr.unsetBit(pos);
                        }
                    }
                    break;
                }
            }else {
                std::cout << "Failed to find the key in the vnode: " << vnode->hdr.id << std::endl;
                return;
            }
        }
    }
    {
        std::unique_lock<std::shared_mutex> inode_wlock(mainIndex->inode_locks[inode->getId()]);
        inode->hdr.coveredNodes++;
        if(inode->checkForActivateGP()) {
            //activate GP
            Key_t targetKey;
            int pos = -1;
            {
                BloomFilter *bloom = &valueList->bf[targetVnode->hdr.id];
                std::shared_lock<std::shared_mutex> lock_target(bloom->mtx);
                targetKey = targetVnode->getMinKey(bloom);
            }
            if(inode->activateGP(targetKey, pos)) {
                bool ret = mainIndex->linkVnodeToInode(*inode, pos, *targetVnode);
                if(!ret) {
                    std::cout << "Failed to link the vnode to the inode." << std::endl;
                    return;
                }
                inode->gps[pos].key = targetKey;
                dram_log_entry_t *entry = new dram_log_entry_t(inode->getId(),inode->hdr.coveredNodes, inode->hdr.last_index, inode->hdr.next);
                for(int i = 0; i <= inode->hdr.last_index; i++) {
                    entry->setKeyVal(i, inode->gps[i].key, inode->gps[i].value);
                }
                entry->setCoveredNodes(inode->hdr.coveredNodes);
                entry->setLastIndex(inode->hdr.last_index);
                ckptLog->enq(entry);
            }else {
                //rebalance the main index, if necessary
                needToRebalance = true;
            }
        }else {
            //no needs to activate GP, just return 
            return;
        }
    } 
    if(needToRebalance && inode){
        std::unique_lock<std::shared_mutex> rebalance_lock(mainIndex->rebalance_lock);
        bool ret = mainIndex->rebalanceInode(*inode, *targetVnode);
        return;
    }
}

void TandemIndex::scan(Key_t key, size_t range, std::priority_queue<Key_t, std::vector<Key_t>, std::greater<Key_t>> &result)
{
    int idx = -1;
    Inode *inode = mainIndex->lookup(key, idx);
    Vnode *vnode = nullptr;
    if(inode == nullptr) {
        std::cout << "Failed to find the inode for the key: " << key << std::endl;
        return;
    }
    {
        std::shared_lock<std::shared_mutex> lock(mainIndex->inode_locks[inode->getId()]);
        vnode = valueList->pmemVnodePool->at(inode->gps[idx].value);
    }
    int remaining_range = range;
    while(true) {
        BloomFilter *bloom = &valueList->bf[vnode->hdr.id];
        std::shared_lock<std::shared_mutex> lock(bloom->mtx);
        remaining_range=vnode->scan(key, remaining_range, result);
        if(remaining_range > 0 && vnode->hdr.next != -1) {
            vnode = valueList->pmemVnodePool->at(vnode->hdr.next);
        }else {
            if(remaining_range > 0) {
                std::cout << "Failed to scan key: " << key << " with range: " << range <<" remaining_range: " << remaining_range << std::endl;
            }
            return;
        }
    }
}
#if 0

void TandemIndex::remove(int key)
{
    mainIndex->remove(key);
}



void TandemIndex::print()
{
    mainIndex->print();
}
#endif