// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/extensions/summary.h>

#include <algorithm>
#include <cstddef>

namespace dftracer::utils::trace::index::extensions {

namespace {

constexpr std::size_t BLOCK = 256;

// Rows of a flat column of `In` as `Out`, skipping nulls; returns the nulls.
template <class In, class Out, class Add>
std::uint64_t feed_numeric(const dataframe::Series& column, Add&& add) {
    const In* data = column.data<In>();
    const auto n = static_cast<std::size_t>(column.length());
    const bool has_nulls = column.null_count() > 0;
    if constexpr (std::is_same_v<In, Out>) {
        if (!has_nulls) {
            add(dstd::Span<const Out>(data, n));
            return 0;
        }
    }
    Out block[BLOCK];
    std::size_t k = 0;
    std::uint64_t nulls = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (has_nulls && column.is_null(static_cast<std::int64_t>(i))) {
            ++nulls;
            continue;
        }
        block[k++] = static_cast<Out>(data[i]);
        if (k == BLOCK) {
            add(dstd::Span<const Out>(block, k));
            k = 0;
        }
    }
    if (k != 0) add(dstd::Span<const Out>(block, k));
    return nulls;
}

}  // namespace

bool SummaryBuilder::add(const dataframe::Series& column) {
    using dataframe::TypeId;
    if (!column.valid()) return false;
    const TypeId type = column.type();
    const bool numeric = type == TypeId::Int8 || type == TypeId::Int16 ||
                         type == TypeId::Int32 || type == TypeId::Int64 ||
                         type == TypeId::Uint8 || type == TypeId::Uint16 ||
                         type == TypeId::Uint32 || type == TypeId::Uint64 ||
                         type == TypeId::Float32 || type == TypeId::Float64;
    if (numeric && !column.is_flat()) return add(column.materialize());
    std::uint64_t nulls = 0;
    const auto i64 = [this](dstd::Span<const std::int64_t> v) { add_i64(v); };
    const auto u64 = [this](dstd::Span<const std::uint64_t> v) { add_u64(v); };
    const auto f64 = [this](dstd::Span<const double> v) { add_f64(v); };
    switch (type) {
        case TypeId::Int8:
            nulls = feed_numeric<std::int8_t, std::int64_t>(column, i64);
            break;
        case TypeId::Int16:
            nulls = feed_numeric<std::int16_t, std::int64_t>(column, i64);
            break;
        case TypeId::Int32:
            nulls = feed_numeric<std::int32_t, std::int64_t>(column, i64);
            break;
        case TypeId::Int64:
            nulls = feed_numeric<std::int64_t, std::int64_t>(column, i64);
            break;
        case TypeId::Uint8:
            nulls = feed_numeric<std::uint8_t, std::uint64_t>(column, u64);
            break;
        case TypeId::Uint16:
            nulls = feed_numeric<std::uint16_t, std::uint64_t>(column, u64);
            break;
        case TypeId::Uint32:
            nulls = feed_numeric<std::uint32_t, std::uint64_t>(column, u64);
            break;
        case TypeId::Uint64:
            nulls = feed_numeric<std::uint64_t, std::uint64_t>(column, u64);
            break;
        case TypeId::Float32:
            nulls = feed_numeric<float, double>(column, f64);
            break;
        case TypeId::Float64:
            nulls = feed_numeric<double, double>(column, f64);
            break;
        case TypeId::String: {
            const auto n = column.length();
            const bool has_nulls = column.null_count() > 0;
            dstd::StringView block[BLOCK];
            std::size_t k = 0;
            for (std::int64_t i = 0; i < n; ++i) {
                if (has_nulls && column.is_null(i)) {
                    ++nulls;
                    continue;
                }
                block[k++] = column.string_at(i);
                if (k == BLOCK) {
                    add_str(dstd::Span<const dstd::StringView>(block, k));
                    k = 0;
                }
            }
            if (k != 0) add_str(dstd::Span<const dstd::StringView>(block, k));
            break;
        }
        default:
            return false;
    }
    if (nulls != 0) add_nulls(nulls);
    return true;
}

}  // namespace dftracer::utils::trace::index::extensions
