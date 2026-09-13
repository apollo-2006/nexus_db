#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <optional>

class SSTable {
public:
    // Write sorted data sequentially to a binary file. Returns false if any
    // byte failed to reach the file (disk full, bad path, permissions).
    static bool write(const std::string& filepath, const std::vector<std::pair<std::string, std::string>>& data) {
        std::ofstream out(filepath, std::ios::binary | std::ios::trunc);
        for (const auto& pair : data) {
            size_t k_len = pair.first.size();
            size_t v_len = pair.second.size();
            out.write(reinterpret_cast<const char*>(&k_len), sizeof(size_t));
            out.write(pair.first.c_str(), k_len);
            out.write(reinterpret_cast<const char*>(&v_len), sizeof(size_t));
            out.write(pair.second.c_str(), v_len);
        }
        out.close();
        return static_cast<bool>(out);
    }

    // Linear scan for prototype (A production DB uses Bloom Filters and Index Blocks here)
    static std::optional<std::string> search(const std::string& filepath, const std::string& target_key) {
        std::ifstream in(filepath, std::ios::binary);
        if (!in.is_open()) return std::nullopt;

        // File size, taken once. Checking it per record with a seek to the end
        // and back cost four syscalls for every key the scan passed over.
        in.seekg(0, std::ios::end);
        const std::streamoff file_size = in.tellg();
        in.seekg(0, std::ios::beg);
        std::streamoff pos = 0;
        const auto remaining = [&] { return static_cast<size_t>(file_size - pos); };

        while (in.peek() != EOF) {
            size_t k_len, v_len;
            if (!in.read(reinterpret_cast<char*>(&k_len), sizeof(size_t))) break;
            pos += sizeof(size_t);

            // A truncated or corrupt file yields a garbage length, and
            // std::string(k_len, '\0') would then try to allocate it. Bound the
            // read against what is actually left in the file.
            if (k_len > remaining()) break;

            std::string key(k_len, '\0');
            if (!in.read(&key[0], k_len)) break;
            pos += k_len;

            if (!in.read(reinterpret_cast<char*>(&v_len), sizeof(size_t))) break;
            pos += sizeof(size_t);
            if (v_len > remaining()) break;

            if (key == target_key) {
                std::string val(v_len, '\0');
                if (!in.read(&val[0], v_len)) break;
                return val;
            } else {
                // Skip the value without materializing it. ignore() walks the
                // stream's buffer; seekg() would discard it and turn every
                // skipped record into a fresh read syscall.
                in.ignore(static_cast<std::streamsize>(v_len));
                pos += v_len;
            }
        }
        return std::nullopt;
    }
};