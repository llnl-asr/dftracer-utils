#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_FILTER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_FILTER_H

#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing {

/**
 * @brief Bloom filter for approximate set membership testing.
 *
 * Uses Kirsch-Mitzenmacher optimization: k hash functions derived from
 * 2 base hash values (std::hash with different seeds). Supports
 * serialization to/from binary blobs for RocksDB storage.
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
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_FILTER_H
