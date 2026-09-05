#pragma once
#include <string>
#include <fstream>
#include <vector>

class WriteAheadLog {
private:
    std::string filepath;
    std::ofstream log_stream;

public:
    WriteAheadLog(const std::string& path) : filepath(path) {
        log_stream.open(filepath, std::ios::app | std::ios::binary);
    }

    ~WriteAheadLog() {
        if (log_stream.is_open()) log_stream.close();
    }

    void append(const std::string& key, const std::string& value) {
        size_t k_len = key.size();
        size_t v_len = value.size();
        log_stream.write(reinterpret_cast<const char*>(&k_len), sizeof(size_t));
        log_stream.write(key.c_str(), k_len);
        log_stream.write(reinterpret_cast<const char*>(&v_len), sizeof(size_t));
        log_stream.write(value.c_str(), v_len);
        // Pushes the C++ stream buffer into the OS page cache. It is NOT an
        // fsync: the kernel may still hold these bytes when the machine loses
        // power, so this survives a process crash but not a hard power cut.
        log_stream.flush();
    }

    void clear() {
        log_stream.close();
        log_stream.open(filepath, std::ios::trunc | std::ios::binary);
    }
};