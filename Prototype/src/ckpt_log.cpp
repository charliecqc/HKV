#include "ckpt_log.h"
#include "pmemManager.h"
#include <unordered_map>
#include <vector>
#include <cstring>
#define CKPLOGPOOL 3
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

    // 初始镜像（持久结构内字段都在 CkptLogNVM::init 中设为 0）
    a_consumed_start.store(ckptlog->start, std::memory_order_relaxed);
    a_durable_end.store(ckptlog->start_persistent, std::memory_order_relaxed);
    a_produced_end.store(ckptlog->end_persistent, std::memory_order_relaxed);
}

// 析构函数
CkptLog::~CkptLog() {
    delete ckptlog;
}

//get nvm_log_entry_t at inde
void CkptLog::enq(dram_log_entry_t *entry)
{
    if(entry->hdr.id == -1)
    {
        cout << "Log entry id is not set" << endl;
    }
    [[maybe_unused]] log_entry_hdr *log_entry_hdr = put_log_entry(entry);
}

log_entry_hdr *CkptLog::put_log_entry(dram_log_entry_t *entry)
{
    // 计算对齐大小
    unsigned long payload = entry->getPayLoadSize();
    unsigned long entry_size = PmemManager::align_uint_to_cacheline(payload + sizeof(log_entry_hdr));

    // 1) 短锁分配（可后续无锁化）
    log_entry_hdr *slot = nullptr;
    {
        std::unique_lock<std::shared_mutex> lk(mtx);
        slot = nvm_log_enq(entry_size);
        if (!slot) return nullptr;
    }

    // 2) 构造到临时缓冲并一次 memcpy
    char stack_buf[1024];
    char *tmp;
    std::vector<char> heap_buf;
    if (entry_size <= sizeof(stack_buf)) {
        tmp = stack_buf;
    } else {
        heap_buf.resize(entry_size);
        tmp = heap_buf.data();
    }

    auto *hdr_out = reinterpret_cast<log_entry_hdr *>(tmp);
    initLogEntryHeaderFromDramLogEntry(hdr_out, entry);

    auto *out_entries = reinterpret_cast<nvm_log_entry_t *>(tmp + sizeof(log_entry_hdr));
    for (int i = 0; i < entry->hdr.count; ++i) {
        out_entries[i].gp_idx = entry->gp_idx[i];
        out_entries[i].key    = entry->key[i];
        out_entries[i].value  = entry->value[i];
        out_entries[i].covered_nodes = entry->covered_nodes[i];
    }

    // 3) memcpy 写入映射区（不 flush）
    std::memcpy(slot, tmp, entry_size);

    // 4) 发布 produced_end（顺序保证：写数据 -> release 发布）
    a_produced_end.fetch_add(entry_size, std::memory_order_release);

    return slot;
}

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

    size_t old_end = ckptlog->end;
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
        size_t durable  = a_durable_end.load(std::memory_order_acquire);
        size_t produced = a_produced_end.load(std::memory_order_acquire);
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

        a_durable_end.store(produced, std::memory_order_release);
    }
}

// 新：使用新游标强制回放并可选重置
void CkptLog::forceReclaim(PmemInodePool *pmemInodePool)
{
    // 1. 先确保全部已持久化
    forcePersist();

    // 2. 回放所有 durable 尚未消费的日志
    for (;;) {
        size_t consumed = a_consumed_start.load(std::memory_order_acquire);
        size_t durable  = a_durable_end.load(std::memory_order_acquire);
        if (consumed >= durable) break;
        // 一次性尝试吃完剩余（内部会按条目完整度截断）
        reclaimBatch(pmemInodePool, durable - consumed);
    }

    // 3. 可选：重置（仅在程序结束时调用）
    std::unique_lock<std::shared_mutex> lk(mtx, std::try_to_lock);
    if (lk.owns_lock()) {
        ckptlog->start = ckptlog->end = ckptlog->current_update = 0;
        ckptlog->start_persistent = ckptlog->end_persistent = 0;

        a_consumed_start.store(0, std::memory_order_relaxed);
        a_durable_end.store(0, std::memory_order_relaxed);
        a_produced_end.store(0, std::memory_order_relaxed);

        PmemManager::flushNoDrain(CKPLOGPOOL, ckptlog, sizeof(*ckptlog));
        PmemManager::drain(CKPLOGPOOL);
    }
}

