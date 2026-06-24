#include "../include/db.hpp"
#include "../include/sstable.hpp"
#include <filesystem>
#include <iostream>

NexusDB::NexusDB(const std::string& directory) : db_dir(directory) {
    std::filesystem::create_directories(db_dir);
    active_memtable = std::make_unique<MemTable>();
    wal = std::make_unique<WriteAheadLog>(db_dir + "/active.wal");

    // In a full implementation, you would recover the WAL here on boot
}

NexusDB::~NexusDB() {
    flush_memtable(); // Safely flush RAM to disk before shutting down
}

void NexusDB::put(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(db_mutex);

    // 1. Append to Write-Ahead Log
    wal->append(key, value);

    // 2. Insert into MemTable
    active_memtable->put(key, value);

    // 3. Flush if memory limit exceeded
    if (active_memtable->byte_size() >= memtable_limit) {
        flush_memtable();
    }
}

void NexusDB::flush_memtable() {
    if (active_memtable->byte_size() == 0) return;

    std::string sst_path = db_dir + "/data_" + std::to_string(sst_counter++) + ".sst";

    // Extract sorted data and write to disk
    auto data = active_memtable->get_all_sorted();
    SSTable::write(sst_path, data);

    sst_files.push_back(sst_path);

    // Reset RAM and WAL
    active_memtable = std::make_unique<MemTable>();
    wal->clear();

    std::cout << "[NexusDB] Flushed MemTable to disk: " << sst_path << "\n";
}

std::optional<std::string> NexusDB::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(db_mutex);

    // 1. Check RAM (MemTable) first
    auto val = active_memtable->get(key);
    if (val) {
        if (*val == "@@TOMBSTONE@@") return std::nullopt; // Handle deleted keys
        return val;
    }

    // 2. Search Disk (SSTables) from newest to oldest
    for (auto it = sst_files.rbegin(); it != sst_files.rend(); ++it) {
        val = SSTable::search(*it, key);
        if (val) {
            if (*val == "@@TOMBSTONE@@") return std::nullopt;
            return val;
        }
    }

    return std::nullopt;
}

void NexusDB::remove(const std::string& key) {
    // Delete is just a write with a special Tombstone marker
    put(key, "@@TOMBSTONE@@");
}