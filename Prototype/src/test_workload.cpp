#include <cassert>
#include <cstdlib>
#include <ctime>
#include "util.h"
#include "tandemIndex.h"

void exec(int num_thread) {
    TandemIndex idx;
    double start_time = 0;
    double end_time = 0;
    double elapsed_time = 0;
    int num_insert = 10000;
    auto func = [&idx, num_insert](int thread_id) {
        int start_idx = thread_id * num_insert;
        int end_idx = start_idx + num_insert;
        for(int i = start_idx; i < end_idx; i++) {
            Val_t random_value = std::rand() % 1000 + 1;
            idx.insert(random_value, random_value);
            auto ret = idx.lookup(random_value);
            if(ret != random_value) {
                std::cout << "Failed to insert " << random_value <<" got " << ret << std::endl;
                assert(false);
            }
        }    
    };
    start_time = get_now();
    startThreads(&idx, num_thread, func);
    end_time = get_now();
    elapsed_time = end_time - start_time;
    std::cout << "Elapsed time: " << elapsed_time << std::endl;
    return;
}

int main() {
    int num_thread = 2;
    exec(num_thread);
    // Lookup nodes

    //list.remove(10);
    //list.remove(20);

    return 0;

}


