#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace dftracer::utils::utilities::composites::dft::indexing {

namespace {
constexpr std::size_t HEADER_SIZE =
    12;  // 4 bytes num_hashes + 4 bytes num_entries + 4 bytes num_bits

void write_u32_le(unsigned char* buf, std::uint32_t val) {
    if (!buf) return;  // Defensive check to silence compiler warning
    buf[0] = static_cast<unsigned char>(val & 0xFF);
    buf[1] = static_cast<unsigned char>((val >> 8) & 0xFF);
    buf[2] = static_cast<unsigned char>((val >> 16) & 0xFF);
    buf[3] = static_cast<unsigned char>((val >> 24) & 0xFF);
}

std::uint32_t read_u32_le(const unsigned char* buf) {
    return static_cast<std::uint32_t>(buf[0]) |
           (static_cast<std::uint32_t>(buf[1]) << 8) |
           (static_cast<std::uint32_t>(buf[2]) << 16) |
           (static_cast<std::uint32_t>(buf[3]) << 24);
}
}  // namespace

std::size_t BloomFilter::optimal_num_bits(std::size_t n, double p) {
    if (n == 0) n = 1;
    if (p <= 0.0) p = 0.001;
    if (p >= 1.0) p = 0.5;
    auto m = static_cast<std::size_t>(
        std::ceil(-static_cast<double>(n) * std::log(p) /
                  (std::log(2.0) * std::log(2.0))));
    return std::max(m, static_cast<std::size_t>(64));
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
    std::size_t num_bytes = (num_bits_ + 7) / 8;
    bits_.resize(num_bytes, 0);
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
        throw std::runtime_error(
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
    h1 = hasher_.get_hash().value;
    // Second hash: mix with a different seed using FNV-like mixing
    std::uint64_t seed = 0x517cc1b727220a95ULL;
    h2 = h1 * seed + 0x9e3779b97f4a7c15ULL;
    h2 ^= (h2 >> 33);
    h2 *= 0xff51afd7ed558ccdULL;
    h2 ^= (h2 >> 33);
}

std::size_t BloomFilter::nth_hash(std::uint64_t h1, std::uint64_t h2,
                                  std::size_t n) const {
    // Kirsch-Mitzenmacher: g_i(x) = h1(x) + i * h2(x)
    return static_cast<std::size_t>((h1 + n * h2) % num_bits_);
}

void BloomFilter::add(std::string_view value) {
    std::uint64_t h1, h2;
    compute_hashes(value, h1, h2);

    for (std::size_t i = 0; i < num_hashes_; ++i) {
        std::size_t bit_pos = nth_hash(h1, h2, i);
        bits_[bit_pos / 8] |= static_cast<std::uint8_t>(1u << (bit_pos % 8));
    }
    ++num_entries_;
}

bool BloomFilter::possibly_contains(std::string_view value) const {
    std::uint64_t h1, h2;
    compute_hashes(value, h1, h2);

    for (std::size_t i = 0; i < num_hashes_; ++i) {
        std::size_t bit_pos = nth_hash(h1, h2, i);
        if (!(bits_[bit_pos / 8] & (1u << (bit_pos % 8)))) {
            return false;
        }
    }
    return true;
}

void BloomFilter::merge_from(const BloomFilter& other) {
    if (bits_.size() != other.bits_.size() || num_bits_ != other.num_bits_ ||
        num_hashes_ != other.num_hashes_) {
        throw std::runtime_error(
            "BloomFilter::merge_from: incompatible filter parameters");
    }

    for (std::size_t i = 0; i < bits_.size(); ++i) {
        bits_[i] |= other.bits_[i];
    }
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
