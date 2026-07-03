#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/little_endian.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dftracer::utils::utilities::composites::dft::indexing {

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

inline std::size_t block_index(std::uint64_t h1, std::size_t num_blocks) {
    return static_cast<std::size_t>(
        (static_cast<__uint128_t>(h1) * num_blocks) >> 64);
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

std::size_t BloomFilter::optimal_num_bits(std::size_t n, double p) {
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

std::size_t BloomFilter::optimal_num_hashes(std::size_t m, std::size_t n) {
    if (n == 0) n = 1;
    auto k = static_cast<std::size_t>(std::round(
        static_cast<double>(m) / static_cast<double>(n) * std::log(2.0)));
    return std::max(k, static_cast<std::size_t>(1));
}

BloomFilter::BloomFilter(std::size_t expected_entries,
                         double false_positive_rate)
    : num_bits_(optimal_num_bits(expected_entries, false_positive_rate)),
      num_hashes_(optimal_num_hashes(num_bits_, expected_entries)),
      num_entries_(0) {
    bits_.assign(num_bits_ / 8, 0);
}

BloomFilter::BloomFilter(std::vector<unsigned char> bits, std::size_t num_bits,
                         std::size_t num_hashes, std::size_t num_entries)
    : bits_(std::move(bits)),
      num_bits_(num_bits),
      num_hashes_(num_hashes),
      num_entries_(num_entries) {}

BloomFilter BloomFilter::from_blob(const unsigned char* data,
                                   std::size_t size) {
    if (size < HEADER_SIZE) {
        throw DFTUtilsException(
            ErrorCode::PARSE,
            "BloomFilter::from_blob: data too small for header");
    }

    std::uint32_t num_hashes = read_u32_le(data);
    std::uint32_t num_entries = read_u32_le(data + 4);
    std::uint32_t num_bits = read_u32_le(data + 8);

    std::size_t bit_bytes = size - HEADER_SIZE;

    std::vector<unsigned char> bits(data + HEADER_SIZE,
                                    data + HEADER_SIZE + bit_bytes);

    return BloomFilter(std::move(bits), static_cast<std::size_t>(num_bits),
                       static_cast<std::size_t>(num_hashes),
                       static_cast<std::size_t>(num_entries));
}

void BloomFilter::compute_hashes(std::string_view value, std::uint64_t& h1,
                                 std::uint64_t& h2) const {
    hasher_.reset();
    hasher_.update(value);
    std::uint64_t raw = hasher_.get_hash().value;
    // FNV-1a leaves correlated high bits for similar short keys, which
    // breaks Lemire reduction in the blocked path. Run a SplitMix64-style
    // finisher to fully avalanche, then derive a second hash for masking.
    h1 = raw;
    h1 = (h1 ^ (h1 >> 30)) * 0xbf58476d1ce4e5b9ULL;
    h1 = (h1 ^ (h1 >> 27)) * 0x94d049bb133111ebULL;
    h1 ^= (h1 >> 31);
    h2 = raw + 0x9e3779b97f4a7c15ULL;
    h2 = (h2 ^ (h2 >> 30)) * 0xbf58476d1ce4e5b9ULL;
    h2 = (h2 ^ (h2 >> 27)) * 0x94d049bb133111ebULL;
    h2 ^= (h2 >> 31);
}

std::size_t BloomFilter::nth_hash(std::uint64_t h1, std::uint64_t h2,
                                  std::size_t n) const {
    return static_cast<std::size_t>((h1 + n * h2) % num_bits_);
}

void BloomFilter::add(std::string_view value) {
    if (last_value_valid_ && value.size() == last_value_size_ &&
        std::memcmp(last_value_buf_.data(), value.data(), value.size()) == 0) {
        ++num_entries_;
        return;
    }

    std::uint64_t h1, h2;
    compute_hashes(value, h1, h2);

    std::size_t num_blocks = num_bits_ / BLOCK_BITS;
    std::size_t blk = block_index(h1, num_blocks);
    auto* block =
        reinterpret_cast<std::uint32_t*>(bits_.data() + blk * BLOCK_BYTES);

    std::uint32_t mask[BLOCK_WORDS];
    compute_block_mask(h2, mask);
    for (std::size_t i = 0; i < BLOCK_WORDS; ++i) block[i] |= mask[i];
    ++num_entries_;

    if (value.size() <= LAST_VALUE_CAP) {
        std::memcpy(last_value_buf_.data(), value.data(), value.size());
        last_value_size_ = value.size();
        last_value_valid_ = true;
    } else {
        last_value_valid_ = false;
    }
}

bool BloomFilter::possibly_contains(std::string_view value) const {
    std::uint64_t h1, h2;
    compute_hashes(value, h1, h2);

    std::size_t num_blocks = num_bits_ / BLOCK_BITS;
    std::size_t blk = block_index(h1, num_blocks);
    const auto* block = reinterpret_cast<const std::uint32_t*>(
        bits_.data() + blk * BLOCK_BYTES);

    std::uint32_t mask[BLOCK_WORDS];
    compute_block_mask(h2, mask);
    for (std::size_t i = 0; i < BLOCK_WORDS; ++i) {
        if ((block[i] & mask[i]) != mask[i]) return false;
    }
    return true;
}

void BloomFilter::merge_from(const BloomFilter& other) {
    if (bits_.size() != other.bits_.size() || num_bits_ != other.num_bits_ ||
        num_hashes_ != other.num_hashes_) {
        throw DFTUtilsException(
            ErrorCode::INVALID_ARGUMENT,
            "BloomFilter::merge_from: incompatible filter parameters");
    }

    auto* dst = reinterpret_cast<std::uint64_t*>(bits_.data());
    const auto* src =
        reinterpret_cast<const std::uint64_t*>(other.bits_.data());
    std::size_t n = bits_.size() / 8;
    for (std::size_t i = 0; i < n; ++i) dst[i] |= src[i];
    num_entries_ += other.num_entries_;
}

std::vector<unsigned char> BloomFilter::serialize() const {
    std::vector<unsigned char> result;
    serialize_into(result);
    return result;
}

void BloomFilter::serialize_into(std::vector<unsigned char>& result) const {
    result.resize(HEADER_SIZE + bits_.size());
    write_u32_le(result.data(), static_cast<std::uint32_t>(num_hashes_));
    write_u32_le(result.data() + 4, static_cast<std::uint32_t>(num_entries_));
    write_u32_le(result.data() + 8, static_cast<std::uint32_t>(num_bits_));
    std::memcpy(result.data() + HEADER_SIZE, bits_.data(), bits_.size());
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing
