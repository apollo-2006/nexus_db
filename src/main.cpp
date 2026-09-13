#include "../include/db.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

// Benchmark: sequential writes, then point reads against keys that live in the
// memtable, keys that were flushed to SSTables, and keys that do not exist.
//
//   ./nexus_db [num_writes] [reads_per_kind]
//
// Always starts from an empty directory. The database re-adopts SSTables on
// open, so reusing the directory would stack each run on top of the last and
// make every read slower than the one before.

using Clock = std::chrono::steady_clock;

static std::string make_key(int i) { return "user_" + std::to_string(i); }

static std::string make_value(int i) {
    return "{\"name\": \"User" + std::to_string(i) + "\", \"tier\": \"premium\"}";
}

static double us_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

static void report(const char* label, std::vector<double> lat) {
    std::sort(lat.begin(), lat.end());
    const auto pct = [&](double p) { return lat[static_cast<size_t>(p * (lat.size() - 1))]; };
    double sum = 0;
    for (double v : lat) sum += v;
    std::printf("  %-22s n=%-6zu mean %9.1f us   p50 %9.1f us   p99 %9.1f us\n",
                label, lat.size(), sum / lat.size(), pct(0.50), pct(0.99));
}

int main(int argc, char** argv) {
    const int num_writes = argc > 1 ? std::atoi(argv[1]) : 1000000;
    const int reads_per_kind = argc > 2 ? std::atoi(argv[2]) : 200;
    if (num_writes <= 0 || reads_per_kind <= 0) {
        std::cerr << "usage: " << argv[0] << " [num_writes] [reads_per_kind]\n";
        return 1;
    }

    const std::string dir = "./bench_db_data";
    std::filesystem::remove_all(dir);

    std::vector<std::string> keys, values;
    keys.reserve(num_writes);
    values.reserve(num_writes);
    for (int i = 0; i < num_writes; i++) {
        keys.push_back(make_key(i));
        values.push_back(make_value(i));
    }

    NexusDB db(dir);

    auto t0 = Clock::now();
    for (int i = 0; i < num_writes; i++) db.put(keys[i], values[i]);
    const double write_ms = us_since(t0) / 1000.0;

    size_t sst_count = 0, sst_bytes = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() == ".sst") {
            sst_count++;
            sst_bytes += e.file_size();
        }
    }

    std::printf("writes: %d in %.0f ms (%.0f ops/s), %zu SSTables, %.1f MB on disk\n",
                num_writes, write_ms, num_writes / (write_ms / 1000.0), sst_count,
                sst_bytes / 1e6);

    // The last few thousand writes are still in the memtable; the first ones are
    // in the oldest SSTable, which a newest-first read reaches last.
    std::mt19937 rng(42);
    const int tail = std::min(num_writes, 5000);
    std::uniform_int_distribution<int> recent(num_writes - tail, num_writes - 1);
    std::uniform_int_distribution<int> any(0, num_writes - 1);

    const auto time_reads = [&](const char* label, auto next_key, bool expect_hit) {
        std::vector<double> lat;
        lat.reserve(reads_per_kind);
        for (int i = 0; i < reads_per_kind; i++) {
            const std::string k = next_key();
            auto r0 = Clock::now();
            auto v = db.get(k);
            lat.push_back(us_since(r0));
            if (v.has_value() != expect_hit) {
                std::cerr << "wrong result for " << k << "\n";
                std::exit(1);
            }
        }
        report(label, std::move(lat));
    };

    std::printf("reads:\n");
    time_reads("recent key (memtable)", [&] { return keys[recent(rng)]; }, true);
    time_reads("random existing key", [&] { return keys[any(rng)]; }, true);
    time_reads("oldest key", [&] { return keys[0]; }, true);
    time_reads("missing key", [&] { return std::string("nope_") + std::to_string(rng()); }, false);

    return 0;
}
