#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "bloom.hpp"
#include "encoding.hpp"
#include "record.hpp"

// A sorted string table: what a memtable becomes when it is flushed, and the
// unit compaction merges. Immutable once written.
//
// Layout, little-endian throughout:
//
//     magic         "NXSSTBL"   7 bytes
//     version       u8          3
//     count         u64         records in the file
//     index_offset  u64
//     bloom_offset  u64
//     bloom_bits    u64
//     bloom_hashes  u32
//     index_stride  u32         records between index entries
//     records       count * [flags u8][klen u32][key][vlen u32][value]
//     index         [entries u32] then entries * [klen u32][key][offset u64]
//     bloom         ceil(bloom_bits / 8) bytes
//
// Reading a file end to end is only for compaction. A point lookup asks the
// Bloom filter whether the key can be here at all, then seeks to the index entry
// that covers it and scans one stride of records.
namespace sst {

inline constexpr char MAGIC[7] = {'N', 'X', 'S', 'S', 'T', 'B', 'L'};
inline constexpr uint8_t VERSION = 3;
inline constexpr size_t HEADER_BYTES = sizeof(MAGIC) + 1 + 8 * 4 + 4 * 2;
inline constexpr uint32_t DEFAULT_INDEX_STRIDE = 16;

}  // namespace sst

// Writes one file. Records must arrive in key order with one version of each key.
class SSTableWriter {
public:
    static bool write(const std::string& path, const std::vector<Record>& records,
                      uint32_t index_stride = sst::DEFAULT_INDEX_STRIDE,
                      int bloom_bits_per_key = BloomFilter::DEFAULT_BITS_PER_KEY) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;

        // Header first with the offsets left blank: they are only known once the
        // records are down, and rewriting eight bytes beats buffering the file.
        write_header(out, records.size(), 0, 0, 0, 0, index_stride);

        std::vector<std::pair<std::string, uint64_t>> index;
        std::vector<std::string> keys;
        keys.reserve(records.size());

        for (size_t i = 0; i < records.size(); i++) {
            const uint64_t offset = static_cast<uint64_t>(out.tellp());
            // The first and last records are always indexed, so the index also
            // carries the key range of the file.
            if (i % index_stride == 0 || i + 1 == records.size()) index.emplace_back(records[i].key, offset);

            put_u8(out, records[i].tombstone ? RECORD_TOMBSTONE : 0);
            put_bytes(out, records[i].key);
            put_bytes(out, records[i].value);
            keys.push_back(records[i].key);
        }

        const uint64_t index_offset = static_cast<uint64_t>(out.tellp());
        put_u32(out, static_cast<uint32_t>(index.size()));
        for (const auto& [key, offset] : index) {
            put_bytes(out, key);
            put_u64(out, offset);
        }

        const BloomFilter bloom = BloomFilter::build(keys, bloom_bits_per_key);
        const uint64_t bloom_offset = static_cast<uint64_t>(out.tellp());
        out.write(reinterpret_cast<const char*>(bloom.bytes().data()),
                  static_cast<std::streamsize>(bloom.bytes().size()));

        out.seekp(0, std::ios::beg);
        write_header(out, records.size(), index_offset, bloom_offset, bloom.bit_count(),
                     bloom.hash_count(), index_stride);

        out.close();
        return static_cast<bool>(out);
    }

private:
    static void write_header(std::ostream& out, uint64_t count, uint64_t index_offset, uint64_t bloom_offset,
                             uint64_t bloom_bits, uint32_t bloom_hashes, uint32_t index_stride) {
        out.write(sst::MAGIC, sizeof(sst::MAGIC));
        put_u8(out, sst::VERSION);
        put_u64(out, count);
        put_u64(out, index_offset);
        put_u64(out, bloom_offset);
        put_u64(out, bloom_bits);
        put_u32(out, bloom_hashes);
        put_u32(out, index_stride);
    }
};

// An open file: its Bloom filter and sparse index are read once and kept, the
// records stay on disk. Immutable and therefore safe to share between readers.
class SSTableReader {
public:
    explicit SSTableReader(std::string path) : path_(std::move(path)) { load(); }

    bool ok() const { return ok_; }
    const std::string& path() const { return path_; }
    uint64_t record_count() const { return count_; }
    uint64_t file_bytes() const { return file_size_; }
    // Both empty when the file holds no records.
    const std::string& min_key() const { return min_key_; }
    const std::string& max_key() const { return max_key_; }

    // Whether this file could hold the key at all: key range first, since it is
    // free, then the Bloom filter.
    bool may_hold(const std::string& key) const {
        if (!ok_ || count_ == 0) return false;
        if (key < min_key_ || key > max_key_) return false;
        return bloom_.maybe_contains(key);
    }

