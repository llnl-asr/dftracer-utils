// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>

#include <algorithm>
#include <bit>
#include <cstring>

namespace dftracer::utils::index::extensions {

namespace {
constexpr std::size_t MAGIC_SIZE = 4;
constexpr std::size_t CHAIN_HEADER_SIZE = 24;
constexpr std::size_t LEVEL_HEADER_SIZE = 12;
constexpr char MAGIC[MAGIC_SIZE] = {'S', 'B', 'F', '1'};

bool has_chain_magic(const unsigned char* data, std::size_t size) {
    return size >= CHAIN_HEADER_SIZE &&
           std::memcmp(data, MAGIC, MAGIC_SIZE) == 0;
}
}  // namespace

ScalableBloomFilter::ScalableBloomFilter(std::size_t initial_capacity,
                                         double false_positive_rate)
    : initial_capacity_(initial_capacity > 0 ? initial_capacity : 1),
      false_positive_rate_(false_positive_rate) {
    open_level(initial_capacity_);
}

void ScalableBloomFilter::open_level(std::size_t capacity) {
    // Each level targets a rate tightened by the growth step it represents,
    // so the sum over an unbounded chain converges.
    double rate = false_positive_rate_;
    for (std::size_t i = 0; i < levels_.size(); ++i) {
        rate *= TIGHTENING_RATIO;
    }
    levels_.push_back(Level{BloomFilter(capacity, rate), capacity, 0});
}

void ScalableBloomFilter::add(dstd::StringView value) {
    const BloomFilter::Hashes hashes = BloomFilter::hash(value);
    ++hashed_;
    for (const auto& level : levels_)
        if (level.filter.possibly_contains(hashes)) return;

    auto& active = levels_.back();
    active.filter.add(hashes);
    ++active.distinct;
    ++distinct_;

    if (active.distinct >= active.capacity) {
        open_level(active.capacity * GROWTH_FACTOR);
    }
}

bool ScalableBloomFilter::possibly_contains(dstd::StringView value) const {
    const BloomFilter::Hashes hashes = BloomFilter::hash(value);
    for (const auto& level : levels_) {
        if (level.filter.possibly_contains(hashes)) return true;
    }
    return false;
}

void ScalableBloomFilter::merge_from(const ScalableBloomFilter& other) {
    for (const auto& src : other.levels_) {
        auto it = std::find_if(
            levels_.begin(), levels_.end(), [&src](const Level& dst) {
                return dst.capacity == src.capacity &&
                       dst.filter.bit_count() == src.filter.bit_count() &&
                       dst.filter.hash_function_count() ==
                           src.filter.hash_function_count();
            });
        if (it != levels_.end()) {
            it->filter.merge_from(src.filter);
            it->distinct += src.distinct;
        } else {
            levels_.push_back(src);
        }
    }
    // Adds go to the last level, which must stay the largest so a merged-in
    // chain does not push new values into an already-full small level.
    std::sort(
        levels_.begin(), levels_.end(),
        [](const Level& a, const Level& b) { return a.capacity < b.capacity; });
    distinct_ += other.distinct_;

    if (levels_.back().distinct >= levels_.back().capacity) {
        open_level(levels_.back().capacity * GROWTH_FACTOR);
    }
}

std::size_t ScalableBloomFilter::size_bytes() const {
    std::size_t total = 0;
    for (const auto& level : levels_) total += level.filter.size_bytes();
    return total;
}

dstd::Vector<unsigned char> ScalableBloomFilter::serialize() const {
    dstd::Vector<unsigned char> result;
    serialize_into(result);
    return result;
}

void ScalableBloomFilter::serialize_into(
    dstd::Vector<unsigned char>& result) const {
    result.clear();
    result.insert(result.end(), MAGIC, MAGIC + MAGIC_SIZE);
    result.resize(CHAIN_HEADER_SIZE);
    dstd::write_u32_le(result.data() + MAGIC_SIZE,
                       static_cast<std::uint32_t>(levels_.size()));
    dstd::write_u64_le(result.data() + 8, initial_capacity_);
    dstd::write_u64_le(result.data() + 16,
                       std::bit_cast<std::uint64_t>(false_positive_rate_));

    dstd::Vector<unsigned char> blob;
    for (const auto& level : levels_) {
        level.filter.serialize_into(blob);
        const auto offset = result.size();
        result.resize(offset + LEVEL_HEADER_SIZE + blob.size());
        dstd::write_u64_le(result.data() + offset, level.capacity);
        dstd::write_u32_le(result.data() + offset + 8,
                           static_cast<std::uint32_t>(blob.size()));
        std::memcpy(result.data() + offset + LEVEL_HEADER_SIZE, blob.data(),
                    blob.size());
    }
}

ScalableBloomFilter ScalableBloomFilter::from_blob(const unsigned char* data,
                                                   std::size_t size) {
    if (!has_chain_magic(data, size)) {
        throw common::DFTUtilsException(
            common::ErrorCode::Parse,
            "ScalableBloomFilter::from_blob: not a filter chain");
    }

    ScalableBloomFilter result;
    result.levels_.clear();
    result.initial_capacity_ = dstd::read_u64_le(data + 8);
    result.false_positive_rate_ =
        std::bit_cast<double>(dstd::read_u64_le(data + 16));

    const std::uint32_t count = dstd::read_u32_le(data + MAGIC_SIZE);
    std::size_t cursor = CHAIN_HEADER_SIZE;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (cursor + LEVEL_HEADER_SIZE > size) {
            throw common::DFTUtilsException(
                common::ErrorCode::Parse,
                "ScalableBloomFilter::from_blob: truncated level header");
        }
        const std::size_t capacity = dstd::read_u64_le(data + cursor);
        const std::size_t blob_size = dstd::read_u32_le(data + cursor + 8);
        cursor += LEVEL_HEADER_SIZE;
        if (cursor + blob_size > size) {
            throw common::DFTUtilsException(
                common::ErrorCode::Parse,
                "ScalableBloomFilter::from_blob: truncated level payload");
        }
        auto filter = BloomFilter::from_blob(data + cursor, blob_size);
        cursor += blob_size;
        const auto entries = filter.entry_count();
        result.distinct_ += entries;
        result.levels_.push_back(Level{std::move(filter), capacity, entries});
    }

    if (result.levels_.empty()) result.open_level(result.initial_capacity_);
    return result;
}

}  // namespace dftracer::utils::index::extensions
