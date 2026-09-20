#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "memtable.hpp"
#include "sstable.hpp"
#include "wal.hpp"

// One immutable file in the tree, and the level it currently sits at.
//
// Files are held by shared pointer so a compaction can retire one while a read
// is still inside it. Marking it obsolete hands the removal to whoever lets go
// last, rather than unlinking a file a reader is about to open.
struct SSTableFile {
    SSTableReader reader;
    int level = 0;
    uint64_t seq = 0;
    std::atomic<bool> obsolete{false};

    SSTableFile(SSTableReader r, int lvl, uint64_t s) : reader(std::move(r)), level(lvl), seq(s) {}
    ~SSTableFile();
};
using SSTableFilePtr = std::shared_ptr<SSTableFile>;

class NexusDB {
public:
    struct Options {
        // Bytes of live data in the memtable before it is handed to the flusher.
        size_t memtable_limit = 1024 * 1024;
        // Files allowed in level 0, which is the only level whose files overlap.
        size_t l0_compaction_trigger = 4;
        // Byte budget for level 1. Each level below it is ten times larger.
        uint64_t base_level_bytes = 8ull << 20;
        // Rough size of a file written by a compaction.
        uint64_t target_file_bytes = 2ull << 20;
        int max_levels = 7;
        // Flush and compact on a worker thread. Tests turn it off so that a
        // write returns with the work already done.
        bool background = true;
    };

    static constexpr size_t DEFAULT_MEMTABLE_LIMIT = 1024 * 1024;

    explicit NexusDB(const std::string& directory, size_t memtable_limit = DEFAULT_MEMTABLE_LIMIT);
    NexusDB(const std::string& directory, Options options);
    ~NexusDB();

    NexusDB(const NexusDB&) = delete;
    NexusDB& operator=(const NexusDB&) = delete;

    void put(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key);
    void remove(const std::string& key);

    // Where a read was answered from, for the dashboard and the web demo.
    // source is -1 for a memtable, -2 for a miss, and otherwise counts the files
    // the search passed before this one, in the order they were consulted.
    struct ReadTrace {
        std::optional<std::string> value;
        int source = -2;
        int level = -1;            // level of the file that answered, if a file did
        std::string file;          // and its name
        int sstables_checked = 0;  // files opened and scanned
        int sstables_skipped = 0;  // files ruled out by key range or Bloom filter
        bool tombstone = false;
    };
    ReadTrace get_traced(const std::string& key);

    size_t sstable_count();
    size_t memtable_bytes();
    size_t wal_records_replayed() const { return replayed_records_; }
    size_t flush_count() const { return flushes_.load(); }
    size_t compaction_count() const { return compactions_.load(); }
    // Bytes this process has written into SSTables, split by which job wrote
    // them. Their sum over the bytes the caller put in is the write
    // amplification the tree costs.
    uint64_t flushed_bytes() const { return flushed_bytes_.load(); }
    uint64_t compacted_bytes() const { return compacted_bytes_.load(); }

    int level_count();
    size_t files_in_level(int level);
    uint64_t bytes_in_level(int level);

    // The tree as it stands, level by level, for the dashboard and the demo.
    struct FileInfo {
        std::string name;
        int level = 0;
        uint64_t bytes = 0;
        uint64_t records = 0;
        uint64_t tombstones = 0;
        std::string min_key, max_key;
    };
    std::vector<FileInfo> files();

    // Blocks until the worker has nothing left to do. For tests, the benchmark
    // and the demo, which want to look at a settled tree.
    void wait_for_background();

private:
    void open(const std::string& directory);
    void write(const std::string& key, const std::string& value, bool tombstone);
    // Answers from the memtables if it can, and otherwise hands back the files
    // to search, in order. Caller holds at least a shared lock.
    bool read_memtables_locked(const std::string& key, ReadTrace& trace) const;
    std::vector<SSTableFilePtr> search_order_locked(const std::string& key) const;

    // Recovery.
    void load_manifest();
    void adopt_orphan_sstables(const std::vector<uint64_t>& listed);
    size_t replay_wals();
    void write_manifest_locked();

    // The write path.
    void rotate_memtable_locked(std::unique_lock<std::shared_mutex>& lock);
    std::string wal_path(uint64_t seq) const;
    std::string sst_path(uint64_t seq) const;

    // The worker, and the two jobs it runs.
    void background_loop();
    void flush_immutable_locked(std::unique_lock<std::shared_mutex>& lock);
    struct LevelStats {
        uint64_t bytes = 0;
        uint64_t records = 0;
        uint64_t tombstones = 0;
    };
    LevelStats level_stats_locked(int level) const;
    int deepest_level_locked() const;
    bool level_wants_compaction_locked(int level) const;
    bool compaction_needed_locked() const;
    void compact_once_locked(std::unique_lock<std::shared_mutex>& lock);
    uint64_t level_budget(int level) const;

    std::string db_dir_;
    Options options_;

    // Readers take this shared and hold it only long enough to look at the
    // memtables and take a reference to the files they will search. Writers and
    // the worker take it exclusively, and drop it while they do file I/O.
    mutable std::shared_mutex mu_;
    std::condition_variable_any work_cv_;   // worker: there is something to do
    std::condition_variable_any idle_cv_;   // writers and waiters: the worker finished

    std::unique_ptr<MemTable> active_;
    std::unique_ptr<MemTable> immutable_;   // handed to the flusher, still readable
    std::unique_ptr<WriteAheadLog> wal_;
    uint64_t wal_seq_ = 0;
    uint64_t immutable_wal_seq_ = 0;

    std::vector<std::vector<SSTableFilePtr>> levels_;
    uint64_t next_seq_ = 0;
    std::vector<std::string> compact_cursor_;  // per level, where the last compaction stopped

    std::thread worker_;
    bool shutdown_ = false;
    bool worker_idle_ = true;

    size_t replayed_records_ = 0;
    std::atomic<size_t> flushes_{0};
    std::atomic<size_t> compactions_{0};
    std::atomic<uint64_t> flushed_bytes_{0};
    std::atomic<uint64_t> compacted_bytes_{0};
};
