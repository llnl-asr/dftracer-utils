// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/hash/xxh3.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/extensions/bloom_filter.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dftracer::utils::trace::index::extensions {

namespace {
constexpr std::size_t HEADER_SIZE = 12;
constexpr std::size_t BLOCK_BYTES = 32;  // 8 x u32 = 256 bits
constexpr std::size_t BLOCK_BITS = BLOCK_BYTES * 8;
constexpr std::size_t BLOCK_WORDS = BLOCK_BYTES / 4;

// Split block Bloom filter SALT array, taken verbatim from the Apache
// Parquet spec (parquet-format/BloomFilter.md). Eight odd 32-bit
// constants; each (h2 * SALT[i]) >> 27 picks one of 32 bits in word i
// of the 256-bit block, with the 8 bit-selectors empirically
// uncorrelated. See Apple, "Split block Bloom filters", arXiv:2101.01719.
constexpr std::uint32_t SALT[BLOCK_WORDS] = {
    0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
    0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U,
};

inline std::size_t block_index(std::uint64_t h1, std::size_t block_count) {
    return static_cast<std::size_t>(
        (static_cast<__uint128_t>(h1) * block_count) >> 64);
}

inline void compute_block_mask(std::uint64_t h2,
                               std::uint32_t (&out)[BLOCK_WORDS]) {
    auto h32 = static_cast<std::uint32_t>(h2 ^ (h2 >> 32));
    for (std::size_t i = 0; i < BLOCK_WORDS; ++i) {
        std::uint32_t y = h32 * SALT[i];
        out[i] = 1U << (y >> 27);
    }
}
}  // namespace

std::size_t BloomFilter::optimal_bit_count(std::size_t n, double p) {
    if (n == 0) n = 1;
    if (p <= 0.0) p = 0.001;
    if (p >= 1.0) p = 0.5;
    auto m = static_cast<std::size_t>(
        std::ceil(-static_cast<double>(n) * std::log(p) /
                  (std::log(2.0) * std::log(2.0))));
    // Round up to a whole number of 512-bit blocks. Blocked bloom filters
    // pay ~10-15% extra memory for the same FPR vs classical; bump the
    // requested bit count to compensate before rounding.
    m = static_cast<std::size_t>(static_cast<double>(m) * 1.15);
    m = std::max(m, BLOCK_BITS);
    return ((m + BLOCK_BITS - 1) / BLOCK_BITS) * BLOCK_BITS;
}

std::size_t BloomFilter::optimal_hash_count(std::size_t m, std::size_t n) {
    if (n == 0) n = 1;
    auto k = static_cast<std::size_t>(std::round(
        static_cast<double>(m) / static_cast<double>(n) * std::log(2.0)));
    return std::max(k, static_cast<std::size_t>(1));
}

BloomFilter::BloomFilter(std::size_t expected_entries,
                         double false_positive_rate)
    : bit_count_(optimal_bit_count(expected_entries, false_positive_rate)),
      hash_count_(optimal_hash_count(bit_count_, expected_entries)),
      entry_count_(0) {
    bits_.assign(bit_count_ / 8, 0);
}

BloomFilter::BloomFilter(dstd::Vector<unsigned char> bits,
                         std::size_t bit_count, std::size_t hash_count,
                         std::size_t entry_count)
    : bits_(std::move(bits)),
      bit_count_(bit_count),
      hash_count_(hash_count),
      entry_count_(entry_count) {}

dstd::Result<BloomFilter> BloomFilter::from_blob(const unsigned char* data,
                                                 std::size_t size) {
    if (size < HEADER_SIZE) {
        return common::make_error(
            common::ErrorCode::Parse,
            "BloomFilter::from_blob: data too small for header");
    }

    std::uint32_t hash_count = dstd::read_u32_le(data);
    std::uint32_t entry_count = dstd::read_u32_le(data + 4);
    std::uint32_t bit_count = dstd::read_u32_le(data + 8);

    std::size_t bit_bytes = size - HEADER_SIZE;

    dstd::Vector<unsigned char> bits(data + HEADER_SIZE,
                                     data + HEADER_SIZE + bit_bytes);

    return BloomFilter(std::move(bits), static_cast<std::size_t>(bit_count),
                       static_cast<std::size_t>(hash_count),
                       static_cast<std::size_t>(entry_count));
}

