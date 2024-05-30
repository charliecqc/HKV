#include <iostream>
#include <vector>

class MemoryPool {
private:
    std::vector<char*> memoryBlocks;
    size_t blockSize;
    size_t numBlocks;

public:
    MemoryPool(size_t blockSize, size_t numBlocks) : blockSize(blockSize), numBlocks(numBlocks) {
        // Allocate memory blocks
        for (size_t i = 0; i < numBlocks; ++i) {
            char* block = new char[blockSize];
            memoryBlocks.push_back(block);
        }
    }

    ~MemoryPool() {
        // Deallocate memory blocks
        for (char* block : memoryBlocks) {
            delete[] block;
        }
    }

    void* allocate() {
        // Find a free block and return its address
        for (char* block : memoryBlocks) {
            // Assuming a block is free if its first byte is zero
            if (block[0] == 0) {
                return block;
            }
        }
        return nullptr; // No free blocks available
    }

    void deallocate(void* block) {
        // Mark the block as free by setting its first byte to zero
        char* memBlock = static_cast<char*>(block);
        memBlock[0] = 0;
    }
};

#if 0
int main() {
    // Create a memory pool with block size of 1024 bytes and 10 blocks
    MemoryPool pool(1024, 10);

    // Allocate a block from the memory pool
    void* block = pool.allocate();
    if (block) {
        std::cout << "Block allocated at address: " << block << std::endl;

        // Do something with the allocated block...

        // Deallocate the block
        pool.deallocate(block);
        std::cout << "Block deallocated." << std::endl;
    } else {
        std::cout << "No free blocks available." << std::endl;
    }

    return 0;
}
#endif
