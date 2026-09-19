// Engine tests. `make test`.
//
// The crash tests fork: the child writes and then _exit()s, which skips every
// destructor, so nothing flushes the memtable on the way out. Whatever the parent
// can read back afterwards survived only because it was in the write-ahead log.
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

#include "../include/bloom.hpp"
#include "../include/db.hpp"

namespace fs = std::filesystem;
static int failures = 0;

static void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) failures++;
}

static std::string fresh(const char* name) {
    std::string dir = "/tmp/nexus_db_test_" + std::to_string(getpid()) + "_" + name;
    fs::remove_all(dir);
    return dir;
}

// Logs are numbered, and several can exist at once: one per memtable that has
// not been flushed yet.
static std::vector<fs::path> wal_files(const std::string& dir) {
    std::vector<std::pair<unsigned long long, fs::path>> found;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("wal_", 0) != 0 || e.path().extension() != ".log") continue;
        found.emplace_back(std::stoull(name.substr(4)), e.path());
    }
    std::sort(found.begin(), found.end());
    std::vector<fs::path> paths;
    for (auto& [seq, path] : found) paths.push_back(path);
    return paths;
}

static uintmax_t wal_bytes(const std::string& dir) {
    uintmax_t total = 0;
    for (const auto& path : wal_files(dir)) total += fs::file_size(path);
    return total;
}

static uintmax_t sst_bytes(const std::string& dir) {
    uintmax_t total = 0;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".sst") total += e.file_size();
    return total;
}

static void crash_after(const std::string& dir, int n, size_t limit) {
    std::fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        std::freopen("/dev/null", "w", stdout);
        auto* db = new NexusDB(dir, limit);  // deliberately never deleted
        for (int i = 0; i < n; i++) db->put("k" + std::to_string(i), "v" + std::to_string(i));
        db->remove("k3");
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
}

