#include <thread>
#include <vector>
#include <functional>
#include <iostream>
#include <chrono>
#include <sys/time.h>
#include "tandemIndex.h"
#pragma once
enum {
    OP_INSERT,
    OP_READ,
    OP_UPSERT,
    OP_DELETE,
    OP_SCAN,
};

void startThreads(TandemIndex *idx, int num_thread, std::function<void(int)> fn)
{
   std::vector<std::thread> thread_group;
   auto fn2 = [idx, &fn](int thread_id)
   {
       fn(thread_id);
   };
   for(int thread_id = 0; thread_id < num_thread; ++thread_id)
   {
        thread_group.push_back(std::thread{fn2, thread_id});
   }
   for(int thread_id = 0; thread_id < num_thread; ++thread_id)
   {
        thread_group[thread_id].join();
   }
}

inline double get_now()
{
  struct timeval tv;
  gettimeofday(&tv, 0);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}