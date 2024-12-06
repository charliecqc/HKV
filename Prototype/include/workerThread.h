#include "common.h"
#pragma once

typedef struct wq_entry {
    Key_t key;
    Val_t value;
    int ops;
    void init(Key_t _key, Val_t _value, int _ops) {
        this->key = _key;
        this->value = _value;
        this->ops = _ops;
    }
} wq_entry_t;

class WorkerThread {
public:
    WorkerThread();
    ~WorkerThread();
    void workerOperation();
};

