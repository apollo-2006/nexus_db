#include "../include/db.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "../include/encoding.hpp"

namespace fs = std::filesystem;

namespace {

// The manifest names every file that belongs to the tree, and at which level.
//
// It is the difference between a crash during a compaction being recoverable and
// not: the outputs of a half-finished merge are on disk but not listed, so the
// next open deletes them and reads the inputs, which are still listed. Writing a
// fresh copy and renaming it over the old one makes each update atomic.
constexpr char MANIFEST_MAGIC[7] = {'N', 'X', 'M', 'A', 'N', 'I', 'F'};
constexpr uint8_t MANIFEST_VERSION = 1;

// Whether two key ranges touch. Compaction rewrites everything a merge could
// change, which is every file one level down that overlaps its inputs.
bool ranges_overlap(const std::string& a_lo, const std::string& a_hi, const std::string& b_lo,
                    const std::string& b_hi) {
    return !(a_hi < b_lo || b_hi < a_lo);
}

}  // namespace

SSTableFile::~SSTableFile() {
    // The last owner removes the file. A compaction marks its inputs obsolete
    // while reads may still be inside them; unlinking there would turn a read
    // that was about to open one into a spurious miss.
    if (obsolete.load()) {
        std::error_code ec;
        fs::remove(reader.path(), ec);
    }
}

NexusDB::NexusDB(const std::string& directory, size_t memtable_limit) {
    options_.memtable_limit = memtable_limit;
    open(directory);
}

NexusDB::NexusDB(const std::string& directory, Options options) : options_(options) { open(directory); }

void NexusDB::open(const std::string& directory) {
    db_dir_ = directory;
    fs::create_directories(db_dir_);
    active_ = std::make_unique<MemTable>();
    levels_.resize(static_cast<size_t>(std::max(2, options_.max_levels)));
    compact_cursor_.resize(levels_.size());

    load_manifest();

    // Everything written since the last flush is in a log and nowhere else, so
    // this is what makes a crash lose nothing that reached one.
    replayed_records_ = replay_wals();
    wal_ = std::make_unique<WriteAheadLog>(wal_path(wal_seq_));

    if (replayed_records_ > 0) {
        std::cout << "[NexusDB] Replayed " << replayed_records_ << " WAL record(s)\n";
    }

    if (options_.background) worker_ = std::thread(&NexusDB::background_loop, this);
}

NexusDB::~NexusDB() {
    {
        std::unique_lock<std::shared_mutex> lock(mu_);
        shutdown_ = true;
    }
    work_cv_.notify_all();
    idle_cv_.notify_all();
    if (worker_.joinable()) worker_.join();

    std::unique_lock<std::shared_mutex> lock(mu_);
    // Whatever the worker did not reach. A clean close leaves nothing that only
    // a log holds, which is what the reopen tests rely on.
    if (immutable_) flush_immutable_locked(lock);
    if (active_->byte_size() > 0) {
        immutable_ = std::move(active_);
        active_ = std::make_unique<MemTable>();
        immutable_wal_seq_ = wal_seq_;
        wal_.reset();  // the flush deletes this log once its records are in a file
        flush_immutable_locked(lock);
    }
}

// ---------------------------------------------------------------- recovery

