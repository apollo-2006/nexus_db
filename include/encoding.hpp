#pragma once
#include <cstdint>
#include <cstring>
#include <istream>
#include <ostream>
#include <string>

// Fixed-width little-endian encoding for everything the engine writes.
//
// The first format wrote size_t directly, so a file written by a 64-bit build
// could not be read by a 32-bit one, and the WebAssembly build is 32-bit. Lengths
// are u32 and offsets u64 on every target.

inline void put_u32(std::ostream& out, uint32_t v) {
    char b[4];
    for (int i = 0; i < 4; i++) b[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
    out.write(b, 4);
}

inline void put_u64(std::ostream& out, uint64_t v) {
    char b[8];
    for (int i = 0; i < 8; i++) b[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
    out.write(b, 8);
}

inline void put_u8(std::ostream& out, uint8_t v) { out.write(reinterpret_cast<const char*>(&v), 1); }

inline void put_bytes(std::ostream& out, const std::string& s) {
    put_u32(out, static_cast<uint32_t>(s.size()));
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

inline bool get_u32(std::istream& in, uint32_t& v) {
    unsigned char b[4];
    if (!in.read(reinterpret_cast<char*>(b), 4)) return false;
    v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(b[i]) << (8 * i);
    return true;
}

inline bool get_u64(std::istream& in, uint64_t& v) {
    unsigned char b[8];
    if (!in.read(reinterpret_cast<char*>(b), 8)) return false;
    v = 0;
    for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(b[i]) << (8 * i);
    return true;
}

inline bool get_u8(std::istream& in, uint8_t& v) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), 1));
}

// Reads a length-prefixed string, refusing a length that cannot fit in what is
// left of the file. A truncated or corrupt record otherwise asks for an
// allocation the size of whatever garbage the length field happened to hold.
inline bool get_bytes(std::istream& in, std::string& s, uint64_t bytes_left) {
    uint32_t len;
    if (!get_u32(in, len)) return false;
    if (len > bytes_left) return false;
    s.assign(len, '\0');
    return len == 0 || static_cast<bool>(in.read(&s[0], len));
}