size_t CkptLog::reclaimBatch(PmemInodePool *pmemInodePool, size_t max_bytes)
{
    size_t start   = a_consumed_start.load(std::memory_order_acquire);
    size_t durable = a_durable_end.load(std::memory_order_acquire);
    if (start >= durable) return 0;

    size_t window = durable - start;
    size_t batch  = std::min(window, max_bytes);
    if (!batch) return 0;

    // 读取（最多两段）
    std::vector<char> buf(batch);
    size_t idx = nvm_log_index(start);
    size_t tail = ckptlog->log_size - idx;
    if (batch <= tail) {
        std::memcpy(buf.data(), nvm_log_at(start), batch);
    } else {
        std::memcpy(buf.data(), nvm_log_at(start), tail);
        std::memcpy(buf.data() + tail, nvm_log_at(0), batch - tail);
    }

    struct Upd { int32_t gp; Key_t k; Val_t v; int16_t cov; };
    std::unordered_map<int16_t, std::vector<Upd>> upds;
    size_t consumed = 0;

    while (consumed + sizeof(log_entry_hdr) <= batch) {
        auto *hdr = reinterpret_cast<log_entry_hdr *>(buf.data() + consumed);
        size_t raw = sizeof(log_entry_hdr) + (size_t)hdr->count * sizeof(nvm_log_entry_t);
        size_t esz = PmemManager::align_uint_to_cacheline(raw);
        if (esz == 0 || consumed + esz > batch) break; // 不完整，留待下批

        auto *entries = reinterpret_cast<nvm_log_entry_t *>(buf.data() + consumed + sizeof(log_entry_hdr));
        for (int i = 0; i < hdr->count; ++i) {
            upds[hdr->id].push_back({entries[i].gp_idx, entries[i].key, entries[i].value, entries[i].covered_nodes});
        }
        consumed += esz;
    }

    if (consumed == 0) return 0;

    // 应用并 flushNoDrain
    for (auto &kv : upds) {
        Inode *inode = pmemInodePool->at(kv.first);
        for (auto &u : kv.second) {
            inode->gps[u.gp].key = u.k;
            inode->gps[u.gp].value = u.v;
            inode->gps[u.gp].covered_nodes = u.cov;
        }
        PmemManager::flushNoDrain(CKPLOGPOOL, inode, sizeof(Inode));
    }
    PmemManager::drain(CKPLOGPOOL);

    // 推进 consumed_start
    size_t expected = start;
    a_consumed_start.compare_exchange_strong(expected, start + consumed, std::memory_order_acq_rel);

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
    size_t c = a_consumed_start.load(std::memory_order_acquire);
    size_t d = a_durable_end.load(std::memory_order_acquire);
    return c >= d;
}

// 返回已持久化的可消费区间长度
size_t CkptLog::getLogQueueSize()
{
    size_t c = a_consumed_start.load(std::memory_order_acquire);
    size_t d = a_durable_end.load(std::memory_order_acquire);
    return (d > c) ? (d - c) : 0;
}

bool CkptLog::flushOnce()
{
    // 防止并发 flush
    if (flush_busy.test_and_set(std::memory_order_acq_rel)) return false;

    size_t durable = a_durable_end.load(std::memory_order_acquire);
    size_t produced = a_produced_end.load(std::memory_order_acquire);
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
    a_durable_end.store(produced, std::memory_order_release);

    // 可选：镜像持久元数据（不阻塞）
    std::unique_lock<std::shared_mutex> lk(mtx, std::try_to_lock);
    if (lk.owns_lock()) {
        ckptlog->end_persistent   = produced;
        ckptlog->start_persistent = a_consumed_start.load(std::memory_order_relaxed);
    }

    flush_busy.clear(std::memory_order_release);
    return true;
}

void CkptLog::waitDurable(size_t lsn)
{
    for (;;) {
        size_t d = a_durable_end.load(std::memory_order_acquire);
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
    // 若后续扩展 header（magic / flags 等），在这里一并设置
}

