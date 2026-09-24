#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/store/internal/payload_codec.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>

namespace dftracer::utils::index::extensions::kinds {

namespace codec = index::store::internal;

namespace {
constexpr std::uint8_t HAS_PRESENT = 1;
constexpr std::uint8_t HAS_HISTOGRAM = 2;
}  // namespace

std::string encode_zone(const Zone& zone) {
    std::string out;
    codec::append_string(out, zone.value_type);
    codec::append_string(out, zone.min);
    codec::append_string(out, zone.max);
    codec::append_u64(out, zone.observed);
    std::uint8_t flags = 0;
    if (zone.present) flags |= HAS_PRESENT;
    if (zone.histogram && !zone.histogram->empty()) flags |= HAS_HISTOGRAM;
    codec::append_u8(out, flags);
    if (zone.present) codec::append_u64(out, *zone.present);
    if (flags & HAS_HISTOGRAM) {
        const auto hist = zone.histogram->serialize();
        codec::append_blob(out, hist);
    }
    return out;
}

std::optional<Zone> decode_zone(std::string_view bytes) {
    try {
        codec::Cursor cursor(bytes);
        Zone zone;
        zone.value_type = cursor.str();
        zone.min = cursor.str();
        zone.max = cursor.str();
        zone.observed = cursor.u64();
        const auto flags = cursor.u8();
        if (flags & HAS_PRESENT) zone.present = cursor.u64();
        if (flags & HAS_HISTOGRAM) {
            const auto blob = cursor.blob_view();
            zone.histogram =
                utilities::common::statistics::TimestampHistogram::deserialize(
                    reinterpret_cast<const std::uint8_t*>(blob.data()),
                    blob.size());
        }
        return zone;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string encode_counts(const CountsGranule& counts) {
    std::string out;
    codec::append_u64(out, counts.observed);
    codec::append_u8(out, counts.compressed ? 1 : 0);
    if (counts.compressed) codec::append_blob(out, *counts.compressed);
    return out;
}

std::optional<CountsGranule> decode_counts(std::string_view bytes) {
    try {
        codec::Cursor cursor(bytes);
        CountsGranule counts;
        counts.observed = cursor.u64();
        if (cursor.u8() != 0) {
            const auto blob = cursor.blob_view();
            counts.compressed.emplace(blob.begin(), blob.end());
        }
        return counts;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string encode_bloom(std::span<const unsigned char> bloom,
                         std::uint64_t num_entries) {
    std::string out;
    codec::append_u64(out, num_entries);
    out.append(reinterpret_cast<const char*>(bloom.data()), bloom.size());
    return out;
}

std::optional<ScalableBloomFilter> decode_bloom(std::string_view bytes) {
    if (bytes.size() <= 8) return std::nullopt;
    try {
        return ScalableBloomFilter::from_blob(
            reinterpret_cast<const unsigned char*>(bytes.data() + 8),
            bytes.size() - 8);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::uint64_t value_hash(std::string_view value) {
    return utilities::hash::fnv1a_hash(value);
}

}  // namespace dftracer::utils::index::extensions::kinds
