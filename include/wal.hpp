#pragma once
#include <fstream>
#include <string>

#include "encoding.hpp"
#include "record.hpp"

// The write-ahead log: every write is appended here before it reaches the
// memtable, so a crash loses nothing that the caller was told had been written.
//
// Records are [flags u8][klen u32][key][vlen u32][value], the same shape an
// SSTable stores, and a deletion is the same flag rather than a reserved value.
class WriteAheadLog {
private:
    std::string filepath;
    std::ofstream log_stream;

public:
    explicit WriteAheadLog(const std::string& path) : filepath(path) {
        log_stream.open(filepath, std::ios::app | std::ios::binary);
    }

    ~WriteAheadLog() {
        if (log_stream.is_open()) log_stream.close();
    }

    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;

    void append(const std::string& key, const std::string& value, bool tombstone) {
        put_u8(log_stream, tombstone ? RECORD_TOMBSTONE : 0);
        put_bytes(log_stream, key);
        put_bytes(log_stream, value);
        // Pushes the C++ stream buffer into the OS page cache. It is NOT an
        // fsync: the kernel may still hold these bytes when the machine loses
        // power, so this survives a process crash but not a hard power cut.
        log_stream.flush();
    }

    const std::string& path() const { return filepath; }

    // Called once the memtable holding these records is safely in an SSTable.
    void clear() {
        log_stream.close();
        log_stream.open(filepath, std::ios::trunc | std::ios::binary);
    }
};
