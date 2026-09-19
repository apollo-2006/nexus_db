#include "../include/db.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "../include/encoding.hpp"
#include "../include/sstable.hpp"

NexusDB::NexusDB(const std::string& directory, size_t limit)
    : db_dir(directory), memtable_limit(limit) {
    std::filesystem::create_directories(db_dir);
    active_memtable = std::make_unique<MemTable>();

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

    // Replay the write-ahead log. Everything written since the last flush is in
    // active.wal and nowhere else, so this is what makes a crash lose nothing that
    // reached the log. The log is opened for appending only afterwards, once any
    // torn tail from the crash has been cut off.
    const std::string wal_path = db_dir + "/active.wal";
    replayed_records = replay_wal(wal_path);
    wal = std::make_unique<WriteAheadLog>(wal_path);

    if (replayed_records > 0) {
        std::cout << "[NexusDB] Replayed " << replayed_records << " WAL record(s)\n";
    }
}

size_t NexusDB::replay_wal(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return 0;

    in.seekg(0, std::ios::end);
    const uint64_t file_size = static_cast<uint64_t>(in.tellg());
    in.seekg(0, std::ios::beg);

    // Records are [flags][key][value], the shape WriteAheadLog appends.
    // good_end is the offset just past the last complete record.
    std::streamoff good_end = 0;
    size_t records = 0;
    for (;;) {
        uint8_t flags;
        if (!get_u8(in, flags)) break;
        std::string key, value;
        if (!get_bytes(in, key, file_size - static_cast<uint64_t>(in.tellg()))) break;
        if (!get_bytes(in, value, file_size - static_cast<uint64_t>(in.tellg()))) break;

        // Straight into the memtable: these records are already in the log.
        active_memtable->put(key, value, (flags & RECORD_TOMBSTONE) != 0);
        good_end = in.tellg();
        records++;
    }
    in.close();

    // A crash mid-append leaves a partial record at the end. Cut it off, or every
    // record appended after this open would sit behind it where replay cannot reach.
    if (good_end < static_cast<std::streamoff>(file_size)) {
        std::cout << "[NexusDB] Truncating " << (static_cast<std::streamoff>(file_size) - good_end)
                  << " byte(s) of torn WAL tail\n";
        std::filesystem::resize_file(path, static_cast<std::uintmax_t>(good_end));
    }

    // If the replayed memtable is already over the limit, the next put flushes it.
    return records;
}

NexusDB::~NexusDB() {
    // Take the same lock put() holds, so a concurrent write cannot be halfway
    // through the memtable while it is being serialized out.
    std::lock_guard<std::mutex> lock(db_mutex);
    flush_memtable(); // Safely flush RAM to disk before shutting down
}

void NexusDB::put(const std::string& key, const std::string& value) {
    write(key, value, false);
}

void NexusDB::remove(const std::string& key) {
    // A deletion is an ordinary write carrying the tombstone flag: the key may
    // live in an SSTable that this level cannot rewrite cheaply, so the delete
    // has to travel the same path as a value and win by being newer.
    write(key, "", true);
}

void NexusDB::write(const std::string& key, const std::string& value, bool tombstone) {
    std::lock_guard<std::mutex> lock(db_mutex);

    // 1. Append to the write-ahead log
    wal->append(key, value, tombstone);

    // 2. Insert into the memtable
    active_memtable->put(key, value, tombstone);

    // 3. Flush if the memory limit is exceeded
    if (active_memtable->byte_size() >= memtable_limit) flush_memtable();
}

void NexusDB::flush_memtable() {
    if (active_memtable->byte_size() == 0) return;

    std::string sst_path = db_dir + "/data_" + std::to_string(sst_counter++) + ".sst";

    if (!SSTable::write(sst_path, active_memtable->entries())) {
        // Keep the memtable and the WAL. Clearing the log after a failed write
        // would drop every record in this memtable with nothing left holding it.
        std::cerr << "[NexusDB] Flush to " << sst_path << " failed; keeping memtable\n";
        std::filesystem::remove(sst_path);
        --sst_counter;
        return;
    }

    sst_files.push_back(sst_path);

    // Reset RAM and WAL
    active_memtable = std::make_unique<MemTable>();
    wal->clear();

    std::cout << "[NexusDB] Flushed MemTable to disk: " << sst_path << "\n";
}

std::optional<std::string> NexusDB::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(db_mutex);
    return get_traced_locked(key).value;
}

NexusDB::ReadTrace NexusDB::get_traced(const std::string& key) {
    std::lock_guard<std::mutex> lock(db_mutex);
    return get_traced_locked(key);
}

NexusDB::ReadTrace NexusDB::get_traced_locked(const std::string& key) {
    ReadTrace trace;

    // 1. The memtable holds the newest version of anything written since the
    //    last flush, so a hit here ends the search whether it is a value or a
    //    deletion.
    if (auto entry = active_memtable->get(key)) {
        trace.source = -1;
        trace.tombstone = entry->tombstone;
        if (!entry->tombstone) trace.value = std::move(entry->value);
        return trace;
    }

    // 2. Then the SSTables, newest first, for the same reason.
    int position = 0;
    for (auto it = sst_files.rbegin(); it != sst_files.rend(); ++it, ++position) {
        trace.sstables_checked++;
        if (auto record = SSTable::search(*it, key)) {
            trace.source = position;
            trace.tombstone = record->tombstone;
            if (!record->tombstone) trace.value = std::move(record->value);
            return trace;
        }
    }

    return trace;
}

size_t NexusDB::sstable_count() {
    std::lock_guard<std::mutex> lock(db_mutex);
    return sst_files.size();
}

size_t NexusDB::memtable_bytes() {
    std::lock_guard<std::mutex> lock(db_mutex);
    return active_memtable->byte_size();
}
