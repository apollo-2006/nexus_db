// Browser bindings for the storage engine. Every call goes to the real NexusDB in
// src/db.cpp, writing real SSTables and a real WAL into Emscripten's in-memory
// filesystem under /db, which the page lists and parses.
#include <emscripten/emscripten.h>

#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "db.hpp"

namespace {
const char* DIR = "/db";
NexusDB* db = nullptr;
NexusDB::ReadTrace last;
std::string last_value;
double bench_results[6];
}

extern "C" {

EMSCRIPTEN_KEEPALIVE void db_open(int memtable_limit) {
    db = new NexusDB(DIR, static_cast<size_t>(memtable_limit));
}

// A clean shutdown: the destructor flushes the memtable to an SSTable.
EMSCRIPTEN_KEEPALIVE void db_close() {
    delete db;
    db = nullptr;
}

// A crash: the object is abandoned without running its destructor, so nothing
// is flushed. Whatever the next open recovers came from the WAL on disk.
EMSCRIPTEN_KEEPALIVE void db_crash() {
    db = nullptr;
}

EMSCRIPTEN_KEEPALIVE void db_wipe() {
    delete db;
    db = nullptr;
    std::filesystem::remove_all(DIR);
}

EMSCRIPTEN_KEEPALIVE void db_put(const char* key, const char* value) { db->put(key, value); }
EMSCRIPTEN_KEEPALIVE void db_remove(const char* key) { db->remove(key); }

EMSCRIPTEN_KEEPALIVE int db_get(const char* key) {
    last = db->get_traced(key);
    last_value = last.value.value_or("");
    return last.value.has_value();
}
EMSCRIPTEN_KEEPALIVE const char* db_last_value() { return last_value.c_str(); }
EMSCRIPTEN_KEEPALIVE int db_last_source() { return last.source; }
EMSCRIPTEN_KEEPALIVE int db_last_checked() { return last.sstables_checked; }
EMSCRIPTEN_KEEPALIVE int db_last_tombstone() { return last.tombstone; }

EMSCRIPTEN_KEEPALIVE int db_sstables() { return static_cast<int>(db->sstable_count()); }
EMSCRIPTEN_KEEPALIVE int db_memtable_bytes() { return static_cast<int>(db->memtable_bytes()); }
EMSCRIPTEN_KEEPALIVE int db_replayed() { return static_cast<int>(db->wal_records_replayed()); }

// The native benchmark in src/main.cpp, scaled for a page: sequential writes into a
// fresh database, then point reads of four kinds. Results in ms and µs.
EMSCRIPTEN_KEEPALIVE void bench_run(int writes, int reads) {
    using Clock = std::chrono::steady_clock;
    const auto us = [](Clock::time_point t0) {
        return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
    };
    const char* dir = "/bench";
    std::filesystem::remove_all(dir);
    std::vector<std::string> keys, values;
    for (int i = 0; i < writes; i++) {
        keys.push_back("user_" + std::to_string(i));
        values.push_back("{\"name\": \"User" + std::to_string(i) + "\", \"tier\": \"premium\"}");
    }
    NexusDB b(dir);
    auto t0 = Clock::now();
    for (int i = 0; i < writes; i++) b.put(keys[i], values[i]);
    bench_results[0] = us(t0) / 1000.0;
    bench_results[1] = static_cast<double>(b.sstable_count());

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> any(0, writes - 1);
    // Timed as a batch: browsers coarsen their clock, so a single memtable read
    // measures as zero. Keys are chosen before the timer starts.
    const auto mean_us = [&](auto next_key) {
        std::vector<std::string> batch;
        for (int i = 0; i < reads; i++) batch.push_back(next_key());
        auto r0 = Clock::now();
        for (const auto& k : batch) b.get(k);
        return us(r0) / reads;
    };
    bench_results[2] = mean_us([&] { return keys[writes - 1 - (rng() % 100)]; });
    bench_results[3] = mean_us([&] { return keys[any(rng)]; });
    bench_results[4] = mean_us([&] { return keys[0]; });
    bench_results[5] = mean_us([&] { return std::string("nope_") + std::to_string(rng()); });
    std::filesystem::remove_all(dir);
}
EMSCRIPTEN_KEEPALIVE double bench_result(int i) { return bench_results[i]; }

}  // extern "C"
