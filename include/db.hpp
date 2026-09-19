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

    size_t memtable_limit;               // flush threshold, 1MB by default
    int sst_counter = 0;
    size_t replayed_records = 0;         // WAL records restored by the constructor

    void flush_memtable();
    size_t replay_wal(const std::string& path);
    void write(const std::string& key, const std::string& value, bool tombstone);

public:
    static constexpr size_t DEFAULT_MEMTABLE_LIMIT = 1024 * 1024;

    explicit NexusDB(const std::string& directory, size_t memtable_limit = DEFAULT_MEMTABLE_LIMIT);
    ~NexusDB();

    void put(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key);
    void remove(const std::string& key);

    // Where a read was answered from, for the dashboard and the web demo.
    // source is -1 for the memtable, the SSTable's position in the newest-first
    // search for a disk hit, and -2 for a miss. sstables_checked counts files opened.
    struct ReadTrace {
        std::optional<std::string> value;
        int source = -2;
        int sstables_checked = 0;
        bool tombstone = false;
    };
    ReadTrace get_traced(const std::string& key);

    size_t sstable_count();
    size_t memtable_bytes();
    size_t wal_records_replayed() const { return replayed_records; }

private:
    ReadTrace get_traced_locked(const std::string& key);  // caller holds db_mutex
};