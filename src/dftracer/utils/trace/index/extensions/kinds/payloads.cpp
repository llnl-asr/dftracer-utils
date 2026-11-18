// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/extensions/kinds/payloads.h>
#include <dftracer/utils/trace/index/store/internal/payload_codec.h>

#include <bit>
#include <cmath>

namespace dftracer::utils::trace::index::extensions::kinds {

namespace {
constexpr std::uint8_t HAS_PRESENT = 1;
constexpr std::uint8_t HAS_HISTOGRAM = 2;
constexpr std::uint8_t MIN_INEXACT = 4;
constexpr std::uint8_t MAX_INEXACT = 8;
}  // namespace

dstd::String encode_zone(const Zone& zone) {
    dstd::String out;
    trace::index::store::internal::append_u8(out, ZONE_PAYLOAD_VERSION);
    trace::index::store::internal::append_u8(
        out, static_cast<std::uint8_t>(zone.type()));
    std::uint8_t flags = 0;
    if (zone.present) flags |= HAS_PRESENT;
    if (zone.histogram && !zone.histogram->empty()) flags |= HAS_HISTOGRAM;
    if (!zone.min_exact()) flags |= MIN_INEXACT;
    if (!zone.max_exact()) flags |= MAX_INEXACT;
    trace::index::store::internal::append_u8(out, flags);
    dstd::append_u64_be(out, zone.observed);
    dstd::append_u64_be(out, zone.null_count());
    dstd::append_u64_be(out, zone.nan_count());
    switch (zone.type()) {
        case BoundType::Int64:
            dstd::append_u64_be(out,
                                static_cast<std::uint64_t>(zone.min_raw_i64()));
            dstd::append_u64_be(out,
                                static_cast<std::uint64_t>(zone.max_raw_i64()));
            break;
        case BoundType::UInt64:
            dstd::append_u64_be(out, zone.min_raw_u64());
            dstd::append_u64_be(out, zone.max_raw_u64());
            break;
        case BoundType::Double:
            dstd::append_u64_be(
                out, std::bit_cast<std::uint64_t>(zone.min_raw_double()));
            dstd::append_u64_be(
                out, std::bit_cast<std::uint64_t>(zone.max_raw_double()));
            break;
        case BoundType::String:
            trace::index::store::internal::append_string(out,
                                                         zone.min_string());
            trace::index::store::internal::append_string(out,
                                                         zone.max_string());
            break;
        case BoundType::None:
        case BoundType::Mixed:
            break;
    }
    if (zone.present) dstd::append_u64_be(out, *zone.present);
    if (flags & HAS_HISTOGRAM) {
        const auto hist = zone.histogram->serialize();
        trace::index::store::internal::append_blob(out, hist);
    }
    return out;
}

dstd::Optional<Zone> decode_zone(dstd::StringView bytes) {
    trace::index::store::internal::Cursor cursor(bytes);
    if (cursor.u8() != ZONE_PAYLOAD_VERSION) return dstd::nullopt;
    const auto type = cursor.u8();
    if (type > static_cast<std::uint8_t>(BoundType::Mixed))
        return dstd::nullopt;
    const auto flags = cursor.u8();
    Zone zone;
    zone.observed = cursor.u64();
    const auto nulls = cursor.u64();
    const auto nans = cursor.u64();
    zone.assign(static_cast<BoundType>(type), 0, nulls, nans,
                (flags & MIN_INEXACT) == 0, (flags & MAX_INEXACT) == 0);
    switch (zone.type()) {
        case BoundType::Int64: {
            const auto lo = static_cast<std::int64_t>(cursor.u64());
            const auto hi = static_cast<std::int64_t>(cursor.u64());
            if (lo > hi) return dstd::nullopt;
            zone.set_min(lo);
            zone.set_max(hi);
            break;
        }
        case BoundType::UInt64: {
            const auto lo = cursor.u64();
            const auto hi = cursor.u64();
            if (lo > hi) return dstd::nullopt;
            zone.set_min(lo);
            zone.set_max(hi);
            break;
        }
        case BoundType::Double: {
            const auto lo = std::bit_cast<double>(cursor.u64());
            const auto hi = std::bit_cast<double>(cursor.u64());
            if (std::isnan(lo) || std::isnan(hi) || lo > hi)
                return dstd::nullopt;
            zone.set_min(lo);
            zone.set_max(hi);
            break;
        }
        case BoundType::String: {
            auto lo = cursor.str();
            auto hi = cursor.str();
            if (lo.compare(hi) > 0) return dstd::nullopt;
            zone.set_min(std::move(lo));
            zone.set_max(std::move(hi));
            break;
        }
        case BoundType::None:
        case BoundType::Mixed:
            break;
    }
    if (flags & HAS_PRESENT) zone.present = cursor.u64();
    if (flags & HAS_HISTOGRAM) {
        const auto blob = cursor.blob_view();
        zone.histogram = dataframe::TimestampHistogram::deserialize(
            reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size());
    }
    if (cursor.failed()) return dstd::nullopt;
    return zone;
}

dstd::String encode_counts(const CountsGranule& counts) {
    dstd::String out;
    dstd::append_u64_be(out, counts.observed);
    trace::index::store::internal::append_u8(out, counts.compressed ? 1 : 0);
    if (counts.compressed)
        trace::index::store::internal::append_blob(out, *counts.compressed);
    return out;
}

dstd::Optional<CountsGranule> decode_counts(dstd::StringView bytes) {
    trace::index::store::internal::Cursor cursor(bytes);
    CountsGranule counts;
    counts.observed = cursor.u64();
    if (cursor.u8() != 0) {
        const auto blob = cursor.blob_view();
        counts.compressed.emplace(blob.begin(), blob.end());
    }
    if (cursor.failed()) return dstd::nullopt;
    return counts;
}

dstd::String encode_bloom(dstd::Span<const unsigned char> bloom,
                          std::uint64_t entry_count) {
    dstd::String out;
    dstd::append_u64_be(out, entry_count);
    out.append(reinterpret_cast<const char*>(bloom.data()), bloom.size());
    return out;
}

dstd::Optional<ScalableBloomFilter> decode_bloom(dstd::StringView bytes) {
    if (bytes.size() <= 8) return dstd::nullopt;
    auto filter = ScalableBloomFilter::from_blob(
        reinterpret_cast<const unsigned char*>(bytes.data() + 8),
        bytes.size() - 8);
    if (!filter) return dstd::nullopt;
    return std::move(*filter);
}

}  // namespace dftracer::utils::trace::index::extensions::kinds