void NexusDB::load_manifest() {
    const std::string path = db_dir_ + "/MANIFEST";
    std::vector<std::pair<int, uint64_t>> listed;  // level, seq

    std::ifstream in(path, std::ios::binary);
    if (in.is_open()) {
        char magic[sizeof(MANIFEST_MAGIC)];
        uint8_t version = 0;
        uint32_t count = 0;
        if (in.read(magic, sizeof(magic)) && std::memcmp(magic, MANIFEST_MAGIC, sizeof(magic)) == 0 &&
            get_u8(in, version) && version == MANIFEST_VERSION && get_u64(in, next_seq_) && get_u32(in, count)) {
            for (uint32_t i = 0; i < count; i++) {
                uint32_t level;
                uint64_t seq;
                if (!get_u32(in, level) || !get_u64(in, seq)) break;
                listed.emplace_back(static_cast<int>(level), seq);
            }
        } else {
            std::cerr << "[NexusDB] MANIFEST is unreadable; falling back to the files on disk\n";
        }
        in.close();
    }

    std::vector<uint64_t> adopted;
    for (const auto& [level, seq] : listed) {
        SSTableReader reader(sst_path(seq));
        if (!reader.ok()) {
            std::cerr << "[NexusDB] MANIFEST lists " << sst_path(seq) << ", which cannot be read\n";
            continue;
        }
        const int lvl = std::min(level, static_cast<int>(levels_.size()) - 1);
        levels_[lvl].push_back(std::make_shared<SSTableFile>(std::move(reader), lvl, seq));
        adopted.push_back(seq);
        next_seq_ = std::max(next_seq_, seq + 1);
    }

    adopt_orphan_sstables(adopted);

    // Level 0 is searched newest first; every level below it is disjoint and
    // searched by key range.
    std::sort(levels_[0].begin(), levels_[0].end(),
              [](const SSTableFilePtr& a, const SSTableFilePtr& b) { return a->seq < b->seq; });
    for (size_t i = 1; i < levels_.size(); i++) {
        std::sort(levels_[i].begin(), levels_[i].end(), [](const SSTableFilePtr& a, const SSTableFilePtr& b) {
            return a->reader.min_key() < b->reader.min_key();
        });
    }

    size_t total = 0;
    for (const auto& level : levels_) total += level.size();
    if (total > 0) std::cout << "[NexusDB] Recovered " << total << " SSTable(s) from " << db_dir_ << "\n";
}

void NexusDB::adopt_orphan_sstables(const std::vector<uint64_t>& listed) {
    const bool have_manifest = fs::exists(db_dir_ + "/MANIFEST");

    for (const auto& entry : fs::directory_iterator(db_dir_)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("data_", 0) != 0 || entry.path().extension() != ".sst") continue;

        const std::string digits = name.substr(5, name.size() - 5 - 4);
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;
        uint64_t seq;
        try {
            seq = std::stoull(digits);
        } catch (const std::exception&) {
            continue;  // not one of ours; leave it alone
        }
        if (std::find(listed.begin(), listed.end(), seq) != listed.end()) continue;

        if (have_manifest) {
            // A file the manifest does not name is the output of a compaction or
            // flush that did not finish. Nothing refers to it, and its contents
            // are still in the files that do.
            std::cout << "[NexusDB] Removing " << name << ", left by an unfinished write\n";
            std::error_code ec;
            fs::remove(entry.path(), ec);
            continue;
        }

        // No manifest at all: a database from before there was one. Take every
        // file as level 0, oldest first, which is how it was being read.
        SSTableReader reader(entry.path().string());
        if (!reader.ok()) {
            std::cerr << "[NexusDB] Ignoring " << name << ": not an SSTable this build can read\n";
            continue;
        }
        levels_[0].push_back(std::make_shared<SSTableFile>(std::move(reader), 0, seq));
        next_seq_ = std::max(next_seq_, seq + 1);
    }
}

