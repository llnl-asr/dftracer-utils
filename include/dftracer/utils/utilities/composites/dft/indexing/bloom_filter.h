#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_FILTER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_FILTER_H

#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing {

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
 *  - Underlying hash is FNV1a + SplitMix64 finisher (not xxhash64).
 *  - Custom 12-byte LE header (num_hashes, num_entries, num_bits) instead
 *    of Thrift; num_hashes is unused at insert/test (vestigial).
 *
 * Serialization format (self-describing):
 *   [4 bytes: num_hashes (uint32_t LE)]
 *   [4 bytes: num_entries (uint32_t LE)]
 *   [4 bytes: num_bits   (uint32_t LE)]
 *   [remaining: bit array bytes]
 */
class BloomFilter {
   public:
    explicit BloomFilter(std::size_t expected_entries = 1024,
                         double false_positive_rate = 0.01);

    static BloomFilter from_blob(const unsigned char* data, std::size_t size);

    void add(std::string_view value);
    bool possibly_contains(std::string_view value) const;
    void merge_from(const BloomFilter& other);

    std::vector<unsigned char> serialize() const;
    void serialize_into(std::vector<unsigned char>& result) const;
    std::size_t num_entries() const { return num_entries_; }
    std::size_t size_bytes() const { return bits_.size(); }
    std::size_t num_hash_functions() const { return num_hashes_; }
    std::size_t num_bits() const { return num_bits_; }

   private:
    BloomFilter(std::vector<unsigned char> bits, std::size_t num_bits,
                std::size_t num_hashes, std::size_t num_entries);

    void compute_hashes(std::string_view value, std::uint64_t& h1,
                        std::uint64_t& h2) const;
    std::size_t nth_hash(std::uint64_t h1, std::uint64_t h2,
                         std::size_t n) const;

    static std::size_t optimal_num_bits(std::size_t n, double p);
    static std::size_t optimal_num_hashes(std::size_t m, std::size_t n);

    std::vector<unsigned char> bits_;
    std::size_t num_bits_;
    std::size_t num_hashes_;
    std::size_t num_entries_;
    mutable hash::Fnv1aHasherUtility hasher_;

    static constexpr std::size_t LAST_VALUE_CAP = 64;
    std::array<char, LAST_VALUE_CAP> last_value_buf_{};
    std::size_t last_value_size_ = 0;
    bool last_value_valid_ = false;
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_FILTER_H
