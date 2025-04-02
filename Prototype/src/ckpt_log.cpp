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
        int ret_val = pmemobj_alloc(pop, &root->ptr[0], maxSize+L1_CACHE_LINE_SIZE, 0, NULL, NULL);
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

void CkptLog::enq(Inode inode) 
{
    //cpy the modification of inode to the log entry
#if 1
    //std::unique_lock<std::mutex> lock(mtx);
    g_ckptlock.lock();
    [[maybe_unused]]log_entry_t *entry = put_log_entry(ckptlog, inode);
    g_ckptlock.unlock();
#endif

}

log_entry_t *CkptLog::log_deq()
{
    log_entry_t *entry;
    if(unlikely(ckptlog->start == ckptlog->end))
    {
        cout << "Log is empty" << endl;
        return nullptr;
    }
    entry = nvm_log_at(ckptlog, ckptlog->start);
    unsigned long entry_size = PmemManager::align_uint_to_cacheline(sizeof(entry->inode));
    ckptlog->start += entry_size;
    return entry;
}

log_entry_t * CkptLog::put_log_entry(CkptLogNVM *nvm_log, Inode inode) {
    log_entry_t *log_entry = nullptr;
    unsigned long entry_size = PmemManager::align_uint_to_cacheline(sizeof(inode));
    [[maybe_unused]]unsigned long log_index = nvm_log->end - nvm_log->start + entry_size;
    log_entry = nvm_log_enq(nvm_log, entry_size);
    PmemManager::memcpyToNVM(3, reinterpret_cast<char *>(&log_entry->inode), reinterpret_cast<char *>(&inode), sizeof(inode));
    assert(log_entry->inode.getId() == inode.getId());
    return log_entry;
}

log_entry_t *CkptLog::nvm_log_enq(CkptLogNVM *nvm_log, size_t obj_size)
{
    log_entry_t *nv_log_entry;
    unsigned int entry_size;
    entry_size = PmemManager::align_uint_to_cacheline(obj_size);
    if(entry_size > nvm_log->log_size)
    {
        cout << "Object size is greater than log size" << endl;
        return NULL;
    }
    if((nvm_log->end + entry_size) - nvm_log->start > nvm_log->log_size)
    {
        std::cout << "Log is full" << std::endl;
        return NULL;
    }
    nv_log_entry = nvm_log_at(nvm_log, nvm_log->end);
    memset((void *)nv_log_entry, 0, entry_size);
    nvm_log->end = nvm_log->end + entry_size;
    return nv_log_entry;
}

log_entry_t *CkptLog::log_peek_head(CkptLogNVM *nvm_log)
{
    log_entry_t *nvl_entry;
    //check if log is empty
    if(unlikely(nvm_log->start == nvm_log->end))
    {
        return NULL;
    }
    nvl_entry = nvm_log_at(nvm_log, nvm_log->start);
    return nvl_entry;
}

log_entry_t * CkptLog::nvm_log_at(CkptLogNVM *nvm_log, unsigned long idx)
{
    return (log_entry_t *)(&nvm_log->buf[nvm_log_index(nvm_log, idx)]);
}

unsigned int CkptLog::nvm_log_index(CkptLogNVM *nvm_log, unsigned long idx)
{
    return (idx & ~(nvm_log->mask));
}

void CkptLog::reclaim(PmemInodePool *pmemInodePool)
{
    try{
        g_ckptlock.lock();
        assert(pmemInodePool != nullptr);
        log_entry_t *entry;
        unsigned long old_head_idx = ckptlog->start;
        while((entry = log_peek_head(ckptlog)) != nullptr)
        {
            
            Inode *inode = &(entry->inode);
            int id = inode->getId();
            PmemManager::memcpyToNVM(1, reinterpret_cast<char *>(pmemInodePool->at(id)), reinterpret_cast<char *>(inode), sizeof(Inode));
            log_deq();
        }
        if(old_head_idx != ckptlog->start)
        {
            ckptlog->end = ckptlog->start = 0;
            PmemManager::flushToNVM(3, reinterpret_cast<char *>(ckptlog), sizeof(*ckptlog));
        }
        g_ckptlock.unlock();
    }
    catch (std::exception &e) {
        std::cout << "Exception in reclaim: " << e.what() << std::endl;
        g_ckptlock.unlock();
    }
}