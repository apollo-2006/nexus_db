#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <optional>

class SSTable {
public:
    // Write sorted data sequentially to a binary file
    static void write(const std::string& filepath, const std::vector<std::pair<std::string, std::string>>& data) {
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
    }

    // Linear scan for prototype (A production DB uses Bloom Filters and Index Blocks here)
    static std::optional<std::string> search(const std::string& filepath, const std::string& target_key) {
        std::ifstream in(filepath, std::ios::binary);
        if (!in.is_open()) return std::nullopt;

        while (in.peek() != EOF) {
            size_t k_len, v_len;
            if (!in.read(reinterpret_cast<char*>(&k_len), sizeof(size_t))) break;

            std::string key(k_len, '\0');
            in.read(&key[0], k_len);

            in.read(reinterpret_cast<char*>(&v_len), sizeof(size_t));

            if (key == target_key) {
                std::string val(v_len, '\0');
                in.read(&val[0], v_len);
                return val;
            } else {
                // Skip the value to save memory parsing
                in.seekg(v_len, std::ios::cur);
            }
        }
        return std::nullopt;
    }
};