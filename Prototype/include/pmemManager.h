#include <libpmemobj.h>
#include <string>
#include <iostream>
#include <unistd.h>
#pragma once
using namespace std;

typedef struct root_obj {
    PMEMoid ptr[2];
    //    PMEMoid ptr2;
} root_obj;

class PmemManager {
    private:
        static void *pmemPool[6]; // dram
    public:
        static void *getPoolStartAddress(int poolId) {
            return pmemPool[poolId];
        }

        static bool createOrOpenPool(int poolId, string path, size_t poolSize, void **rootp, bool &isCreate) {
            PMEMobjpool *pop = nullptr;
            if(access(path.c_str(), F_OK) != 0) {
                std::cout << "File does not exists: " << path << std::endl;
                pop = pmemobj_create(path.c_str(), "pmemvaluepool", poolSize, 0666);
                if (pop == nullptr) {
                    std::cout << "Failed to create pool: " << path << ", error code: " << errno << std::endl;
                    return false;
                }
                isCreate = true;
                std::cout << "Created pool: " << path << std::endl;                
            } else {
                std::cout << "File exist: " << path << std::endl;
                pop = pmemobj_open(path.c_str(), "pmemvaluepool");
                if(pop != NULL) {
                    std::cout << "Opened pool: " << path << std::endl;
                } else {
                    std::cout << "Failed to open pool: " << path << std::endl;
                    return false;
                }
                isCreate = false;
            }
            pmemPool[poolId] = reinterpret_cast<void *>(pop);
            PMEMoid root = pmemobj_root(pop, sizeof(root_obj));
            *rootp = (root_obj*)pmemobj_direct(root);
            return true;
        }

        static bool closePool(int poolId) {
            PMEMobjpool *pop = (PMEMobjpool *)pmemPool[poolId];
            pmemobj_close(pop);
            return true;
        }

        static void *alloc(int poolId, size_t size) {
            PMEMobjpool *pop = (PMEMobjpool *)pmemPool[poolId];
            PMEMoid oid;
            int ret = pmemobj_alloc(pop, &oid, size, 0, NULL, NULL);
            if (ret) {
                return nullptr;
            }
            return pmemobj_direct(oid);
        }

        static inline void flushToNVM(int poolId, char *data, size_t size) {
            PMEMobjpool *pop = (PMEMobjpool *)pmemPool[poolId];
            pmemobj_persist(pop, data, size);
        }

        static inline void memcpyToNVM(int poolId, char *dest, char *src, size_t size) {
            PMEMobjpool *pop = (PMEMobjpool *)pmemPool[poolId];
            pmemobj_memcpy_persist(pop, dest, src, size);
        }

        static inline void memcpyToDRAM(int poolId, char *dest, char *src, size_t size) {
            PMEMobjpool *pop = (PMEMobjpool *)pmemPool[poolId];
            memcpy(dest, src, size);
        }

};