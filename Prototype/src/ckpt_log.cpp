#include "ckpt_log.h"
#include "pmemManager.h"
#include <cstring>
#include <vector>
#include <unordered_map>

#ifndef CKPLOGPOOL
#define CKPLOGPOOL 3
#endif

using namespace std;

int CkptLogNVM::init(root_obj *root) {
    size_t ckp_log_space_size = 4UL * 1024UL * 1024UL * 1024UL;  //1 GiB for checkpoint log, later will reduce it to MiB 
    bool isCreate;
    bool ret = PmemManager::createOrOpenPool(CKPLOGPOOL, fileName.c_str(), ckp_log_space_size, (void **)&root, isCreate);
    if (!ret) {
        std::cout << "Failed to create or open pool: " << fileName << std::endl;
        return -1;
    } 

    // To allocate the checkpoint log. 1. allocate memory. 2. cast into ckp_entry 3. pot them into vector.
    PMEMobjpool *pop = (PMEMobjpool *)PmemManager::getPoolStartAddress(CKPLOGPOOL);
    if(isCreate) {
        int ret_val = pmemobj_zalloc(pop, &root->ptr[0], maxSize+L1_CACHE_LINE_SIZE, 0);
        if (ret_val) {
            std::cout << "Failed to allocate memory for root->ptr[0]" << std::endl;
            return -1;
        }
        this->_buf= static_cast<unsigned char *> (pmemobj_direct(root->ptr[0]));
        this->buf = PmemManager::align_ptr_to_cacheline((void *)this->_buf);
        PmemManager::flushToNVM(CKPLOGPOOL, (char *)(this->buf), maxSize+L1_CACHE_LINE_SIZE);
        return 0;
    }else {
        this->_buf= static_cast<unsigned char *> (pmemobj_direct(root->ptr[0]));
        this->buf = PmemManager::align_ptr_to_cacheline((void *)this->_buf);
        return 0;
    }
}

// 构造函数
CkptLog::CkptLog(size_t logSize)
    : retry_count(0),
      ckptlog(new CkptLogNVM(logSize)) {

    a_consumed_start.v.store(ckptlog->start, std::memory_order_relaxed);
    a_durable_end.v.store(ckptlog->start_persistent, std::memory_order_relaxed);
    a_produced_end.v.store(ckptlog->end_persistent, std::memory_order_relaxed);
}

// 析构函数
CkptLog::~CkptLog() {
    delete ckptlog;
}

//get nvm_log_entry_t at inde
void CkptLog::enq(dram_log_entry_t *entry)
{
    if (!entry) return;

    // 计算条目长度：2字节类型标记 + 头 + 负载，对齐到 cacheline
    const size_t tag_sz   = sizeof(uint16_t); // WalLogType
    const size_t hdr_sz   = sizeof(log_entry_hdr);
    const size_t body_sz  = entry->getPayLoadSize(); // = sizeof(nvm_log_entry_t) * count
    const size_t used     = tag_sz + hdr_sz + body_sz;
    const size_t entry_sz = PmemManager::align_uint_to_cacheline(static_cast<unsigned int>(used));

    log_entry_hdr *slot = nullptr;
    {
        std::unique_lock<std::shared_mutex> lk(mtx);
        slot = nvm_log_enq(entry_sz);
        if (!slot) {
            throw std::runtime_error("ckpt log full");
        }
    }

    // 起始地址（可按字节写入）
    auto *base = reinterpret_cast<unsigned char *>(slot);

    // 1) 写类型标记：FULL
    *reinterpret_cast<uint16_t *>(base) = WAL_LOG_TYPE_FULL;

    // 2) 写 header（位于标记之后）
    auto *hdr = reinterpret_cast<log_entry_hdr *>(base + tag_sz);
    initLogEntryHeaderFromDramLogEntry(hdr, entry); // 复用现有函数初始化头部（包含 count 等）

    // 3) 写 payload（nvm_log_entry_t[count]）
    auto *out = reinterpret_cast<nvm_log_entry_t *>(reinterpret_cast<unsigned char *>(hdr) + sizeof(log_entry_hdr));
    for (int i = 0; i < entry->hdr.count; ++i) {
        out[i].gp_idx        = entry->gp_idx[i];
        out[i].key           = entry->key[i];
        out[i].value         = entry->value[i];
        out[i].covered_nodes = entry->covered_nodes[i];
    }

    // 4) 尾部对齐填充
    if (entry_sz > used) {
        std::memset(base + used, 0, entry_sz - used);
    }
    
    // 5) 推进生产游标
    a_produced_end.v.fetch_add(entry_sz, std::memory_order_release);
}

