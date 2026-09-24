#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/little_endian.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>

#include <algorithm>
#include <cstring>

namespace dftracer::utils::index::extensions {

namespace {
constexpr std::size_t MAGIC_SIZE = 4;
constexpr std::size_t CHAIN_HEADER_SIZE = 8;
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

void ScalableBloomFilter::add(std::string_view value) {
    if (possibly_contains(value)) return;

    auto& active = levels_.back();
    active.filter.add(value);
    ++active.distinct;
    ++distinct_;

    if (active.distinct >= active.capacity) {
        open_level(active.capacity * GROWTH_FACTOR);
    }
}

bool ScalableBloomFilter::possibly_contains(std::string_view value) const {
    for (const auto& level : levels_) {
        if (level.filter.possibly_contains(value)) return true;
    }
    return false;
}

void ScalableBloomFilter::merge_from(const ScalableBloomFilter& other) {
    for (const auto& src : other.levels_) {
        auto it = std::find_if(
            levels_.begin(), levels_.end(), [&src](const Level& dst) {
                return dst.capacity == src.capacity &&
                       dst.filter.num_bits() == src.filter.num_bits() &&
                       dst.filter.num_hash_functions() ==
                           src.filter.num_hash_functions();
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

std::vector<unsigned char> ScalableBloomFilter::serialize() const {
    std::vector<unsigned char> result;
    serialize_into(result);
    return result;
}

void ScalableBloomFilter::serialize_into(
    std::vector<unsigned char>& result) const {
    result.clear();
    result.insert(result.end(), MAGIC, MAGIC + MAGIC_SIZE);
    result.resize(CHAIN_HEADER_SIZE);
    write_u32_le(result.data() + MAGIC_SIZE,
                 static_cast<std::uint32_t>(levels_.size()));

    std::vector<unsigned char> blob;
    for (const auto& level : levels_) {
        level.filter.serialize_into(blob);
        const auto offset = result.size();
        result.resize(offset + sizeof(std::uint32_t) + blob.size());
        write_u32_le(result.data() + offset,
                     static_cast<std::uint32_t>(blob.size()));
        std::memcpy(result.data() + offset + sizeof(std::uint32_t), blob.data(),
                    blob.size());
    }
}

ScalableBloomFilter ScalableBloomFilter::from_blob(const unsigned char* data,
                                                   std::size_t size) {
    ScalableBloomFilter result;
    result.levels_.clear();

    if (!has_chain_magic(data, size)) {
        auto legacy = BloomFilter::from_blob(data, size);
        const auto entries = legacy.num_entries();
        result.levels_.push_back(
            Level{std::move(legacy), entries > 0 ? entries : 1, entries});
        result.distinct_ = entries;
        return result;
    }

    const std::uint32_t count = read_u32_le(data + MAGIC_SIZE);
    std::size_t cursor = CHAIN_HEADER_SIZE;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (cursor + sizeof(std::uint32_t) > size) {
            throw DFTUtilsException(
                ErrorCode::PARSE,
                "ScalableBloomFilter::from_blob: truncated level header");
        }
        const std::size_t blob_size = read_u32_le(data + cursor);
        cursor += sizeof(std::uint32_t);
        if (cursor + blob_size > size) {
            throw DFTUtilsException(
                ErrorCode::PARSE,
                "ScalableBloomFilter::from_blob: truncated level payload");
        }
        auto filter = BloomFilter::from_blob(data + cursor, blob_size);
        cursor += blob_size;
        const auto entries = filter.num_entries();
        result.distinct_ += entries;
        result.levels_.push_back(
            Level{std::move(filter), entries > 0 ? entries : 1, entries});
    }

    if (result.levels_.empty()) result.open_level(result.initial_capacity_);
    return result;
}

}  // namespace dftracer::utils::index::extensions