size_t NexusDB::replay_wals() {
    // A log written before logs were numbered.
    const std::string legacy = db_dir_ + "/active.wal";
    if (fs::exists(legacy) && !fs::exists(wal_path(0))) {
        std::error_code ec;
        fs::rename(legacy, wal_path(0), ec);
    }

    std::vector<uint64_t> seqs;
    for (const auto& entry : fs::directory_iterator(db_dir_)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("wal_", 0) != 0 || entry.path().extension() != ".log") continue;
        const std::string digits = name.substr(4, name.size() - 4 - 4);
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;
        seqs.push_back(std::stoull(digits));
    }

    // Oldest first: a later log holds the newer version of any key both mention.
    std::sort(seqs.begin(), seqs.end());

    size_t records = 0;
    for (uint64_t seq : seqs) {
        const std::string path = wal_path(seq);
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) continue;

        in.seekg(0, std::ios::end);
        const uint64_t file_size = static_cast<uint64_t>(in.tellg());
        in.seekg(0, std::ios::beg);

        std::streamoff good_end = 0;
        for (;;) {
            uint8_t flags;
            if (!get_u8(in, flags)) break;
            std::string key, value;
            if (!get_bytes(in, key, file_size - static_cast<uint64_t>(in.tellg()))) break;
            if (!get_bytes(in, value, file_size - static_cast<uint64_t>(in.tellg()))) break;

            active_->put(key, value, (flags & RECORD_TOMBSTONE) != 0);
            good_end = in.tellg();
            records++;
        }
        in.close();

        // A crash during an append leaves a partial record. Cut it off, or every
        // record appended after this open would sit behind it, where replay stops.
        if (good_end < static_cast<std::streamoff>(file_size)) {
            std::cout << "[NexusDB] Truncating " << (static_cast<std::streamoff>(file_size) - good_end)
                      << " byte(s) of torn WAL tail\n";
            fs::resize_file(path, static_cast<std::uintmax_t>(good_end));
        }
        wal_seq_ = std::max(wal_seq_, seq + 1);
    }

    // The replayed records are held only by these logs and the memtable, so they
    // stay until the flush that writes them into a file.
    return records;
}

void NexusDB::write_manifest_locked() {
    const std::string path = db_dir_ + "/MANIFEST";
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "[NexusDB] Cannot write " << tmp << "\n";
            return;
        }
        out.write(MANIFEST_MAGIC, sizeof(MANIFEST_MAGIC));
        put_u8(out, MANIFEST_VERSION);
        put_u64(out, next_seq_);

        uint32_t count = 0;
        for (const auto& level : levels_) count += static_cast<uint32_t>(level.size());
        put_u32(out, count);

        for (size_t level = 0; level < levels_.size(); level++) {
            for (const auto& file : levels_[level]) {
                put_u32(out, static_cast<uint32_t>(level));
                put_u64(out, file->seq);
            }
        }
        out.close();
        if (!out) {
            std::cerr << "[NexusDB] Cannot write " << tmp << "\n";
            return;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) std::cerr << "[NexusDB] Cannot install " << path << ": " << ec.message() << "\n";
}

std::string NexusDB::wal_path(uint64_t seq) const {
    return db_dir_ + "/wal_" + std::to_string(seq) + ".log";
}

std::string NexusDB::sst_path(uint64_t seq) const {
    return db_dir_ + "/data_" + std::to_string(seq) + ".sst";
}

// ------------------------------------------------------------- write path

void NexusDB::put(const std::string& key, const std::string& value) { write(key, value, false); }

void NexusDB::remove(const std::string& key) {
    // A deletion is an ordinary write carrying the tombstone flag. The key may
    // live in a file this write cannot afford to rewrite, so the delete travels
    // the same path as a value and wins by being newer.
    write(key, "", true);
}

void NexusDB::write(const std::string& key, const std::string& value, bool tombstone) {
    std::unique_lock<std::shared_mutex> lock(mu_);

    wal_->append(key, value, tombstone);
    active_->put(key, value, tombstone);

    if (active_->byte_size() >= options_.memtable_limit) rotate_memtable_locked(lock);
}

void NexusDB::rotate_memtable_locked(std::unique_lock<std::shared_mutex>& lock) {
    if (options_.background) {
        // One memtable may be waiting to be flushed. A second would mean writes
        // are outrunning the flusher, so the writer waits here rather than
        // letting memory grow without a bound.
        idle_cv_.wait(lock, [&] { return immutable_ == nullptr || shutdown_; });
        if (shutdown_) return;
    }

    immutable_ = std::move(active_);
    active_ = std::make_unique<MemTable>();
    immutable_wal_seq_ = wal_seq_;

    // Writes from here on go to a new log, so the old one can be deleted whole
    // once its records are in a file.
    wal_seq_++;
    wal_ = std::make_unique<WriteAheadLog>(wal_path(wal_seq_));

    if (options_.background) {
        work_cv_.notify_all();
        return;
    }

    flush_immutable_locked(lock);
    while (compaction_needed_locked()) compact_once_locked(lock);
}

