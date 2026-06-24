#pragma once
#include <string>
#include <vector>
#include <optional>
#include <random>

class MemTable {
private:
    struct Node {
        std::string key;
        std::string value;
        std::vector<Node*> forward;
        Node(std::string k, std::string v, int level) : key(k), value(v), forward(level, nullptr) {}
    };

    const int MAX_LEVEL = 16;
    const float P = 0.5;
    int current_level;
    Node* head;
    size_t current_byte_size;

    int random_level() {
        int lvl = 1;
        while (((float)rand() / RAND_MAX) < P && lvl < MAX_LEVEL) {
            lvl++;
        }
        return lvl;
    }

public:
    MemTable() : current_level(1), current_byte_size(0) {
        head = new Node("", "", MAX_LEVEL);
    }

    ~MemTable() {
        Node* curr = head;
        while (curr != nullptr) {
            Node* next = curr->forward[0];
            delete curr;
            curr = next;
        }
    }

    void put(const std::string& key, const std::string& value) {
        std::vector<Node*> update(MAX_LEVEL, nullptr);
        Node* curr = head;

        for (int i = current_level - 1; i >= 0; i--) {
            while (curr->forward[i] != nullptr && curr->forward[i]->key < key) {
                curr = curr->forward[i];
            }
            update[i] = curr;
        }

        curr = curr->forward[0];

        // If key exists, update value
        if (curr != nullptr && curr->key == key) {
            current_byte_size -= curr->value.size();
            curr->value = value;
            current_byte_size += value.size();
        } else {
            // Insert new node
            int lvl = random_level();
            if (lvl > current_level) {
                for (int i = current_level; i < lvl; i++) {
                    update[i] = head;
                }
                current_level = lvl;
            }

            Node* new_node = new Node(key, value, lvl);
            for (int i = 0; i < lvl; i++) {
                new_node->forward[i] = update[i]->forward[i];
                update[i]->forward[i] = new_node;
            }
            current_byte_size += key.size() + value.size();
        }
    }

    std::optional<std::string> get(const std::string& key) {
        Node* curr = head;
        for (int i = current_level - 1; i >= 0; i--) {
            while (curr->forward[i] != nullptr && curr->forward[i]->key < key) {
                curr = curr->forward[i];
            }
        }
        curr = curr->forward[0];
        if (curr != nullptr && curr->key == key) {
            return curr->value;
        }
        return std::nullopt;
    }

    std::vector<std::pair<std::string, std::string>> get_all_sorted() {
        std::vector<std::pair<std::string, std::string>> result;
        Node* curr = head->forward[0];
        while (curr != nullptr) {
            result.push_back({curr->key, curr->value});
            curr = curr->forward[0];
        }
        return result;
    }

    size_t byte_size() const { return current_byte_size; }
};