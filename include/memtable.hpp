#pragma once
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "record.hpp"

// The write buffer: a skip list holding the newest version of every key written
// since the last flush, in key order so a flush is a sequential write.
//
// A skip list is used rather than a balanced tree because insertion needs no
// rebalancing, which keeps the write path free of whole-structure rewrites.
class MemTable {
private:
    struct Node {
        std::string key;
        std::string value;
        bool tombstone;
        std::vector<Node*> forward;
        Node(std::string k, std::string v, bool dead, int level)
            : key(std::move(k)), value(std::move(v)), tombstone(dead), forward(level, nullptr) {}
    };

    static constexpr int MAX_LEVEL = 16;
    static constexpr double P = 0.5;
    int current_level;
    Node* head;
    size_t current_byte_size;
    size_t entry_count = 0;
    std::mt19937 rng{std::random_device{}()};
    std::bernoulli_distribution coin{P};

    int random_level() {
        int lvl = 1;
        while (coin(rng) && lvl < MAX_LEVEL) lvl++;
        return lvl;
    }

public:
    // What a lookup finds: the value and whether the write was a deletion.
    struct Entry {
        std::string value;
        bool tombstone = false;
    };

    MemTable() : current_level(1), current_byte_size(0) { head = new Node("", "", false, MAX_LEVEL); }

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    ~MemTable() {
        Node* curr = head;
        while (curr != nullptr) {
            Node* next = curr->forward[0];
            delete curr;
            curr = next;
        }
    }

    void put(const std::string& key, const std::string& value, bool tombstone = false) {
        std::vector<Node*> update(MAX_LEVEL, nullptr);
        Node* curr = head;

        for (int i = current_level - 1; i >= 0; i--) {
            while (curr->forward[i] != nullptr && curr->forward[i]->key < key) curr = curr->forward[i];
            update[i] = curr;
        }

        curr = curr->forward[0];

        if (curr != nullptr && curr->key == key) {
            current_byte_size -= curr->value.size();
            curr->value = value;
            curr->tombstone = tombstone;
            current_byte_size += value.size();
            return;
        }

        int lvl = random_level();
        if (lvl > current_level) {
            for (int i = current_level; i < lvl; i++) update[i] = head;
            current_level = lvl;
        }

        Node* new_node = new Node(key, value, tombstone, lvl);
        for (int i = 0; i < lvl; i++) {
            new_node->forward[i] = update[i]->forward[i];
            update[i]->forward[i] = new_node;
        }
        current_byte_size += key.size() + value.size();
        entry_count++;
    }

    std::optional<Entry> get(const std::string& key) const {
        Node* curr = head;
        for (int i = current_level - 1; i >= 0; i--) {
            while (curr->forward[i] != nullptr && curr->forward[i]->key < key) curr = curr->forward[i];
        }
        curr = curr->forward[0];
        if (curr != nullptr && curr->key == key) return Entry{curr->value, curr->tombstone};
        return std::nullopt;
    }

    // Every entry in key order, which is the form a flush writes.
    std::vector<Record> entries() const {
        std::vector<Record> result;
        result.reserve(entry_count);
        for (Node* curr = head->forward[0]; curr != nullptr; curr = curr->forward[0]) {
            result.push_back(Record{curr->key, curr->value, curr->tombstone});
        }
        return result;
    }

    size_t byte_size() const { return current_byte_size; }
    size_t size() const { return entry_count; }
};