int main() {
    {
        // The filter may say maybe about a key it has never seen, which costs a
        // wasted seek, but it may never say no about one it holds, which would
        // lose data.
        std::vector<std::string> keys;
        for (int i = 0; i < 10000; i++) keys.push_back("user_" + std::to_string(i));
        const BloomFilter filter = BloomFilter::build(keys);

        bool all_present = true;
        for (const auto& key : keys) all_present &= filter.maybe_contains(key);
        check(all_present, "every key that was added reads back as maybe present");

        int false_positives = 0;
        for (int i = 0; i < 100000; i++)
            if (filter.maybe_contains("absent_" + std::to_string(i))) false_positives++;
        // Ten bits per key puts the theoretical rate near 1%. The bound here is
        // loose enough to survive a different key distribution and tight enough
        // that a broken hash, which would drive it toward 100%, fails.
        check(false_positives < 3000, "fewer than 3% of absent keys are false positives");

        check(!BloomFilter::build({}).maybe_contains("anything"),
              "a filter built from no keys rules out every key");
    }

    {
        auto dir = fresh("basic");
        NexusDB db(dir);
        db.put("a", "1");
        db.put("a", "2");
        db.put("b", "3");
        db.remove("b");
        check(db.get("a") == std::optional<std::string>("2"), "overwrite returns the newest value");
        check(!db.get("b").has_value(), "a deleted key reads as missing");
        check(!db.get("never").has_value(), "a key that was never written reads as missing");
        auto t = db.get_traced("a");
        check(t.source == -1 && t.sstables_checked == 0, "a fresh key is answered from the memtable");
    }

    {
        // Level 0 only, so the read path can be watched with every flush still
        // sitting where it landed.
        auto dir = fresh("level0");
        NexusDB::Options opts;
        opts.memtable_limit = 4096;
        opts.background = false;
        opts.l0_compaction_trigger = 1000;  // no compaction in this test
        NexusDB db(dir, opts);
        for (int i = 0; i < 2000; i++) db.put("k" + std::to_string(i), std::string(40, 'x'));

        check(db.sstable_count() >= 10 && db.files_in_level(0) == db.sstable_count(),
              "a small memtable limit flushes many files, all into level 0");

        auto t = db.get_traced("k0");
        check(t.value && t.source > 0, "an old key is found below the newer files");
        check(t.sstables_skipped > 0 && t.sstables_checked <= 2,
              "the files that cannot hold it are skipped rather than read");

        auto miss = db.get_traced("k99999");
        check(!miss.value && miss.sstables_checked <= 1,
              "a missing key is ruled out of nearly every file without a read");
    }

    {
        auto dir = fresh("flush");
        {
            NexusDB db(dir, 4096);
            for (int i = 0; i < 2000; i++) db.put("k" + std::to_string(i), std::string(40, 'x'));
            db.remove("k5");
            db.put("k6", "newest");
            db.wait_for_background();
            check(db.flush_count() >= 10, "a small memtable limit flushes many times");
            check(db.compaction_count() > 0 && db.sstable_count() < db.flush_count(),
                  "compaction merges those flushes into fewer files");
            check(db.level_count() > 1 && db.files_in_level(1) > 0, "and moves them off level 0");
            check(db.get("k0") == std::optional<std::string>(std::string(40, 'x')),
                  "an old key survives the merge");
        }
        NexusDB db(dir, 4096);
        check(db.get("k1999") == std::optional<std::string>(std::string(40, 'x')), "reopen re-adopts SSTables");
        check(!db.get("k5").has_value(), "a tombstone still hides the older value after reopening");
        check(db.get("k6") == std::optional<std::string>("newest"), "the newest value wins across files");
    }

    {
        // Every key rewritten five times. Without compaction all six versions
        // stay on disk; with it only the newest survives the merge.
        auto dir = fresh("space");
        NexusDB::Options opts;
        opts.memtable_limit = 16 * 1024;
        opts.base_level_bytes = 64 * 1024;
        opts.target_file_bytes = 32 * 1024;
        NexusDB db(dir, opts);

        const std::string value(60, 'v');
        for (int round = 0; round < 6; round++)
            for (int i = 0; i < 2000; i++) db.put("key" + std::to_string(i), value);
        db.wait_for_background();

        const uintmax_t live = 2000 * (value.size() + 8);
        check(sst_bytes(dir) < 3 * live, "rewritten keys do not keep six copies on disk");
        check(db.get("key1999") == std::optional<std::string>(value), "and the newest version is the one that is read");

        for (int i = 0; i < 2000; i++) db.remove("key" + std::to_string(i));
        db.wait_for_background();
        bool all_gone = true;
        for (int i = 0; i < 2000; i++) all_gone &= !db.get("key" + std::to_string(i)).has_value();
        check(all_gone, "deleting every key reads back as empty");
        check(sst_bytes(dir) < live, "and the deletions release the space rather than adding to it");
    }

    {
        // Four readers against a tree that a writer and the worker are both
        // changing underneath them. Every key holds the same value throughout,
        // so any version a reader sees is the right one, and a missing key is
        // not.
        auto dir = fresh("threads");
        NexusDB::Options opts;
        opts.memtable_limit = 32 * 1024;
        opts.base_level_bytes = 128 * 1024;
        opts.target_file_bytes = 64 * 1024;
        NexusDB db(dir, opts);

        const std::string value(80, 'v');
        const int keys = 5000;
        for (int i = 0; i < keys; i++) db.put("key" + std::to_string(i), value);

        std::atomic<bool> stop{false};
        std::atomic<int> wrong{0};
        std::atomic<long> reads{0};
        std::vector<std::thread> readers;
        for (int t = 0; t < 4; t++) {
            readers.emplace_back([&, t] {
                std::mt19937 rng(static_cast<unsigned>(t) + 1);
                while (!stop.load()) {
                    auto value_read = db.get("key" + std::to_string(rng() % keys));
                    if (!value_read || *value_read != value) wrong++;
                    reads++;
                }
            });
        }
        for (int round = 0; round < 3; round++)
            for (int i = 0; i < keys; i++) db.put("key" + std::to_string(i), value);
        stop = true;
        for (auto& reader : readers) reader.join();

        check(wrong.load() == 0 && reads.load() > 0,
              "reads stay correct while a writer and the compactor work underneath them");
    }

    {
        // What a crash during a compaction leaves: outputs on disk that the
        // manifest does not name, and the inputs it does name still in place.
        auto dir = fresh("orphan");
        {
            NexusDB db(dir, 4096);
            for (int i = 0; i < 500; i++) db.put("k" + std::to_string(i), "v" + std::to_string(i));
            db.wait_for_background();
        }
        const uintmax_t before = sst_bytes(dir);
        { std::ofstream f(dir + "/data_99999.sst", std::ios::binary); f << std::string(4096, 'x'); }

        NexusDB db(dir, 4096);
        check(!fs::exists(dir + "/data_99999.sst"), "a file the manifest does not name is removed at open");
        check(sst_bytes(dir) == before, "and nothing it does name is touched");
        check(db.get("k499") == std::optional<std::string>("v499"), "the data is intact");
    }

    {
        // The engine used to store deletions as the literal value "@@TOMBSTONE@@",
        // so writing that string deleted the key instead of storing it.
        auto dir = fresh("tombstone_value");
        const std::string looks_like_a_tombstone = "@@TOMBSTONE@@";
        {
            NexusDB db(dir, 4096);
            db.put("marker", looks_like_a_tombstone);
            db.put("padding", std::string(8192, 'x'));  // forces a flush
            check(db.get("marker") == std::optional<std::string>(looks_like_a_tombstone),
                  "a value that looks like a tombstone is still a value");
        }
        NexusDB db(dir, 4096);
        check(db.get("marker") == std::optional<std::string>(looks_like_a_tombstone),
              "and it survives a flush and reopen");

        auto t = db.get_traced("marker");
        check(t.value.has_value() && !t.tombstone, "the read is not reported as a deletion");
    }

    {
        auto dir = fresh("foreign_sst");
        fs::create_directories(dir);
        { std::ofstream f(dir + "/data_0.sst", std::ios::binary); f << "not an sstable at all"; }
        NexusDB db(dir);
        db.put("k", "v");
        check(db.get("k") == std::optional<std::string>("v") && !db.get("other").has_value(),
              "a file that is not an SSTable is not parsed as one");
    }

    {
        auto dir = fresh("crash");
        crash_after(dir, 50, NexusDB::DEFAULT_MEMTABLE_LIMIT);
        check(wal_bytes(dir) > 0, "the crashed process left a WAL behind");
        NexusDB db(dir);
        check(db.wal_records_replayed() == 51, "all 50 puts and the delete are replayed");
        check(db.get("k49") == std::optional<std::string>("v49"), "a write that was never flushed survives the crash");
        check(!db.get("k3").has_value(), "a delete that was never flushed survives the crash");
    }

    {
        auto dir = fresh("crash_after_flush");
        crash_after(dir, 3000, 8192);
        NexusDB db(dir, 8192);
        bool all = true;
        for (int i = 0; i < 3000; i++) if (i != 3 && db.get("k" + std::to_string(i)) != std::optional<std::string>("v" + std::to_string(i))) all = false;
        check(all && db.sstable_count() > 0, "a crash after several flushes loses nothing: SSTables plus WAL");
    }

    {
        auto dir = fresh("torn");
        crash_after(dir, 10, NexusDB::DEFAULT_MEMTABLE_LIMIT);
        const auto wal = wal_files(dir).back();
        const auto whole = fs::file_size(wal);
        fs::resize_file(wal, whole - 3);  // the last record is cut mid-value
        {
            NexusDB db(dir);
            check(db.wal_records_replayed() == 10, "a torn final record is skipped, the rest replay");
            check(fs::file_size(wal) < whole - 3, "the torn tail is truncated off the log");
            db.put("after", "tear");
        }
        // A clean close flushes everything, so crash again to prove the log is readable
        // past the point where it was torn.
        crash_after(dir, 0, NexusDB::DEFAULT_MEMTABLE_LIMIT);
        NexusDB db(dir);
        check(db.get("after") == std::optional<std::string>("tear"), "writes after a torn tail are still readable");
    }

    {
        auto dir = fresh("garbage");
        fs::create_directories(dir);
        { std::ofstream f(dir + "/wal_0.log", std::ios::binary); f << "\xff\xff\xff\xff\xff\xff\xff\x7f garbage"; }
        NexusDB db(dir);
        check(db.wal_records_replayed() == 0 && fs::file_size(dir + "/wal_0.log") == 0,
              "a corrupt length is not allocated; the log is truncated to its last good record");
    }

    // Every database above has been destroyed by now, so its final flush has run.
    const std::string prefix = "nexus_db_test_" + std::to_string(getpid()) + "_";
    for (const auto& e : fs::directory_iterator("/tmp"))
        if (e.path().filename().string().rfind(prefix, 0) == 0) fs::remove_all(e.path());

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures != 0;
}
