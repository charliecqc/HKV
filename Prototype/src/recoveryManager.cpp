#include "recoveryManager.h"
#include <iostream>

RecoveryManager::RecoveryManager(PmemInodePool *&pmemRecoveryArray) {
    this->pmemRecoveryArray = pmemRecoveryArray;
}

int RecoveryManager::recoveryOperation() {
    Inode *superNode = pmemRecoveryArray->at(MAX_NODES - 1);
    int32_t last_index = 0;
    dramInodePool = new DramInodePool(sizeof(Inode), MAX_NODES);
    std::cout << "[Recovery] superNode->hdr.next=" << superNode->hdr.next
              << ", superNode->hdr.level=" << superNode->hdr.level << std::endl;
    if(superNode->hdr.next != 0) {
        last_index = superNode->hdr.next;
        auto pmemPool = pmemRecoveryArray->at(0);
        auto dramPool = dramInodePool->at(0);
        PmemManager::memcpyToDRAM(1, reinterpret_cast<char *>(dramPool), reinterpret_cast<char *>(pmemPool), sizeof(Inode) * (last_index ));
        dramInodePool->setCurrentIdx(last_index);

        // Fix seqlock versions: if an inode was mid-write at crash time,
        // its version is odd (write-locked). Readers spin forever on odd
        // versions, so bump them to the next even value.
        int fixed = 0;
        for (int32_t i = 0; i < last_index; ++i) {
            Inode *node = dramInodePool->at(i);
            uint64_t v = node->version.load(std::memory_order_relaxed);
            if (v & 1) {
                node->version.store(v + 1, std::memory_order_relaxed);
                ++fixed;
            }
        }

        std::cout << "[Recovery] Copied " << last_index << " inodes from PMem to DRAM"
                  << " (fixed " << fixed << " odd seqlock versions)" << std::endl;
        return superNode->hdr.level;
    }
    std::cout << "[Recovery] No inodes found (superNode->hdr.next == 0)" << std::endl;
    return 1;
}

DramInodePool *RecoveryManager::getDramInodePool() {
    return dramInodePool;
}