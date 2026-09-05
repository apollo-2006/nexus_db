#include "../include/db.hpp"
#include "../include/sstable.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>

NexusDB::NexusDB(const std::string& directory) : db_dir(directory) {
    std::filesystem::create_directories(db_dir);
    active_memtable = std::make_unique<MemTable>();
    wal = std::make_unique<WriteAheadLog>(db_dir + "/active.wal");

    // Re-adopt the SSTables already on disk.
    //
    // Without this, reopening a database started from an empty sst_files list:
    // every previously flushed key read back as "not found", and because
    // sst_counter also restarted at 0, the next flush wrote data_0.sst straight
    // over the existing one. Opening an existing database therefore destroyed it.
    std::vector<std::pair<int, std::string>> found;
    for (const auto& entry : std::filesystem::directory_iterator(db_dir)) {
        if (!entry.is_regular_file()) continue;

        const std::string name = entry.path().filename().string();
        if (name.rfind("data_", 0) != 0 || entry.path().extension() != ".sst") continue;

        // "data_12.sst" -> 12
        const std::string digits = name.substr(5, name.size() - 5 - 4);
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;

        try {
            found.emplace_back(std::stoi(digits), entry.path().string());
        } catch (const std::exception&) {
            continue;   // Not one of ours; leave it alone.
        }
    }

    // Oldest first, so that get()'s reverse walk still sees newest first.
    std::sort(found.begin(), found.end());
    for (const auto& [index, path] : found) {
        sst_files.push_back(path);
        sst_counter = std::max(sst_counter, index + 1);
    }

    if (!sst_files.empty()) {
        std::cout << "[NexusDB] Recovered " << sst_files.size()
                  << " SSTable(s) from " << db_dir << "\n";
    }

    // Still missing: replaying active.wal. Anything written since the last flush
    // is in that file and is not read back here, so a crash loses it.
}

NexusDB::~NexusDB() {
    // Take the same lock put() holds, so a concurrent write cannot be halfway
    // through the memtable while it is being serialized out.
    std::lock_guard<std::mutex> lock(db_mutex);
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