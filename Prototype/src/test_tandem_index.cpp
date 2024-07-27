#include "tandemIndex.h"
int main() {
    TandemIndex list;
    list.insert(1, 1);
    list.insert(2, 2);
    list.insert(3, 3);
    list.insert(4, 4);
    list.insert(5, 5);
    // Lookup nodes
	std::cout << "Lookup 5: " << list.lookup(5)  << std::endl;
	std::cout << "Lookup 3: " << list.lookup(3)  << std::endl;
	std::cout << "Lookup 6: " << list.lookup(6)  << std::endl;
	std::cout << "Lookup 1: " << list.lookup(1)  << std::endl;
    std::cout << "Lookup 10: " << list.lookup(10) << std::endl;

     // Delete nodes
    //list.remove(10);
    //list.remove(20);

    return 0;

}
