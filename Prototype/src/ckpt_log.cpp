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
    cout << "Log deq, old_start: " << old_start << " new ckptlog->start: " << ckptlog->start << " ckptlog->end: "<< ckptlog->end << endl;
    if(ckptlog->start > ckptlog->end) {
        cout << "Log is empty in log_deq, current start: "<<ckptlog->start <<" current end: "<< ckptlog->end << endl;
    }
#endif
    return hdr;
}

log_entry_hdr *CkptLog::put_log_entry(dram_log_entry_t *entry) 
{
    nvm_log_entry_t *log_entry = nullptr;
    log_entry_hdr *log_entry_hdr = nullptr;
    unsigned long obj_size = entry->getPayLoadSize();
    unsigned long entry_size = obj_size + sizeof(entry->hdr);
    entry_size = PmemManager::align_uint_to_cacheline(entry_size);
    {
        std::unique_lock<std::shared_mutex> lock(mtx);
        log_entry_hdr = nvm_log_enq(entry_size);
#ifdef LOG_DEBUG
        if(entry->hdr.id == 32 && entry->hdr.count == 5 && entry_size == 128) {
        cout << "Log enq, log_entry_hdr->id: " << entry->hdr.id << " log_entry_hdr->count: " << entry->hdr.count << " size: " << entry_size<<endl;
        }
#endif
        
    log_entry_hdr->id = entry->hdr.id;
    log_entry_hdr->count = entry->hdr.count;
    log_entry_hdr->next = entry->hdr.next;
    log_entry_hdr->coveredNodes = entry->hdr.coveredNodes;
    log_entry_hdr->last_index = entry->hdr.last_index;
    log_entry = reinterpret_cast<nvm_log_entry_t *>((char *)log_entry_hdr + sizeof(*log_entry_hdr));
    for(int i = 0; i < entry->hdr.count; i++)
    {
        log_entry->gp_idx = entry->gp_idx[i];
        log_entry->key = entry->key[i];
        log_entry->value = entry->value[i];
#ifdef LOG_DEBUG
        if(log_entry->gp_idx == -1) {
            cout << "gp_idx is not set" << endl;
        }
        if(log_entry->key == static_cast<Key_t>(-1)) {
            cout << "Key is not set" << endl;
        }

        if(entry->hdr.id == 425 && entry->hdr.next == 73 && entry->hdr.coveredNodes == 10 && i == 9) {
            cout << "Log entry id is set to 168" << endl;
        }
#endif 
        char *temp = reinterpret_cast<char *>(log_entry);
        temp += sizeof(nvm_log_entry_t);
        log_entry = reinterpret_cast<nvm_log_entry_t *>(temp);
    }
    }
    
    PmemManager::flushToNVM(3, reinterpret_cast<char *>(log_entry_hdr), entry_size);
    {
        std::unique_lock<std::shared_mutex> lock(mtx);
        ckptlog->current_update += entry_size;
    }
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
    cout << "Log enq, old_end: " << old_end << " ckptlog->start: " << ckptlog->start << " ckptlog->end: "<< ckptlog->end << endl;
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

void CkptLog::reclaim(PmemInodePool *pmemInodePool)
{
    try{
        //g_ckptlock.lock();
        std::unique_lock<std::shared_mutex> lock(mtx);
        log_entry_hdr *entry_hdr = nullptr;
        unsigned long old_head_idx = ckptlog->start;
        while(!isLogEmpty())
        {
            entry_hdr = log_peek_head();
            if(entry_hdr == nullptr)
            {
#ifdef LOG_DEBUG
                cout << "Log is under modification" << endl;
#endif
                break;
            }
            int16_t id = entry_hdr->id;
            Inode *inode = pmemInodePool->at(id);
            inode->hdr.id = entry_hdr->id;
            inode->hdr.coveredNodes = entry_hdr->coveredNodes;
            inode->hdr.last_index = entry_hdr->last_index;
            inode->hdr.next = entry_hdr->next;
            int16_t count = entry_hdr->count;
            nvm_log_entry_t *entry = reinterpret_cast<nvm_log_entry_t *>((unsigned char *)entry_hdr + sizeof(log_entry_hdr));
#ifdef LOG_DEBUG
            size_t obj_size = entry_hdr->getPayLoadSize();
            if(obj_size == 153 && entry_hdr->id == 273) {
                cout << "object size: " << obj_size << " log_entry_hdr size: " << sizeof(log_entry_hdr) << endl;
            }
            if(entry_hdr->id == 168 && entry_hdr->next == 535 && entry_hdr->coveredNodes == 6) {
                cout << "Log entry id is set to 168" << endl;
            }
#endif
            while(count > 0)
            {
                if(entry_hdr->id == 425 && entry_hdr->next == 73 && entry_hdr->coveredNodes == 10) {
                    cout << "Address of key: " << &entry->key << ", Address of value: " << &entry->value << endl;
                    cout << "Alignment of key: " << alignof(entry->key) << endl;
                    cout << "Alignment of Value: " << alignof(entry->value) << endl;
                    cout << "key: " << hex<< entry->key << " Value: " << hex << entry->value << endl;
                }
                int32_t idx = entry->gp_idx;
                inode->gps[idx].key = entry->key;
                inode->gps[idx].value = entry->value;
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
            ckptlog->current_update= ckptlog->end = ckptlog->start = 0;
            PmemManager::flushToNVM(3, reinterpret_cast<char *>(ckptlog), sizeof(*ckptlog));
        }
    }
    catch (std::exception &e) {
        std::cout << "Exception in reclaim: " << e.what() << std::endl;
        //g_ckptlock.unlock();
    }
}