// -------------------------------------------------------------- read path

std::optional<std::string> NexusDB::get(const std::string& key) { return get_traced(key).value; }

NexusDB::ReadTrace NexusDB::get_traced(const std::string& key) {
    ReadTrace trace;
    std::vector<SSTableFilePtr> files;
    {
        // The lock is held for the memtable lookup and for taking a reference to
        // the files to search, and released before any of them is opened. A read
        // that has to go to disk therefore blocks neither writers nor each other.
        std::shared_lock<std::shared_mutex> lock(mu_);
        if (read_memtables_locked(key, trace)) return trace;
        files = search_order_locked(key);
    }

    // Holding a reference is what makes this safe: a compaction may retire any
    // of these files while the search is inside them, and the file is removed
    // from disk only once the last reference to it is gone.
    int position = 0;
    for (const auto& file : files) {
        // A file whose key range or Bloom filter rules the key out is never
        // opened, which is what keeps a miss from costing a pass over the tree.
        if (!file->reader.may_hold(key)) {
            trace.sstables_skipped++;
            position++;
            continue;
        }
        trace.sstables_checked++;
        auto record = file->reader.get(key);
        if (record) {
            trace.source = position;
            trace.level = file->level;
            trace.file = fs::path(file->reader.path()).filename().string();
            trace.tombstone = record->tombstone;
            if (!record->tombstone) trace.value = std::move(record->value);
            return trace;
        }
        position++;
    }

    return trace;
}

bool NexusDB::read_memtables_locked(const std::string& key, ReadTrace& trace) const {
    // The memtables hold the newest version of anything written since the last
    // flush, so a hit there ends the search whether it is a value or a deletion.
    for (const MemTable* table : {active_.get(), immutable_.get()}) {
        if (table == nullptr) continue;
        if (auto entry = table->get(key)) {
            trace.source = -1;
            trace.tombstone = entry->tombstone;
            if (!entry->tombstone) trace.value = std::move(entry->value);
            return true;
        }
    }
    return false;
}

std::vector<SSTableFilePtr> NexusDB::search_order_locked(const std::string& key) const {
    std::vector<SSTableFilePtr> files;

    // Level 0 files overlap, so any of them can hold the key and the newest has
    // to be consulted first.
    files.insert(files.end(), levels_[0].rbegin(), levels_[0].rend());

    // Below that a level is disjoint, so at most one of its files can hold it.
    for (size_t level = 1; level < levels_.size(); level++) {
        const auto& candidates = levels_[level];
        auto it = std::upper_bound(
            candidates.begin(), candidates.end(), key,
            [](const std::string& k, const SSTableFilePtr& f) { return k < f->reader.min_key(); });
        if (it == candidates.begin()) continue;
        --it;
        if (key > (*it)->reader.max_key()) continue;
        files.push_back(*it);
    }

    return files;
}

// --------------------------------------------------------- the background

void NexusDB::background_loop() {
    std::unique_lock<std::shared_mutex> lock(mu_);
    for (;;) {
        worker_idle_ = true;
        idle_cv_.notify_all();
        work_cv_.wait(lock, [&] { return shutdown_ || immutable_ != nullptr || compaction_needed_locked(); });
        if (shutdown_) return;

        worker_idle_ = false;
        if (immutable_) {
            flush_immutable_locked(lock);
            continue;
        }
        if (compaction_needed_locked()) compact_once_locked(lock);
    }
}

