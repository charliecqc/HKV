#include "recoveryManager.h"

RecoveryManager::RecoveryManager(PmemInodePool *&pmemRecoveryArray) {
    this->pmemRecoveryArray = pmemRecoveryArray;
}

int RecoveryManager::recoveryOperation() {
    Inode *superNode = pmemRecoveryArray->at(MAX_NODES - 1);
    int16_t last_index = 0;
    dramInodePool = new DramInodePool(sizeof(Inode), MAX_NODES);
    if(superNode->hdr.last_index != -1) {
        last_index = superNode->hdr.last_index;
        auto pmemPool = pmemRecoveryArray->at(0);
        auto dramPool = dramInodePool->at(0);
        PmemManager::memcpyToDRAM(1, reinterpret_cast<char *>(dramPool), reinterpret_cast<char *>(pmemPool), sizeof(Inode) * (last_index + 1));
        dramInodePool->setCurrentIdx(last_index);
        return superNode->hdr.level;
    }
    return 1;
}

DramInodePool *RecoveryManager::getDramInodePool() {
    return dramInodePool;
}