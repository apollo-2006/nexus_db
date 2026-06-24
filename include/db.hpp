#pragma once
#include "memtable.hpp"
#include "wal.hpp"
#include <string>
#include <optional>
#include <memory>
#include <mutex>
#include <vector>

class NexusDB {
private:
    std::string db_dir;
    std::unique_ptr<MemTable> active_memtable;
    std::unique_ptr<WriteAheadLog> wal;

    std::vector<std::string> sst_files; // Tracks all flushed disk files
    std::mutex db_mutex;

    size_t memtable_limit = 1024 * 1024; // 1MB threshold for flushing
    int sst_counter = 0;

    void flush_memtable();

public:
    NexusDB(const std::string& directory);
    ~NexusDB();

    void put(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key);
    void remove(const std::string& key);
};