void NexusDB::flush_immutable_locked(std::unique_lock<std::shared_mutex>& lock) {
    if (!immutable_ || immutable_->byte_size() == 0) {
        immutable_.reset();
        idle_cv_.notify_all();
        return;
    }

    const std::vector<Record> records = immutable_->entries();
    const uint64_t seq = next_seq_;
    const std::string path = sst_path(seq);

    // The write itself is the slow part and touches nothing shared, so it runs
    // with the lock released: reads and writes carry on against the memtables.
    lock.unlock();
    bool ok = SSTableWriter::write(path, records);
    SSTableReader reader(path);
    ok = ok && reader.ok();
    lock.lock();

    if (!ok) {
        // The memtable and its log stay exactly as they were. Dropping either
        // would lose records nothing else holds.
        std::cerr << "[NexusDB] Flush to " << path << " failed; keeping the memtable\n";
        std::error_code ec;
        fs::remove(path, ec);
        work_cv_.wait_for(lock, std::chrono::milliseconds(100));
        return;
    }

    flushed_bytes_ += reader.file_bytes();
    levels_[0].push_back(std::make_shared<SSTableFile>(std::move(reader), 0, seq));
    next_seq_ = seq + 1;
    immutable_.reset();
    write_manifest_locked();
    flushes_++;

    // The records are in a file now, so every log up to the one this memtable
    // was filled from has nothing left to protect.
    for (uint64_t s = 0; s <= immutable_wal_seq_; s++) {
        std::error_code ec;
        fs::remove(wal_path(s), ec);
    }

    std::cout << "[NexusDB] Flushed MemTable to disk: " << path << "\n";
    idle_cv_.notify_all();
}

uint64_t NexusDB::level_budget(int level) const {
    uint64_t budget = options_.base_level_bytes;
    for (int i = 1; i < level; i++) budget *= 10;
    return budget;
}

NexusDB::LevelStats NexusDB::level_stats_locked(int level) const {
    LevelStats stats;
    for (const auto& file : levels_[static_cast<size_t>(level)]) {
        stats.bytes += file->reader.file_bytes();
        stats.records += file->reader.record_count();
        stats.tombstones += file->reader.tombstone_count();
    }
    return stats;
}

int NexusDB::deepest_level_locked() const {
    int deepest = 0;
    for (size_t level = 0; level < levels_.size(); level++)
        if (!levels_[level].empty()) deepest = static_cast<int>(level);
    return deepest;
}

// A level earns a compaction by being over its byte budget, or by being thick
// with deletions. The second rule is what makes a delete give back the space it
// covers: a tombstone only releases anything once it is merged with the value it
// deletes, and the value is always further down.
bool NexusDB::level_wants_compaction_locked(int level) const {
    const LevelStats stats = level_stats_locked(level);
    // Level 0 is measured in files rather than bytes: what makes it expensive is
    // that its files overlap, so a read has to consult every one of them.
    if (level == 0) {
        if (levels_[0].size() >= options_.l0_compaction_trigger) return true;
    } else if (stats.bytes > level_budget(level)) {
        return true;
    }
    // Deletions are only worth chasing downward while the values they cover are
    // still below. At the deepest level there is nothing left to cancel.
    if (level >= deepest_level_locked()) return false;
    return stats.records > 0 && stats.tombstones * 4 > stats.records;
}

bool NexusDB::compaction_needed_locked() const {
    for (size_t level = 0; level + 1 < levels_.size(); level++)
        if (level_wants_compaction_locked(static_cast<int>(level))) return true;
    return false;
}

void NexusDB::compact_once_locked(std::unique_lock<std::shared_mutex>& lock) {
    // Which level is over its budget by the most. Level 0 is counted in files
    // rather than bytes, because what makes it expensive is that a read has to
    // consult every one of them.
    int from = -1;
    if (level_wants_compaction_locked(0)) {
        from = 0;
    } else {
        double worst = 0.0;
        for (size_t level = 1; level + 1 < levels_.size(); level++) {
            if (!level_wants_compaction_locked(static_cast<int>(level))) continue;
            const LevelStats stats = level_stats_locked(static_cast<int>(level));
            const double ratio =
                static_cast<double>(stats.bytes) / static_cast<double>(level_budget(static_cast<int>(level)));
            if (ratio > worst) {
                worst = ratio;
                from = static_cast<int>(level);
            }
        }
    }
    if (from < 0 || from + 1 >= static_cast<int>(levels_.size())) return;
    const int to = from + 1;

    // Inputs, newest first. All of level 0, since its files overlap each other
    // and a merge of some of them could leave an older version on top of a newer
    // one. One file otherwise, taken from where the last compaction of this
    // level stopped so that the level is swept rather than rewritten at one end.
    std::vector<SSTableFilePtr> inputs;
    if (from == 0) {
        inputs.assign(levels_[0].rbegin(), levels_[0].rend());
    } else {
        const auto& files = levels_[from];
        auto pick = std::find_if(files.begin(), files.end(), [&](const SSTableFilePtr& f) {
            return f->reader.min_key() > compact_cursor_[from];
        });
        inputs.push_back(pick == files.end() ? files.front() : *pick);
    }
    if (inputs.empty()) return;

    std::string lo = inputs.front()->reader.min_key();
    std::string hi = inputs.front()->reader.max_key();
    for (const auto& file : inputs) {
        lo = std::min(lo, file->reader.min_key());
        hi = std::max(hi, file->reader.max_key());
    }

    std::vector<SSTableFilePtr> overlap;
    for (const auto& file : levels_[to]) {
        if (ranges_overlap(lo, hi, file->reader.min_key(), file->reader.max_key())) overlap.push_back(file);
    }

    // A tombstone may only be dropped once nothing below can still hold an older
    // version of that key, or the delete would uncover it.
    bool anything_below = false;
    for (size_t level = to + 1; level < levels_.size(); level++) anything_below |= !levels_[level].empty();
    const bool drop_tombstones = !anything_below;

    uint64_t seq = next_seq_;
    const uint64_t target_bytes = options_.target_file_bytes;

    // Sources in priority order: the inputs newest first, then the level below,
    // which holds the oldest version of anything they share.
    std::vector<SSTableFilePtr> sources = inputs;
    sources.insert(sources.end(), overlap.begin(), overlap.end());

    lock.unlock();

    // Each source is read whole. At these file sizes that is a few megabytes;
    // an engine holding terabytes would stream them through a heap instead.
    struct Source {
        std::vector<Record> records;
        size_t pos = 0;
        bool done() const { return pos >= records.size(); }
        const Record& head() const { return records[pos]; }
    };
    std::vector<Source> readers;
    readers.reserve(sources.size());
    for (const auto& file : sources) readers.push_back(Source{file->reader.read_all(), 0});

    std::vector<std::pair<uint64_t, std::string>> written;
    std::vector<Record> buffer;
    uint64_t buffer_bytes = 0;
    bool failed = false;

    const auto emit = [&](bool force) {
        if (buffer.empty() || (!force && buffer_bytes < target_bytes)) return true;
        const std::string path = sst_path(seq);
        if (!SSTableWriter::write(path, buffer)) {
            std::cerr << "[NexusDB] Compaction output " << path << " failed to write\n";
            return false;
        }
        written.emplace_back(seq, path);
        seq++;
        buffer.clear();
        buffer_bytes = 0;
        return true;
    };

    for (;;) {
        const std::string* lowest = nullptr;
        for (const auto& source : readers) {
            if (source.done()) continue;
            if (lowest == nullptr || source.head().key < *lowest) lowest = &source.head().key;
        }
        if (lowest == nullptr) break;
        const std::string key = *lowest;

        // The first source holding this key is the newest, and every older copy
        // of it is dropped: that is what reclaims the space an overwrite costs.
        Record newest;
        bool have = false;
        for (auto& source : readers) {
            if (source.done() || source.head().key != key) continue;
            if (!have) {
                newest = source.head();
                have = true;
            }
            source.pos++;
        }

        if (newest.tombstone && drop_tombstones) continue;
        buffer_bytes += key.size() + newest.value.size() + 9;
        buffer.push_back(std::move(newest));
        if (!emit(false)) {
            failed = true;
            break;
        }
    }
    if (!failed && !emit(true)) failed = true;

    lock.lock();

    if (failed) {
        // Leave the tree exactly as it was. The outputs are not in the manifest,
        // so they are removed here and would be removed at the next open anyway.
        for (const auto& [output_seq, path] : written) {
            (void)output_seq;
            std::error_code ec;
            fs::remove(path, ec);
        }
        work_cv_.wait_for(lock, std::chrono::milliseconds(100));
        return;
    }

    const auto drop = [&](int level, const std::vector<SSTableFilePtr>& retire) {
        auto& files = levels_[static_cast<size_t>(level)];
        files.erase(std::remove_if(files.begin(), files.end(),
                                   [&](const SSTableFilePtr& f) {
                                       return std::find(retire.begin(), retire.end(), f) != retire.end();
                                   }),
                    files.end());
        for (const auto& file : retire) file->obsolete.store(true);
    };
    drop(from, inputs);
    drop(to, overlap);

    for (const auto& [output_seq, path] : written) {
        SSTableReader reader(path);
        if (!reader.ok()) {
            std::cerr << "[NexusDB] Compaction wrote " << path << " but cannot read it back\n";
            continue;
        }
        compacted_bytes_ += reader.file_bytes();
        levels_[static_cast<size_t>(to)].push_back(std::make_shared<SSTableFile>(std::move(reader), to, output_seq));
    }
    std::sort(levels_[static_cast<size_t>(to)].begin(), levels_[static_cast<size_t>(to)].end(),
              [](const SSTableFilePtr& a, const SSTableFilePtr& b) {
                  return a->reader.min_key() < b->reader.min_key();
              });

    next_seq_ = seq;
    compact_cursor_[static_cast<size_t>(from)] = hi;
    write_manifest_locked();
    compactions_++;

    std::cout << "[NexusDB] Compacted " << inputs.size() << " file(s) from L" << from << " and "
              << overlap.size() << " from L" << to << " into " << written.size() << " file(s)\n";
}

void NexusDB::wait_for_background() {
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (!options_.background) return;
    idle_cv_.wait(lock, [&] {
        return shutdown_ || (worker_idle_ && immutable_ == nullptr && !compaction_needed_locked());
    });
}

// ------------------------------------------------------------------ stats

size_t NexusDB::sstable_count() {
    std::shared_lock<std::shared_mutex> lock(mu_);
    size_t total = 0;
    for (const auto& level : levels_) total += level.size();
    return total;
}

size_t NexusDB::memtable_bytes() {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return active_->byte_size() + (immutable_ ? immutable_->byte_size() : 0);
}

int NexusDB::level_count() {
    std::shared_lock<std::shared_mutex> lock(mu_);
    int highest = 0;
    for (size_t level = 0; level < levels_.size(); level++)
        if (!levels_[level].empty()) highest = static_cast<int>(level);
    return highest + 1;
}

size_t NexusDB::files_in_level(int level) {
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (level < 0 || level >= static_cast<int>(levels_.size())) return 0;
    return levels_[static_cast<size_t>(level)].size();
}

std::vector<NexusDB::FileInfo> NexusDB::files() {
    std::shared_lock<std::shared_mutex> lock(mu_);
    std::vector<FileInfo> out;
    for (size_t level = 0; level < levels_.size(); level++) {
        for (const auto& file : levels_[level]) {
            out.push_back(FileInfo{fs::path(file->reader.path()).filename().string(), static_cast<int>(level),
                                   file->reader.file_bytes(), file->reader.record_count(),
                                   file->reader.tombstone_count(), file->reader.min_key(), file->reader.max_key()});
        }
    }
    return out;
}

uint64_t NexusDB::bytes_in_level(int level) {
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (level < 0 || level >= static_cast<int>(levels_.size())) return 0;
    uint64_t bytes = 0;
    for (const auto& file : levels_[static_cast<size_t>(level)]) bytes += file->reader.file_bytes();
    return bytes;
}
