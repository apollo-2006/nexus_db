#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// A Bloom filter over the keys of one SSTable.
//
// It answers "definitely not in this file" or "maybe", which is the answer a
// read needs: a maybe costs a seek and a short scan, a no costs nothing. At the
// default ten bits per key the false positive rate is about 1%, so a key that is
// not in a file is skipped without touching it ninety-nine times in a hundred.
//
// Hashing follows Kirsch and Mitzenmacher: two hashes generate k probes as
// h1 + i*h2, which is as accurate as k independent hashes and costs one pass
// over the key.
class BloomFilter {
public:
    static constexpr int DEFAULT_BITS_PER_KEY = 10;

    BloomFilter() = default;
    BloomFilter(std::vector<uint8_t> bits, uint64_t bit_count, uint32_t hash_count)
        : bits_(std::move(bits)), bit_count_(bit_count), hash_count_(hash_count) {}

    static BloomFilter build(const std::vector<std::string>& keys, int bits_per_key = DEFAULT_BITS_PER_KEY) {
        // An empty file still needs a filter that answers "no" to everything.
        const uint64_t bit_count = std::max<uint64_t>(64, keys.size() * static_cast<uint64_t>(bits_per_key));
        // k = (m/n) ln 2 minimises the false positive rate for a given size.
        const uint32_t hash_count =
            keys.empty() ? 1 : std::max(1u, std::min(30u, static_cast<uint32_t>(std::round(bits_per_key * 0.69314718))));

        BloomFilter f(std::vector<uint8_t>((bit_count + 7) / 8, 0), bit_count, hash_count);
        for (const auto& key : keys) f.add(key);
        return f;
    }

    void add(const std::string& key) {
        const uint64_t h1 = hash(key);
        const uint64_t h2 = (h1 >> 32) | 1;  // odd, so the probes walk the whole filter
        uint64_t probe = h1;
        for (uint32_t i = 0; i < hash_count_; i++) {
            const uint64_t bit = probe % bit_count_;
            bits_[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
            probe += h2;
        }
    }

    // False positives are possible, false negatives are not: a key that was added
    // always reads back as maybe.
    bool maybe_contains(const std::string& key) const {
        if (bit_count_ == 0) return true;  // no filter loaded: cannot rule anything out
        const uint64_t h1 = hash(key);
        const uint64_t h2 = (h1 >> 32) | 1;
        uint64_t probe = h1;
        for (uint32_t i = 0; i < hash_count_; i++) {
            const uint64_t bit = probe % bit_count_;
            if ((bits_[bit / 8] & (1u << (bit % 8))) == 0) return false;
            probe += h2;
        }
        return true;
    }

    const std::vector<uint8_t>& bytes() const { return bits_; }
    uint64_t bit_count() const { return bit_count_; }
    uint32_t hash_count() const { return hash_count_; }

private:
    // FNV-1a, 64-bit. Not a cryptographic hash and does not need to be: the cost
    // of a weak bit here is an occasional wasted seek, not a wrong answer.
    static uint64_t hash(const std::string& s) {
        uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    }

    std::vector<uint8_t> bits_;
    uint64_t bit_count_ = 0;
    uint32_t hash_count_ = 0;
};