BloomFilter::Hashes BloomFilter::hash(dstd::StringView value) {
    const std::uint64_t raw = dstd::hash::xxh3_64(value);
    // Run a SplitMix64-style finisher over the XXH3 hash, then derive a
    // second hash for masking.
    std::uint64_t h1 = raw;
    h1 = (h1 ^ (h1 >> 30)) * dstd::hash::SPLITMIX64_MUL1;
    h1 = (h1 ^ (h1 >> 27)) * dstd::hash::SPLITMIX64_MUL2;
    h1 ^= (h1 >> 31);
    std::uint64_t h2 = raw + dstd::hash::GOLDEN_RATIO;
    h2 = (h2 ^ (h2 >> 30)) * dstd::hash::SPLITMIX64_MUL1;
    h2 = (h2 ^ (h2 >> 27)) * dstd::hash::SPLITMIX64_MUL2;
    h2 ^= (h2 >> 31);
    return {h1, h2};
}

void BloomFilter::add(const Hashes& hashes) {
    std::size_t block_count = bit_count_ / BLOCK_BITS;
    std::size_t blk = block_index(hashes.block, block_count);
    auto* block =
        reinterpret_cast<std::uint32_t*>(bits_.data() + blk * BLOCK_BYTES);

    std::uint32_t mask[BLOCK_WORDS];
    compute_block_mask(hashes.mask, mask);
    for (std::size_t i = 0; i < BLOCK_WORDS; ++i) block[i] |= mask[i];
    ++entry_count_;
}

bool BloomFilter::possibly_contains(const Hashes& hashes) const {
    std::size_t block_count = bit_count_ / BLOCK_BITS;
    std::size_t blk = block_index(hashes.block, block_count);
    const auto* block = reinterpret_cast<const std::uint32_t*>(
        bits_.data() + blk * BLOCK_BYTES);

    std::uint32_t mask[BLOCK_WORDS];
    compute_block_mask(hashes.mask, mask);
    for (std::size_t i = 0; i < BLOCK_WORDS; ++i) {
        if ((block[i] & mask[i]) != mask[i]) return false;
    }
    return true;
}

void BloomFilter::merge_from(const BloomFilter& other) {
    if (bits_.size() != other.bits_.size() || bit_count_ != other.bit_count_ ||
        hash_count_ != other.hash_count_) {
        dstd::fatal("BloomFilter::merge_from: incompatible filter parameters");
    }

    auto* dst = reinterpret_cast<std::uint64_t*>(bits_.data());
    const auto* src =
        reinterpret_cast<const std::uint64_t*>(other.bits_.data());
    std::size_t n = bits_.size() / 8;
    for (std::size_t i = 0; i < n; ++i) dst[i] |= src[i];
    entry_count_ += other.entry_count_;
}

dstd::Vector<unsigned char> BloomFilter::serialize() const {
    dstd::Vector<unsigned char> result;
    serialize_into(result);
    return result;
}

void BloomFilter::serialize_into(dstd::Vector<unsigned char>& result) const {
    result.resize(HEADER_SIZE + bits_.size());
    dstd::write_u32_le(result.data(), static_cast<std::uint32_t>(hash_count_));
    dstd::write_u32_le(result.data() + 4,
                       static_cast<std::uint32_t>(entry_count_));
    dstd::write_u32_le(result.data() + 8,
                       static_cast<std::uint32_t>(bit_count_));
    std::memcpy(result.data() + HEADER_SIZE, bits_.data(), bits_.size());
}

}  // namespace dftracer::utils::trace::index::extensions
