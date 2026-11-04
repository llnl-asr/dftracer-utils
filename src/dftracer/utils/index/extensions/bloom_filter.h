// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_BLOOM_FILTER_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_BLOOM_FILTER_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dftracer::utils::index::extensions {

/**
 * @brief Split block Bloom filter for approximate set membership testing.
 *
 * Implements the split block Bloom filter from the Apache Parquet spec:
 * 256-bit blocks of 8 x uint32 words; each insert/query touches exactly
 * one block (one cache line) and sets/tests one bit in each of the 8
 * words via a fixed SALT array. Block selection uses Lemire's reduction
 * on h1; in-block masks use h2 multiplied by SALT.
 *
 * References:
 *  - Apple, J. "Split block Bloom filters." arXiv:2101.01719 (2021).
 *  - Putze, F., Sanders, P., Singler, J. "Cache-, hash-, and space-
 *    efficient bloom filters." ACM JEA 14, Article 4 (2009).
 *  - Apache Parquet Bloom filter spec:
 *    https://github.com/apache/parquet-format/blob/master/BloomFilter.md
 *
 * Differs from canonical Parquet:
 *  - Underlying hash is XXH3-64 + SplitMix64 finisher (not xxhash64).
 *  - Custom 12-byte LE header (hash_count, entry_count, bit_count) instead
 *    of Thrift; hash_count is unused at insert/test (vestigial).
 *
 * Serialization format (self-describing):
 *   [4 bytes: hash_count (uint32_t LE)]
 *   [4 bytes: entry_count (uint32_t LE)]
 *   [4 bytes: bit_count   (uint32_t LE)]
 *   [remaining: bit array bytes]
 */
class BloomFilter {
   public:
    explicit BloomFilter(std::size_t expected_entries = 1024,
                         double false_positive_rate = 0.01);

    static BloomFilter from_blob(const unsigned char* data, std::size_t size);

    /// Depend on the value alone, so a caller that probes and then inserts
    /// computes them once.
    struct Hashes {
        std::uint64_t block;
        std::uint64_t mask;
    };
    static Hashes hash(dstd::StringView value);

    void add(dstd::StringView value) { add(hash(value)); }
    void add(const Hashes& hashes);
    bool possibly_contains(dstd::StringView value) const {
        return possibly_contains(hash(value));
    }
    bool possibly_contains(const Hashes& hashes) const;
    void merge_from(const BloomFilter& other);

    dstd::Vector<unsigned char> serialize() const;
    void serialize_into(dstd::Vector<unsigned char>& result) const;
    std::size_t entry_count() const { return entry_count_; }
    std::size_t size_bytes() const { return bits_.size(); }
    std::size_t hash_function_count() const { return hash_count_; }
    std::size_t bit_count() const { return bit_count_; }

    /// Bits a filter for `n` entries at rate `p` holds: a whole number of
    /// 512-bit blocks, at least one.
    static std::size_t optimal_bit_count(std::size_t n, double p);

   private:
    BloomFilter(dstd::Vector<unsigned char> bits, std::size_t bit_count,
                std::size_t hash_count, std::size_t entry_count);

    static std::size_t optimal_hash_count(std::size_t m, std::size_t n);

    dstd::Vector<unsigned char> bits_;
    std::size_t bit_count_;
    std::size_t hash_count_;
    std::size_t entry_count_;
};

}  // namespace dftracer::utils::index::extensions

#endif  // DFTRACER_UTILS_INDEX_EXTENSIONS_BLOOM_FILTER_H