#ifndef ENABLE_DELTA_LOG
#define ENABLE_DELTA_LOG 1
#endif

#if ENABLE_DELTA_LOG
void CkptLog::enqDelta(int32_t inode_id,
                       int32_t last_index,
                       int32_t next,
                       int32_t parent_id,
                       const WalDeltaEntry *entries,
                       size_t entry_count)
{
    if (!appendDeltaLog(inode_id, last_index, next, parent_id, entries, entry_count)) {
        throw std::runtime_error("ckpt log full (delta)");
    }
}

bool CkptLog::appendDeltaLog(int32_t inode_id,
                             int32_t last_index,
                             int32_t next,
                             int32_t parent_id,
                             const WalDeltaEntry *entries,
                             size_t entry_count)
{
    if (!entries || entry_count == 0) return true;

    size_t payload_bytes = sizeof(WalDeltaHeader) + entry_count * sizeof(WalDeltaEntry);
    size_t entry_size = PmemManager::align_uint_to_cacheline((unsigned)payload_bytes);

    log_entry_hdr *slot = nullptr;
    {
        std::unique_lock<std::shared_mutex> lk(mtx);
        slot = nvm_log_enq(entry_size);
        if (!slot) return false;
    }

    auto *hdr = reinterpret_cast<WalDeltaHeader *>(slot);
    hdr->type       = WAL_LOG_TYPE_DELTA;
    hdr->count      = (uint16_t)entry_count;
    hdr->inode_id   = inode_id;
    hdr->last_index = last_index;
    hdr->next       = next;
    hdr->parent_id  = parent_id;

    auto *delta_entries = reinterpret_cast<WalDeltaEntry *>(hdr + 1);
    std::memcpy(delta_entries, entries, entry_count * sizeof(WalDeltaEntry));

    size_t used = sizeof(WalDeltaHeader) + entry_count * sizeof(WalDeltaEntry);
    if (entry_size > used) {
        std::memset(reinterpret_cast<char *>(hdr) + used, 0, entry_size - used);
    }
    
    a_produced_end.v.fetch_add(entry_size, std::memory_order_release);
    
    return true;
}

void CkptLog::applyDeltaEntries(Inode *inode,
                                const WalDeltaEntry *entries,
                                size_t entry_count,
                                int32_t new_last_index,
                                int32_t new_next,
                                int32_t new_parent_id)
{
    if (!inode || !entries) return;
    for (size_t i = 0; i < entry_count; ++i) {
        int16_t s = entries[i].slot;
        if (s < 0 || s >= fanout) continue;
        auto &gp = inode->gps[s];
        gp.key = entries[i].key;
        gp.value = entries[i].value;
        gp.covered_nodes = entries[i].covered;
        PmemManager::flushNoDrain(CKPLOGPOOL, &gp, sizeof(gp));
    }
    if (new_last_index != WAL_META_KEEP) {
        inode->hdr.last_index = new_last_index;
        PmemManager::flushNoDrain(CKPLOGPOOL, &inode->hdr.last_index, sizeof(inode->hdr.last_index));
    }
    if (new_next != WAL_META_KEEP) {
        inode->hdr.next = new_next;
        PmemManager::flushNoDrain(CKPLOGPOOL, &inode->hdr.next, sizeof(inode->hdr.next));
    }
    if (new_parent_id != WAL_META_KEEP) {
        inode->hdr.parent_id = new_parent_id;
        PmemManager::flushNoDrain(CKPLOGPOOL, &inode->hdr.parent_id, sizeof(inode->hdr.parent_id));
    }
}
#endif // ENABLE_DELTA_LOG

log_entry_hdr *CkptLog::nvm_log_enq(size_t entry_size)
{
    log_entry_hdr *log_entry_hdr;
    if (entry_size > ckptlog->log_size) {
        cout << "Object size is greater than log size: " << entry_size << endl;
        return NULL;
    }
    if ((ckptlog->end + entry_size) - ckptlog->start > ckptlog->log_size) {
        std::cout << "Log is full" << std::endl;
        return NULL;
    }
    log_entry_hdr = nvm_log_at(ckptlog->end);
    ckptlog->current_update = ckptlog->end;
#ifdef LOG_DEBUG
    size_t old_end = ckptlog->end;
#endif
    ckptlog->end = ckptlog->end + entry_size;
#ifdef LOG_DEBUG
    cout << "Log enq, old_end: " << old_end << " ckptlog->start: " << ckptlog->start
         << " ckptlog->end: "<< ckptlog->end <<" ckptlog->current_update "<< ckptlog->current_update
         <<" ckptlog->start_persistent " << ckptlog->start_persistent
         << " ckptlog->end_persis " << ckptlog->end_persistent << endl;
#endif
    return log_entry_hdr;
}

