#include "ckpt_log.h"
#include "pmemManager.h"
#define CKPLOGPOOL 3
using namespace std;

int CkptLogNVM::init(root_obj *root) {
    size_t ckp_log_space_size = 2UL * 1024UL * 1024UL * 1024UL;  //1 GiB for checkpoint log, later will reduce it to MiB 
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

//get nvm_log_entry_t at inde
void CkptLog::enq(dram_log_entry_t *entry)
{
    if(entry->hdr.id == -1)
    {
        cout << "Log entry id is not set" << endl;
    }
    [[maybe_unused]] log_entry_hdr *log_entry_hdr = put_log_entry(entry);
}

log_entry_hdr *CkptLog::log_deq()
{
    log_entry_hdr *hdr;
    if(unlikely(ckptlog->start == ckptlog->end))
    {
        return nullptr;
    }
    hdr = nvm_log_at(ckptlog->start);
    size_t obj_size = hdr->getPayLoadSize();
    size_t entry_size = obj_size + sizeof(log_entry_hdr);
    entry_size = PmemManager::align_uint_to_cacheline(entry_size);
    size_t old_start = ckptlog->start;
    ckptlog->start += entry_size;
#ifdef LOG_DEBUG
    cout << "Log deq, old_start: " << old_start << " new ckptlog->start: " << ckptlog->start << " ckptlog->end: "<< ckptlog->end << " ckptlog->current_update: "<< ckptlog->current_update << " ckptlog->start_pers: " << ckptlog->start_persistent << " ckptlog->end_perss: " << ckptlog->end_persistent<< endl;
    if(ckptlog->start > ckptlog->end) {
        cout << "Log is empty in log_deq, current start: "<<ckptlog->start <<" current end: "<< ckptlog->end << endl;
    }
#endif
    return hdr;
}

log_entry_hdr *CkptLog::put_log_entry(dram_log_entry_t *entry)
{
    nvm_log_entry_t *log_entry = nullptr;
    log_entry_hdr *persistent_start_addr = nullptr;
    log_entry_hdr *log_entry_hdr = nullptr;
    size_t start = 0;
    size_t end = 0;

    unsigned long obj_size = entry->getPayLoadSize();
    unsigned long entry_size = obj_size + sizeof(entry->hdr);
    entry_size = PmemManager::align_uint_to_cacheline(entry_size);
    {
        std::unique_lock<std::shared_mutex> lock(mtx);
        log_entry_hdr = nvm_log_enq(entry_size); // current_update is set to the log_Entry_hdr, end is set to the end of the log entry
    }
    initLogEntryHeaderFromDramLogEntry(log_entry_hdr, entry);
#ifdef LOG_DEBUG
        cout << "Log enq, log_entry_hdr->id: " << entry->hdr.id << " log_entry_hdr->count: " << entry->hdr.count << " size: " << entry_size<<endl;
#endif
    log_entry = reinterpret_cast<nvm_log_entry_t *>((char *)log_entry_hdr + sizeof(*log_entry_hdr));
    for(int i = 0; i < entry->hdr.count; i++)
    {
        log_entry->gp_idx = entry->gp_idx[i];
        log_entry->key = entry->key[i];
        log_entry->value = entry->value[i];
        // **新增：将 per-GP 覆盖数写入持久化日志**
        log_entry->covered_nodes = entry->covered_nodes[i];

        char *temp = reinterpret_cast<char *>(log_entry);
        temp += sizeof(nvm_log_entry_t);
        log_entry = reinterpret_cast<nvm_log_entry_t *>(temp);
    }
    {
        std::unique_lock<std::shared_mutex> lock(mtx);
        ckptlog->current_update += entry_size;
        ckptlog->end_persistent = ckptlog->current_update;
        end = ckptlog->end_persistent;
        start = ckptlog->start_persistent;
    }
    if(end - start > PERSISTENT_THRESHOLD)
    {
        persistent_start_addr = nvm_log_at(start);
        size_t buf_size = end - start;
        PmemManager::flushToNVM(3, reinterpret_cast<char *>(persistent_start_addr), buf_size);
        {
            std::unique_lock<std::shared_mutex> lock(mtx);
            ckptlog->start_persistent = ckptlog->end_persistent;
        }
#ifdef LOG_DEBUG
        cout << "Log is persistent, ckptlog->start_persistent: " << start << " ckptlog->end_persistent: "<< end << endl;
#endif
    }
#ifdef LOG_DEBUG
        cout << "Increase end_persistent, ckptlog->start_persistent: " << ckptlog->start_persistent << " ckptlog->end_persistent: "<< ckptlog->end_persistent << endl;
#endif
    assert(log_entry_hdr->id == entry->hdr.id);
    return log_entry_hdr;
}

log_entry_hdr *CkptLog::nvm_log_enq(size_t entry_size)
{
    log_entry_hdr *log_entry_hdr;
    if(entry_size > ckptlog->log_size)
    {
        cout << "Object size is greater than log size: " << entry_size << endl;
        return NULL;
    }
    if((ckptlog->end + entry_size) - ckptlog->start > ckptlog->log_size)
    {
        std::cout << "Log is full" << std::endl;
        return NULL;
    }
    log_entry_hdr = nvm_log_at(ckptlog->end);
    ckptlog->current_update = ckptlog->end;
    memset((unsigned char *)log_entry_hdr, 0, entry_size);
    size_t old_end = ckptlog->end;
    ckptlog->end = ckptlog->end + entry_size;
#ifdef LOG_DEBUG
    cout << "Log enq, old_end: " << old_end << " ckptlog->start: " << ckptlog->start << " ckptlog->end: "<< ckptlog->end <<" ckptlog->current_update "<< ckptlog->current_update <<" ckptlog->start_persistent " << ckptlog->start_persistent << " ckptlog->end_persis " << ckptlog->end_persistent << endl;
#endif
    return log_entry_hdr;
}

log_entry_hdr *CkptLog::log_peek_head()
{
    log_entry_hdr *nvl_entry_hdr;
    //check if log is empty
    if(unlikely(ckptlog->start == ckptlog->end))
    {
        cout << "Log is empty" << " ckptlog->start: " << ckptlog->start << " ckptlog->end: "<< ckptlog->end<< endl;
        return NULL;
    }
    if(ckptlog->start == ckptlog->current_update)
    {
#ifdef LOG_DEBUG
        cout << "Log start is under modification" << " ckptlog->start: " << ckptlog->start <<" ckptlog->current_update: "<< ckptlog->current_update << " ckptlog->end: "<< ckptlog->end <<" at line: " << __LINE__ << endl;
#endif
        return NULL;
    }

    if(ckptlog->start >= ckptlog->start_persistent)
    {
#ifdef LOG_DEBUG1
        cout << "Log start has not been persistent" << " ckptlog->start: " << ckptlog->start <<" ckptlog->current_update: "<< ckptlog->current_update << " ckptlog->end: "<< ckptlog->end <<" at line: " << __LINE__ << endl;
#endif
        return NULL;
    }
    nvl_entry_hdr = nvm_log_at(ckptlog->start);
#ifdef LOG_DEBUG
    cout << "Log peek head, ckptlog->start: " << ckptlog->start << " ckptlog->end: "<< ckptlog->end << endl << " log_entry_hdr->id: " << nvl_entry_hdr->id << " log_entry_hdr->count: " << nvl_entry_hdr->count << endl;
#endif
    return nvl_entry_hdr;
}

log_entry_hdr * CkptLog::nvm_log_at(unsigned long idx)
{
    return (log_entry_hdr *)(&ckptlog->buf[nvm_log_index(idx)]);
}

unsigned int CkptLog::nvm_log_index(unsigned long idx)
{
    return (idx & ~(ckptlog->mask));
}

void CkptLog::forcePersist()
{
    std::unique_lock<std::shared_mutex> lock(mtx);
    log_entry_hdr *persistent_start_addr = nvm_log_at(ckptlog->start_persistent);
    size_t buf_size = ckptlog->end_persistent - ckptlog->start_persistent;
    PmemManager::flushToNVM(3, reinterpret_cast<char *>(persistent_start_addr), buf_size);
    ckptlog->start_persistent = ckptlog->end_persistent;
}

void CkptLog::forceReclaim(PmemInodePool *pmemInodePool)
{
    std::shared_lock<std::shared_mutex> lock(mtx);
    unsigned long old_head_idx = ckptlog->start;
    while(true)
    {
        if(isLogEmpty()) {
            break;
        }
        log_entry_hdr *entry_hdr = log_peek_head();
        if(entry_hdr == nullptr)
        {
#ifdef LOG_DEBUG
            cout << "Log is under modification" << endl;
#endif
            continue;   
        }
        Inode *inode = pmemInodePool->at(entry_hdr->id);
        initInodeFromLogEntry(inode, entry_hdr);
        int16_t count = entry_hdr->count;   
        nvm_log_entry_t *entry = reinterpret_cast<nvm_log_entry_t *>((unsigned char *)entry_hdr + sizeof(log_entry_hdr));
        while(count > 0)
        {
            int32_t idx = entry->gp_idx;
            inode->gps[idx].key = entry->key;
            inode->gps[idx].value = entry->value;
            // **新增：从持久化日志中恢复 per-GP 覆盖数**
            inode->gps[idx].covered_nodes = entry->covered_nodes;

            char *temp = reinterpret_cast<char *>(entry);
            temp += sizeof(nvm_log_entry_t);
            entry = reinterpret_cast<nvm_log_entry_t *>(temp);
            count--;
        }
        PmemManager::flushToNVM(1, reinterpret_cast<char *>(inode), sizeof(Inode));
        log_deq();
    }
    if(old_head_idx != ckptlog->start && ckptlog->start == ckptlog->end)
    {
#ifdef LOG_DEBUG
        cout << "Reclaiming log, old_head_idx: " << old_head_idx << " ckptlog->start: " << ckptlog->start <<" ckptlog->end: " <<ckptlog->end<< endl;
#endif
        ckptlog->start_persistent = ckptlog->end_persistent = ckptlog->current_update= ckptlog->end = ckptlog->start = 0;
        PmemManager::flushToNVM(3, reinterpret_cast<char *>(ckptlog), sizeof(*ckptlog));
    }
}

void CkptLog::reclaim(PmemInodePool *pmemInodePool)
{
    try{
        size_t queue_size = 0;
        unsigned long old_head_idx = 0;
        {
            std::shared_lock<std::shared_mutex> lock(mtx);
            queue_size = getLogQueueSize();
            old_head_idx = ckptlog->start;
        }
        if(queue_size < RECLAIM_THRESHOLD && retry_count < RECLAIM_RETRY_THRESHOLD) 
        {
            retry_count++;
            return;
        }
        while(true)
        {
            log_entry_hdr *entry_hdr = nullptr;
            {
                std::shared_lock<std::shared_mutex> lock(mtx);
                if(isLogEmpty()) {
                    break;
                }
                entry_hdr = log_peek_head();
                if(entry_hdr == nullptr)
                {
#ifdef LOG_DEBUG
                    cout << "Log is under modification" << endl;
#endif
                    break;
                }
            }
            Inode *inode = pmemInodePool->at(entry_hdr->id);
            initInodeFromLogEntry(inode, entry_hdr);
            int16_t count = entry_hdr->count;
            nvm_log_entry_t *entry = reinterpret_cast<nvm_log_entry_t *>((unsigned char *)entry_hdr + sizeof(log_entry_hdr));
            while(count > 0)
            {
                int32_t idx = entry->gp_idx;
                inode->gps[idx].key = entry->key;
                inode->gps[idx].value = entry->value;
                // **新增：从持久化日志中恢复 per-GP 覆盖数**
                inode->gps[idx].covered_nodes = entry->covered_nodes;

                char *temp = reinterpret_cast<char *>(entry);
                temp += sizeof(nvm_log_entry_t);
                entry = reinterpret_cast<nvm_log_entry_t *>(temp);
                count--;
            }
            PmemManager::flushToNVM(1, reinterpret_cast<char *>(inode), sizeof(Inode));
            {
                std::unique_lock<std::shared_mutex> lock(mtx);
                log_deq();
            }
        }
        {
            std::unique_lock<std::shared_mutex> lock(mtx);
            if(old_head_idx != ckptlog->start && ckptlog->start == ckptlog->end)
            {
#ifdef LOG_DEBUG
                cout << "Reclaiming log, old_head_idx: " << old_head_idx << " ckptlog->start: " << ckptlog->start <<" ckptlog->end: " <<ckptlog->end<< endl;
#endif  
                ckptlog->start_persistent = ckptlog->end_persistent = ckptlog->current_update= ckptlog->end = ckptlog->start = 0;
                PmemManager::flushToNVM(3, reinterpret_cast<char *>(ckptlog), sizeof(*ckptlog));
            }
        }
        retry_count = 0;
    }
    catch (std::exception &e) {
        std::cout << "Exception in reclaim: " << e.what() << std::endl;
    }
}
