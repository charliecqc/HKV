#include "tandemIndex.h"
#include "valuelist.h"

bool TandemIndex::insert(int key, int value)
{
    Vnode *vnode = nullptr;
    vnode = mainIndex->lookup(key);
    if(valueList->insert(key, value) == true)
        return mainIndex->insert(key, value);
    else {
        std::cout << "Failed to insert the value in the value list." << std::endl;
        return false;
    }
}

#if 0
void TandemIndex::update(int key, int value)
{
    mainIndex->update(key, value);
}

void TandemIndex::remove(int key)
{
    mainIndex->remove(key);
}

int TandemIndex::lookup(int key)
{
    return mainIndex->lookup(key);
}

void TandemIndex::print()
{
    mainIndex->print();
}
#endif