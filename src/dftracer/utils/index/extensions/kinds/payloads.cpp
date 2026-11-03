// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/hash/xxh3.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/store/internal/payload_codec.h>

namespace dftracer::utils::index::extensions::kinds {

namespace {
constexpr std::uint8_t HAS_PRESENT = 1;
constexpr std::uint8_t HAS_HISTOGRAM = 2;
}  // namespace

dstd::String encode_zone(const Zone& zone) {
    dstd::String out;
    index::store::internal::append_string(out, zone.value_type);
    index::store::internal::append_string(out, zone.min);
    index::store::internal::append_string(out, zone.max);
    dstd::append_u64_be(out, zone.observed);
    std::uint8_t flags = 0;
    if (zone.present) flags |= HAS_PRESENT;
    if (zone.histogram && !zone.histogram->empty()) flags |= HAS_HISTOGRAM;
    index::store::internal::append_u8(out, flags);
    if (zone.present) dstd::append_u64_be(out, *zone.present);
    if (flags & HAS_HISTOGRAM) {
        const auto hist = zone.histogram->serialize();
        index::store::internal::append_blob(out, hist);
    }
    return out;
}

dstd::Optional<Zone> decode_zone(dstd::StringView bytes) {
    try {
        index::store::internal::Cursor cursor(bytes);
        Zone zone;
        zone.value_type = cursor.str();
        zone.min = cursor.str();
        zone.max = cursor.str();
        zone.observed = cursor.u64();
        const auto flags = cursor.u8();
        if (flags & HAS_PRESENT) zone.present = cursor.u64();
        if (flags & HAS_HISTOGRAM) {
            const auto blob = cursor.blob_view();
            zone.histogram = dataframe::TimestampHistogram::deserialize(
                reinterpret_cast<const std::uint8_t*>(blob.data()),
                blob.size());
        }
        return zone;
    } catch (const std::exception&) {
        return dstd::nullopt;
    }
}

dstd::String encode_counts(const CountsGranule& counts) {
    dstd::String out;
    dstd::append_u64_be(out, counts.observed);
    index::store::internal::append_u8(out, counts.compressed ? 1 : 0);
    if (counts.compressed)
        index::store::internal::append_blob(out, *counts.compressed);
    return out;
}

dstd::Optional<CountsGranule> decode_counts(dstd::StringView bytes) {
    try {
        index::store::internal::Cursor cursor(bytes);
        CountsGranule counts;
        counts.observed = cursor.u64();
        if (cursor.u8() != 0) {
            const auto blob = cursor.blob_view();
            counts.compressed.emplace(blob.begin(), blob.end());
        }
        return counts;
    } catch (const std::exception&) {
        return dstd::nullopt;
    }
}

dstd::String encode_bloom(dstd::Span<const unsigned char> bloom,
                          std::uint64_t num_entries) {
    dstd::String out;
    dstd::append_u64_be(out, num_entries);
    out.append(reinterpret_cast<const char*>(bloom.data()), bloom.size());
    return out;
}

dstd::Optional<ScalableBloomFilter> decode_bloom(dstd::StringView bytes) {
    if (bytes.size() <= 8) return dstd::nullopt;
    try {
        return ScalableBloomFilter::from_blob(
            reinterpret_cast<const unsigned char*>(bytes.data() + 8),
            bytes.size() - 8);
    } catch (const std::exception&) {
        return dstd::nullopt;
    }
}

std::uint64_t value_hash(dstd::StringView value) {
    return dstd::hash::xxh3_64(value);
}

}  // namespace dftracer::utils::index::extensions::kinds
