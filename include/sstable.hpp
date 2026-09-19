#pragma once
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "encoding.hpp"
#include "record.hpp"

// A sorted string table: the immutable file a memtable becomes when it is
// flushed, and the unit compaction merges.
//
// Layout, little-endian throughout:
//
//     magic    "NXSSTBL"     7 bytes
//     version  u8            2
//     count    u64           records in the file
//     records  count * [flags u8][klen u32][key][vlen u32][value]
//
// Records are written in key order with one version of each key, so a search can
// stop as soon as it passes the key it is looking for.
namespace sst {

inline constexpr char MAGIC[7] = {'N', 'X', 'S', 'S', 'T', 'B', 'L'};
inline constexpr uint8_t VERSION = 2;
inline constexpr size_t HEADER_BYTES = sizeof(MAGIC) + 1 + 8;

}  // namespace sst

class SSTable {
public:
    // Writes `records`, which must already be sorted by key. Returns false if any
    // byte failed to reach the file (disk full, bad path, permissions), leaving
    // the caller to keep whatever it was flushing.
    static bool write(const std::string& path, const std::vector<Record>& records) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;

        out.write(sst::MAGIC, sizeof(sst::MAGIC));
        put_u8(out, sst::VERSION);
        put_u64(out, records.size());

        for (const auto& r : records) {
            put_u8(out, r.tombstone ? RECORD_TOMBSTONE : 0);
            put_bytes(out, r.key);
            put_bytes(out, r.value);
        }

        out.close();
        return static_cast<bool>(out);
    }

    // The newest version of `key` in this file, or nothing if the file does not
    // hold it. A tombstone is returned like any other record: only the caller
    // knows whether an older file is still worth searching.
    static std::optional<Record> search(const std::string& path, const std::string& key) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return std::nullopt;

        in.seekg(0, std::ios::end);
        const uint64_t file_size = static_cast<uint64_t>(in.tellg());
        in.seekg(0, std::ios::beg);

        uint64_t count;
        if (!read_header(in, file_size, count)) return std::nullopt;

        for (uint64_t i = 0; i < count; i++) {
            uint8_t flags;
            std::string found;
            if (!read_key(in, file_size, flags, found)) break;

            if (found == key) {
                Record r;
                r.key = std::move(found);
                r.tombstone = (flags & RECORD_TOMBSTONE) != 0;
                if (!get_bytes(in, r.value, file_size - static_cast<uint64_t>(in.tellg()))) break;
                return r;
            }
            // Sorted file: everything past this point sorts higher than the key.
            if (found > key) break;

            // Step over the value without materialising it. ignore() walks the
            // stream's own buffer; a seek would discard it and turn every record
            // passed over into a fresh read syscall.
            uint32_t value_len;
            if (!get_u32(in, value_len)) break;
            in.ignore(static_cast<std::streamsize>(value_len));
        }
        return std::nullopt;
    }

    // Every record in key order, for compaction and for tests.
    static std::vector<Record> read_all(const std::string& path) {
        std::vector<Record> records;
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return records;

        in.seekg(0, std::ios::end);
        const uint64_t file_size = static_cast<uint64_t>(in.tellg());
        in.seekg(0, std::ios::beg);

        uint64_t count;
        if (!read_header(in, file_size, count)) return records;

        records.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; i++) {
            Record r;
            if (!read_record(in, file_size, r)) break;
            records.push_back(std::move(r));
        }
        return records;
    }

private:
    // Refuses anything this build does not recognise rather than reading whatever
    // the bytes happen to say. A file from the pre-versioning format has no magic
    // at all, and interpreting its first bytes as a length reads nonsense.
    static bool read_header(std::istream& in, uint64_t file_size, uint64_t& count) {
        if (file_size < sst::HEADER_BYTES) return false;

        char magic[sizeof(sst::MAGIC)];
        if (!in.read(magic, sizeof(magic))) return false;
        if (std::memcmp(magic, sst::MAGIC, sizeof(magic)) != 0) return false;

        uint8_t version;
        if (!get_u8(in, version) || version != sst::VERSION) return false;
        return get_u64(in, count);
    }

    static bool read_key(std::istream& in, uint64_t file_size, uint8_t& flags, std::string& key) {
        if (!get_u8(in, flags)) return false;
        return get_bytes(in, key, file_size - static_cast<uint64_t>(in.tellg()));
    }

    static bool read_record(std::istream& in, uint64_t file_size, Record& out) {
        uint8_t flags;
        if (!read_key(in, file_size, flags, out.key)) return false;
        if (!get_bytes(in, out.value, file_size - static_cast<uint64_t>(in.tellg()))) return false;
        out.tombstone = (flags & RECORD_TOMBSTONE) != 0;
        return true;
    }
};
