#include <iostream>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <cstring>

const int MAX_LEVEL = 6;

// Node class for skip list
class Node {
public:
    int key;
    Node* next;
    Node* down;

    Node(int key, Node* next = nullptr, Node* down = nullptr) {
        this->key = key;
        this->next = next;
        this->down = down;
    }
};

// SkipList class
class SkipList {
private:
    Node* head;
	Node *header[MAX_LEVEL];
    int level;

public:
    SkipList() {
        level = 1;
		head = new Node(std::numeric_limits<int>::min());
		Node *pivot = head;
		for(int i = MAX_LEVEL - 1; i >= 0; i--) {
			header[i] = pivot;
			if(i > 0) {
				Node *update = new Node(std::numeric_limits<int>::min());
				pivot->down = update;
				pivot = update;
			}
		}
    }

    ~SkipList() {
		Node* down = head;
		for (int i = MAX_LEVEL - 1; i >= 0; i--) {
			Node* current = down;
			while (current != nullptr) {
				Node* next = current->next;
				delete current;
				current = next;
			}
			down = down->down;
		}
    }

    // Generate a random level for a new node
    int randomLevel() {
        int level = 1;
        while (rand() % 2 == 0 && level < MAX_LEVEL) {
            level++;
        }
        return level;
    }

    // Insert a key into the skip list
    void insert(int key) {
        Node* current = header[level - 1];
        Node* update[MAX_LEVEL];
		memset(update, 0, sizeof(Node *)*MAX_LEVEL);

        for (int i = level - 1; i >= 0; i--) {
            while (current->next != nullptr && current->next->key < key) {
                current = current->next;
            }
            if (update[i] == nullptr) {
                update[i] = current;
            }
            current = current->down;
        }

        if (current == nullptr || current->key != key) {
            int newLevel = randomLevel();

            if (newLevel > level) {
                for(int i = level; i < newLevel; i++) {
                    update[i] = header[i];
                }
                level = newLevel;
            }

            Node* newNode = new Node(key);

            while (newLevel > 0) {
                newNode->next = update[newLevel - 1]->next;
                update[newLevel - 1] -> next = newNode;
				if(newLevel > 1) {
					Node* newDownNode = new Node(key);
					newNode->down = newDownNode;
					newNode = newDownNode;
				}
                newLevel--;
            }
        }
    }

    // Search for a key in the skip list
    bool search(int key) {
        Node* current = header[level - 1];
        for (int i = level - 1; i >= 0; i--) {
            while (current->next != nullptr && current->next->key < key) {
                current = current->next;
            }
			if(i > 0)
				current = current->down;
        }
        return (current->next != nullptr && current->next->key == key);
    }

    // Print the skip list
    void print() {
        for (int i = level - 1; i >= 0; i--) {
            std::cout << "Level " << i << ": ";
            Node* current = header[i];
            while( current != nullptr) {
				std::cout << current->key << " ";
                current = current->next;
            }
            std::cout << std::endl;
        }
    }

};

int main() {
    // Initialize random seed
    srand(time(nullptr));

    // Create a skip list
    SkipList skipList;

    // Insert keys into the skip list
    skipList.insert(3);
    skipList.insert(6);
    skipList.insert(2);
    skipList.insert(9);
    skipList.insert(5);

    // Print the skip list
    skipList.print();

    // Search for a key in the skip list
    int key = 6;
    if (skipList.search(key)) {
        std::cout << key << " is found in the skip list." << std::endl;
    } else {
        std::cout << key << " is not found in the skip list." << std::endl;
    }

    return 0;
}
