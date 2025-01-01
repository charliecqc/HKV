#include "common.h"
#pragma once

enum Operation {
    INSERT = 0,
    DELETE = 1,
    UPDATE = 2,
    LOOKUP = 3
};

class wq_entry{
    Key_t key;
    Val_t value;
    int ops;
    wq_entry(Key_t _key, Val_t _value, int _ops) {
        this->key = _key;
        this->value = _value;
        this->ops = _ops;
    }
};

class WorkerThread {
public:
    WorkerThread();
    ~WorkerThread();
    void workerOperation();
};

