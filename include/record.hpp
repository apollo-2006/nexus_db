#pragma once
#include <cstdint>
#include <string>

// One version of one key, as it is held in memory and written to disk.
//
// A deletion is a record whose tombstone flag is set, not a value with a
// reserved shape. The engine used to store the literal string "@@TOMBSTONE@@",
// which a caller could write by accident and read back as a missing key.
struct Record {
    std::string key;
    std::string value;
    bool tombstone = false;
};

// Bit 0 of the flags byte that precedes every stored record.
inline constexpr uint8_t RECORD_TOMBSTONE = 1u << 0;
