#include <cstdint>
#pragma once
typedef uint64_t Key_t; // 8 bytes
typedef uint64_t Val_t; // 8 bytes
const int MAX_LEVEL = 16;
#define SEARCH_STABLITY_COEFFICIENT 4
#define WORKERQUEUE_NUM 1
#define L1_CACHE_LINE_SIZE 64
#define L1_CACHE_LINE_MASK (~(L1_CACHE_LINE_SIZE - 1))

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

//#define DBG 1