// 新：强制把尚未持久化的 [a_durable_end, a_produced_end) 全部刷到 NVM（忽略阈值）
void CkptLog::forcePersist()
{
    for (;;) {
        size_t durable  = a_durable_end.v.load(std::memory_order_acquire);
        size_t produced = a_produced_end.v.load(std::memory_order_acquire);
        if (produced <= durable) break;
        size_t len = produced - durable;

        // 处理环形两段
        size_t off       = nvm_log_index(durable);
        size_t tail_left = ckptlog->log_size - off;
        if (len <= tail_left) {
            PmemManager::flushNoDrain(CKPLOGPOOL, nvm_log_at(durable), len);
        } else {
            PmemManager::flushNoDrain(CKPLOGPOOL, nvm_log_at(durable), tail_left);
            PmemManager::flushNoDrain(CKPLOGPOOL, nvm_log_at(0), len - tail_left);
        }
        PmemManager::drain(CKPLOGPOOL);

        // 持久化元数据（可恢复 durable 边界）
        ckptlog->end_persistent = produced;
        PmemManager::flushNoDrain(CKPLOGPOOL, &ckptlog->end_persistent, sizeof(ckptlog->end_persistent));
        PmemManager::drain(CKPLOGPOOL);

        a_durable_end.v.store(produced, std::memory_order_release);
    }
}

// 新：使用新游标强制回放并可选重置
void CkptLog::forceReclaim(PmemInodePool *pmemInodePool)
{
    forcePersist();

    for (;;) {
        size_t consumed   = a_consumed_start.v.load(std::memory_order_acquire);
        size_t durable = a_durable_end.v.load(std::memory_order_acquire);
        if (consumed >= durable) break;

        // 一次性尝试吃完剩余（内部会按条目完整度截断）
        reclaimBatch(pmemInodePool, durable - consumed);
    }
    std::unique_lock<std::shared_mutex> lk(mtx, std::try_to_lock);
    if (lk.owns_lock()) {
        ckptlog->start = ckptlog->end = ckptlog->current_update = 0;
        ckptlog->start_persistent = ckptlog->end_persistent = 0;

        a_consumed_start.v.store(0, std::memory_order_relaxed);
        a_durable_end.v.store(0, std::memory_order_relaxed);
        a_produced_end.v.store(0, std::memory_order_relaxed);

        PmemManager::flushNoDrain(CKPLOGPOOL, ckptlog, sizeof(*ckptlog));
        PmemManager::drain(CKPLOGPOOL);
    }
}

