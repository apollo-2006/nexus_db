// Engine tests. `make test`.
//
// The crash tests fork: the child writes and then _exit()s, which skips every
// destructor, so nothing flushes the memtable on the way out. Whatever the parent
// can read back afterwards survived only because it was in the write-ahead log.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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
        auto dir = fresh("flush");
        {
            NexusDB db(dir, 4096);
            for (int i = 0; i < 2000; i++) db.put("k" + std::to_string(i), std::string(40, 'x'));
            db.remove("k5");
            db.put("k6", "newest");
            check(db.sstable_count() >= 10, "a small memtable limit flushes many SSTables");
            auto t = db.get_traced("k0");
            check(t.value && t.source > 0 && t.sstables_checked == t.source + 1,
                  "an old key is found in an older SSTable, newest searched first");
        }
        NexusDB db(dir, 4096);
        check(db.get("k1999") == std::optional<std::string>(std::string(40, 'x')), "reopen re-adopts SSTables");
        check(!db.get("k5").has_value(), "a tombstone in a newer SSTable hides the older value");
        check(db.get("k6") == std::optional<std::string>("newest"), "the newest value wins across files");
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
        check(fs::exists(dir + "/active.wal") && fs::file_size(dir + "/active.wal") > 0, "the crashed process left a WAL behind");
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
        const auto wal = dir + "/active.wal";
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
        { std::ofstream f(dir + "/active.wal", std::ios::binary); f << "\xff\xff\xff\xff\xff\xff\xff\x7f garbage"; }
        NexusDB db(dir);
        check(db.wal_records_replayed() == 0 && fs::file_size(dir + "/active.wal") == 0,
              "a corrupt length is not allocated; the log is truncated to its last good record");
    }

    // Every database above has been destroyed by now, so its final flush has run.
    const std::string prefix = "nexus_db_test_" + std::to_string(getpid()) + "_";
    for (const auto& e : fs::directory_iterator("/tmp"))
        if (e.path().filename().string().rfind(prefix, 0) == 0) fs::remove_all(e.path());

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures != 0;
}
