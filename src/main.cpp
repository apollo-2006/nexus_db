#include "../include/db.hpp"
#include <iostream>
#include <chrono>

int main() {
    std::cout << "--- Booting NexusDB Engine ---\n";
    NexusDB db("./test_db_data");

    const int NUM_OPS = 100000;

    std::cout << "Executing " << NUM_OPS << " rapid writes...\n";
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_OPS; i++) {
        std::string key = "user_" + std::to_string(i);
        std::string value = "{\"name\": \"User" + std::to_string(i) + "\", \"tier\": \"premium\"}";
        db.put(key, value);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "Writes completed in " << duration << " ms.\n";

    std::cout << "Testing Read Speed...\n";
    start = std::chrono::high_resolution_clock::now();

    // Fetch a key that we know was flushed to disk
    auto result = db.get("user_500");

    end = std::chrono::high_resolution_clock::now();
    auto read_duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    if (result) {
        std::cout << "Found user_500: " << *result << "\n";
    } else {
        std::cout << "Key not found!\n";
    }

    std::cout << "Read completed in " << read_duration << " microseconds.\n";
    std::cout << "--- Engine Shutdown ---\n";

    return 0;
}