size_t CkptLog::reclaimBatch(PmemInodePool *pmemInodePool, size_t max_bytes)
{
    size_t start   = a_consumed_start.v.load(std::memory_order_acquire);
    size_t durable = a_durable_end.v.load(std::memory_order_acquire);
    if (start >= durable) return 0;

    size_t window = durable - start;
    size_t batch  = std::min(window, max_bytes);
    if (!batch) return 0;

    std::vector<unsigned char> buf(batch);
    size_t idx  = nvm_log_index(start);
    size_t tail = ckptlog->log_size - idx;
    if (batch <= tail) {
        std::memcpy(buf.data(), nvm_log_at(start), batch);
    } else {
        std::memcpy(buf.data(), nvm_log_at(start), tail);
        std::memcpy(buf.data() + tail, nvm_log_at(0), batch - tail);
    }

    size_t consumed = 0;

    while (consumed + sizeof(uint16_t) <= batch) {
        auto *base = buf.data() + consumed;
        uint16_t tag = *reinterpret_cast<uint16_t *>(base);

        // 解析 DELTA
#if ENABLE_DELTA_LOG
        if (tag == WAL_LOG_TYPE_DELTA) {
            if (consumed + sizeof(WalDeltaHeader) > batch) break;
            auto *dh = reinterpret_cast<WalDeltaHeader *>(base);
            if (dh->count == 0 || dh->count > fanout) break;

            size_t raw_sz   = sizeof(WalDeltaHeader) + dh->count * sizeof(WalDeltaEntry);
            size_t entry_sz = PmemManager::align_uint_to_cacheline(static_cast<unsigned int>(raw_sz));
            if (entry_sz == 0 || consumed + entry_sz > batch) break;

            auto *entries = reinterpret_cast<WalDeltaEntry *>(dh + 1);
            Inode *inode = pmemInodePool->at(dh->inode_id);
            applyDeltaEntries(inode, entries, dh->count, dh->last_index, dh->next, dh->parent_id);

            consumed += entry_sz;
            continue;
        }
#endif

        // 解析 FULL
        if (tag == WAL_LOG_TYPE_FULL) {
            // header 紧随 tag
            if (consumed + sizeof(uint16_t) + sizeof(log_entry_hdr) > batch) break;
            auto *fh = reinterpret_cast<log_entry_hdr *>(base + sizeof(uint16_t));
            if (fh->count < 0 || fh->count > fanout) break;

            size_t raw_sz   = sizeof(uint16_t) + sizeof(log_entry_hdr)
                            + static_cast<size_t>(fh->count) * sizeof(nvm_log_entry_t);
            size_t entry_sz = PmemManager::align_uint_to_cacheline(static_cast<unsigned int>(raw_sz));
            if (entry_sz == 0 || consumed + entry_sz > batch) break;

            auto *entries = reinterpret_cast<nvm_log_entry_t *>(
                reinterpret_cast<unsigned char *>(fh) + sizeof(log_entry_hdr));

            // 应用到 pmemInode
            Inode *inode = pmemInodePool->at(fh->id);
            inode->hdr.last_index = fh->last_index;
            inode->hdr.next       = fh->next;
            inode->hdr.level      = fh->level;   // 新增：回放 FULL 时同步 level
            inode->hdr.parent_id  = fh->parent_id; // 新增：回放 FULL 时同步 parent_id

            for (int i = 0; i < fh->count; ++i) {
                int gi = entries[i].gp_idx;
                inode->gps[gi].key           = entries[i].key;
                inode->gps[gi].value         = entries[i].value;
                inode->gps[gi].covered_nodes = entries[i].covered_nodes;
            }

            // flush（可按需改为更细粒度）
            PmemManager::flushNoDrain(CKPLOGPOOL, inode, sizeof(Inode));

            consumed += entry_sz;
            continue;
        }

        // 非法/未知类型，停止本批（避免越界）
        break;
    }

    if (consumed == 0) return 0;

    // drain 一次，提交之前的 flushNoDrain
    PmemManager::drain(CKPLOGPOOL);

    // 推进消费游标
    a_consumed_start.v.fetch_add(consumed, std::memory_order_acq_rel);
    return consumed;
}

// 修改：reclaim 使用批处理，按阈值驱动
void CkptLog::reclaim(PmemInodePool *pmemInodePool)
{
    try {
        size_t q = getLogQueueSize();
        if (q < RECLAIM_THRESHOLD && retry_count < RECLAIM_RETRY_THRESHOLD) {
            retry_count++;
            return;
        }
        retry_count = 0;

        // 每批按阈值大小处理，直到该次没有更多完整条目
        const size_t BATCH_BYTES = std::max<size_t>(PERSISTENT_THRESHOLD, RECLAIM_THRESHOLD);
        while (reclaimBatch(pmemInodePool, BATCH_BYTES) > 0) { /* loop */ }
    } catch (std::exception &e) {
        std::cout << "Exception in reclaim: " << e.what() << std::endl;
    }
}

// 统一环形索引：要求 mask = log_size - 1；否则回退到 %
unsigned int CkptLog::nvm_log_index(unsigned long idx)
{
    const size_t log_size = ckptlog->log_size;
    const size_t mask     = ckptlog->mask;

    if (log_size && (log_size & (log_size - 1)) == 0 && mask == (log_size - 1)) {
        return static_cast<unsigned int>(idx & mask);
    }
    if (log_size) {
        return static_cast<unsigned int>(idx % log_size);
    }
    // fallback（未初始化时避免崩溃）
    return static_cast<unsigned int>(idx);
}

// 通过环形索引定位 NVM 中的条目头地址
log_entry_hdr *CkptLog::nvm_log_at(size_t index)
{
    const unsigned int off = nvm_log_index(index);
    auto *base = const_cast<unsigned char *>(ckptlog->buf); // buf 为持久化起始
    return reinterpret_cast<log_entry_hdr *>(base + off);
}