    // The record for `key`, or nothing. A tombstone comes back like any other
    // record: only the caller knows whether an older file is still worth reading.
    std::optional<Record> get(const std::string& key) const {
        if (!may_hold(key)) return std::nullopt;

        std::ifstream in(path_, std::ios::binary);
        if (!in.is_open()) return std::nullopt;
        in.seekg(static_cast<std::streamoff>(block_offset(key)), std::ios::beg);

        // One stride of records at most, because the next index entry's key is
        // already above the target.
        while (static_cast<uint64_t>(in.tellg()) < index_offset_) {
            uint8_t flags;
            std::string found;
            if (!read_key(in, flags, found)) break;

            if (found == key) {
                Record r;
                r.key = std::move(found);
                r.tombstone = (flags & RECORD_TOMBSTONE) != 0;
                if (!get_bytes(in, r.value, bytes_left(in))) break;
                return r;
            }
            if (found > key) break;  // sorted: the key is not in this file

            // Step over the value without materialising it. ignore() walks the
            // stream's own buffer; a seek would discard it and turn every record
            // passed over into a fresh read syscall.
            uint32_t value_len;
            if (!get_u32(in, value_len)) break;
            in.ignore(static_cast<std::streamsize>(value_len));
        }
        return std::nullopt;
    }

    // Every record in key order. For compaction, and for tests.
    std::vector<Record> read_all() const {
        std::vector<Record> records;
        if (!ok_) return records;

        std::ifstream in(path_, std::ios::binary);
        if (!in.is_open()) return records;
        in.seekg(static_cast<std::streamoff>(sst::HEADER_BYTES), std::ios::beg);

        records.reserve(static_cast<size_t>(count_));
        for (uint64_t i = 0; i < count_; i++) {
            uint8_t flags;
            Record r;
            if (!read_key(in, flags, r.key)) break;
            if (!get_bytes(in, r.value, bytes_left(in))) break;
            r.tombstone = (flags & RECORD_TOMBSTONE) != 0;
            records.push_back(std::move(r));
        }
        return records;
    }

private:
    void load() {
        std::ifstream in(path_, std::ios::binary);
        if (!in.is_open()) return;

        in.seekg(0, std::ios::end);
        file_size_ = static_cast<uint64_t>(in.tellg());
        in.seekg(0, std::ios::beg);
        if (file_size_ < sst::HEADER_BYTES) return;

        // Anything this build does not recognise is refused rather than read as
        // lengths, which is what a file from an older format would be.
        char magic[sizeof(sst::MAGIC)];
        uint8_t version;
        if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, sst::MAGIC, sizeof(magic)) != 0) return;
        if (!get_u8(in, version) || version != sst::VERSION) return;

        uint64_t bloom_offset = 0, bloom_bits = 0;
        uint32_t bloom_hashes = 0, stride = 0;
        if (!get_u64(in, count_) || !get_u64(in, index_offset_) || !get_u64(in, bloom_offset) ||
            !get_u64(in, bloom_bits) || !get_u32(in, bloom_hashes) || !get_u32(in, stride))
            return;
        if (index_offset_ > file_size_ || bloom_offset > file_size_) return;

        in.seekg(static_cast<std::streamoff>(index_offset_), std::ios::beg);
        uint32_t entries;
        if (!get_u32(in, entries)) return;
        index_.reserve(entries);
        for (uint32_t i = 0; i < entries; i++) {
            std::string key;
            uint64_t offset;
            if (!get_bytes(in, key, bytes_left(in)) || !get_u64(in, offset)) return;
            index_.emplace_back(std::move(key), offset);
        }

        const uint64_t bloom_bytes = (bloom_bits + 7) / 8;
        if (bloom_offset + bloom_bytes > file_size_) return;
        std::vector<uint8_t> bits(static_cast<size_t>(bloom_bytes));
        in.seekg(static_cast<std::streamoff>(bloom_offset), std::ios::beg);
        if (bloom_bytes && !in.read(reinterpret_cast<char*>(bits.data()), static_cast<std::streamsize>(bloom_bytes)))
            return;
        bloom_ = BloomFilter(std::move(bits), bloom_bits, bloom_hashes);

        if (!index_.empty()) {
            min_key_ = index_.front().first;
            max_key_ = index_.back().first;
        }
        ok_ = true;
    }

    // The offset of the last indexed record whose key is not above the target.
    uint64_t block_offset(const std::string& key) const {
        auto it = std::upper_bound(index_.begin(), index_.end(), key,
                                   [](const std::string& k, const auto& entry) { return k < entry.first; });
        if (it == index_.begin()) return sst::HEADER_BYTES;
        return std::prev(it)->second;
    }

    uint64_t bytes_left(std::istream& in) const { return file_size_ - static_cast<uint64_t>(in.tellg()); }

    bool read_key(std::istream& in, uint8_t& flags, std::string& key) const {
        if (!get_u8(in, flags)) return false;
        return get_bytes(in, key, bytes_left(in));
    }

    std::string path_;
    bool ok_ = false;
    uint64_t count_ = 0;
    uint64_t file_size_ = 0;
    uint64_t index_offset_ = 0;
    std::vector<std::pair<std::string, uint64_t>> index_;
    BloomFilter bloom_;
    std::string min_key_, max_key_;
};
