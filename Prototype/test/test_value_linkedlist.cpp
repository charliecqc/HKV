#include "valuelist.h"
int main() {
    ValueList list;
    list.insert(1, 1);
    list.insert(2, 2);
    list.insert(3, 3);
    list.insert(4, 4);
    list.insert(5, 5);
    // Lookup nodes
    std::cout << "Lookup 10: " << (list.lookup(10) ? "Found" : "Not found") << std::endl;
    std::cout << "Lookup 20: " << (list.lookup(20) ? "Found" : "Not found") << std::endl;

     // Delete nodes
    list.remove(10);
    list.remove(20);

    return 0;

}