// 基于“已持久化边界”的无锁判空
bool CkptLog::isLogEmpty()
{
    size_t c = a_consumed_start.v.load(std::memory_order_acquire);
    size_t d = a_durable_end.v.load(std::memory_order_acquire);
    return c >= d;
}

// 返回已持久化的可消费区间长度
size_t CkptLog::getLogQueueSize()
{
    size_t c = a_consumed_start.v.load(std::memory_order_acquire);
    size_t d = a_durable_end.v.load(std::memory_order_acquire);
    return (d > c) ? (d - c) : 0;
}

bool CkptLog::flushOnce()
{
    // 防止并发 flush
    if (flush_busy.test_and_set(std::memory_order_acq_rel)) return false;

    size_t durable  = a_durable_end.v.load(std::memory_order_acquire);
    size_t produced = a_produced_end.v.load(std::memory_order_acquire);
    if (produced <= durable) {
        flush_busy.clear(std::memory_order_release);
        return false;
    }

    size_t len = produced - durable;

    // 阈值门控（可调）：不足 PERSISTENT_THRESHOLD 可暂缓
    if (len < PERSISTENT_THRESHOLD) {
        flush_busy.clear(std::memory_order_release);
        return false;
    }

    // 处理环形两段 flush
    size_t off = nvm_log_index(durable);
    size_t tail_bytes = ckptlog->log_size - off;
    if (len <= tail_bytes) {
        PmemManager::flushNoDrain(CKPLOGPOOL, nvm_log_at(durable), len);
    } else {
        PmemManager::flushNoDrain(CKPLOGPOOL, nvm_log_at(durable), tail_bytes);
        PmemManager::flushNoDrain(CKPLOGPOOL, nvm_log_at(0), len - tail_bytes);
    }
    // 单次 drain
    PmemManager::drain(CKPLOGPOOL);

    // 推进 durable_end（release，让回放线程看到完整持久内容）
    a_durable_end.v.store(produced, std::memory_order_release);

    // 可选：镜像持久元数据（不阻塞）
    std::unique_lock<std::shared_mutex> lk(mtx, std::try_to_lock);
    if (lk.owns_lock()) {
        ckptlog->end_persistent   = produced;
        ckptlog->start_persistent = a_consumed_start.v.load(std::memory_order_relaxed);
    }

    flush_busy.clear(std::memory_order_release);
    return true;
}

void CkptLog::waitDurable(size_t lsn)
{
    for (;;) {
        size_t d = a_durable_end.v.load(std::memory_order_acquire);
        if (d >= lsn) break;
        // 轻量自旋或 sleep/yield
        std::this_thread::yield();
    }
}

// 成员函数实现：从 DRAM 日志条目头复制必要字段
void CkptLog::initLogEntryHeaderFromDramLogEntry(log_entry_hdr *dst,
                                                 const dram_log_entry_t *src)
{
    // 基本防御
    if (!dst || !src) return;
    // 复制头字段
    dst->id         = src->hdr.id;
    dst->count      = src->hdr.count;
    dst->last_index = src->hdr.last_index;
    dst->next       = src->hdr.next;
    dst->level      = src->hdr.level;
    dst->parent_id  = src->hdr.parent_id;
    assert(dst->id >=0);
}

size_t CkptLog::getDurableGap() const
{
    size_t c = a_consumed_start.v.load(std::memory_order_acquire);
    size_t d = a_durable_end.v.load(std::memory_order_acquire);
    return d > c ? (d - c) : 0;
}

size_t CkptLog::getProducedGap() const
{
    size_t d = a_durable_end.v.load(std::memory_order_acquire);
    size_t p = a_produced_end.v.load(std::memory_order_acquire);
    return p > d ? (p - d) : 0;
}

size_t CkptLog::getBacklogGap() const
{
    size_t c = a_consumed_start.v.load(std::memory_order_acquire);
    size_t p = a_produced_end.v.load(std::memory_order_acquire);
    return p > c ? (p - c) : 0;
}

bool CkptLog::tryFlushOnce()
{
    // 封装内部 flushOnce()；按内部阈值决定是否执行
    return flushOnce();
}

size_t CkptLog::suggestReclaimBatchBytes() const
{
#ifdef PERSISTENT_THRESHOLD
    size_t a = PERSISTENT_THRESHOLD;
#else
    size_t a = 64 * 1024;
#endif
#ifdef RECLAIM_THRESHOLD
    size_t b = RECLAIM_THRESHOLD;
#else
    size_t b = 64 * 1024;
#endif
    return a > b ? a : b;
}

