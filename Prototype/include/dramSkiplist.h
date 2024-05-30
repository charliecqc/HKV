#include <utility>
#include <iostream>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <cstring>
#include "skiplist.h"

const int MAX_LEVEL = 16;

// SkipList class
class DramSkiplist: public SkipList {
    DramSkiplist();
    ~DramSkiplist();
};
