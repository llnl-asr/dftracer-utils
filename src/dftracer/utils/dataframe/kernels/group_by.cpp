// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/numeric_dispatch.h>
#include <dftracer/utils/dataframe/internal/varwidth_offsets.h>
#include <dftracer/utils/dataframe/kernels/group_by.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstring>
#include <type_traits>

namespace {

using dftracer::utils::dataframe::byte_width;
using dftracer::utils::dataframe::Encoding;
using dftracer::utils::dataframe::is_wide_offset_type;
using dftracer::utils::dataframe::narrow_varwidth_type;
using dftracer::utils::dataframe::offsets_of;
using dftracer::utils::dataframe::TypeId;
using dftracer::utils::dataframe::writable;

bool is_valid(const dftu_series& v, std::int64_t i) {
    if (!v.validity.block()) return true;
    return (v.validity.data()[i >> 3] & (1u << (i & 7))) != 0;
}

template <class X>
constexpr TypeId typeid_of() {
    if constexpr (std::is_same_v<X, std::int8_t>)
        return TypeId::Int8;
    else if constexpr (std::is_same_v<X, std::int16_t>)
        return TypeId::Int16;
    else if constexpr (std::is_same_v<X, std::int32_t>)
        return TypeId::Int32;
    else if constexpr (std::is_same_v<X, std::int64_t>)
        return TypeId::Int64;
    else if constexpr (std::is_same_v<X, std::uint8_t>)
        return TypeId::Uint8;
    else if constexpr (std::is_same_v<X, std::uint16_t>)
        return TypeId::Uint16;
    else if constexpr (std::is_same_v<X, std::uint32_t>)
        return TypeId::Uint32;
    else if constexpr (std::is_same_v<X, std::uint64_t>)
        return TypeId::Uint64;
    else if constexpr (std::is_same_v<X, float>)
        return TypeId::Float32;
    else
        return TypeId::Float64;
}

template <class X>
dftu_series* flat_from(const dftracer::utils::dstd::Vector<X>& vals) {
    return dftu_series_new_flat(
        static_cast<dftu_dtype>(typeid_of<X>()), vals.data(),
        static_cast<std::int64_t>(vals.size()), nullptr);
}

struct Groups {
    dftracer::utils::dstd::Vector<std::int32_t> group_of;
    std::int32_t group_count = 0;
    dftu_series* keys = nullptr;
};

// String/Binary(/Large) key hashing at offset width `Off`: assigns each row
// its first-seen distinct-value group index.
template <class Off>
void hash_string_keys(
    const dftu_series& k, dftracer::utils::dstd::Vector<std::int32_t>& group_of,
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& distinct) {
    const Off* offsets =
        reinterpret_cast<const Off*>(offsets_of<Off>(k).data());
    const char* data = reinterpret_cast<const char*>(k.data.data());
    dftracer::utils::dstd::HashMap<dftracer::utils::dstd::String, std::int32_t>
        idx;
    for (std::int64_t i = 0; i < k.length; ++i) {
        dftracer::utils::dstd::String key(
            data + offsets[i],
            static_cast<std::size_t>(offsets[i + 1] - offsets[i]));
        auto [it, ins] = idx.try_emplace(
            std::move(key), static_cast<std::int32_t>(distinct.size()));
        if (ins) distinct.push_back(it->first);
        group_of[static_cast<std::size_t>(i)] = it->second;
    }
}

// Assign each row to a group by hashing the key's raw bytes (numeric value or
// string). Returns the assignment and a distinct-key column in first-seen
// order. A String/Binary key column's distinct-value output is always
// narrow: the group key set is bounded by the row count, a fresh sizing
// question independent of the source column's own offset width. Keys past
// INT32_MAX bytes return a Groups with null keys.
Groups build_groups(const dftu_series* keys_in) {
    const dftu_series* k = keys_in;
    dftu_series* materialized = nullptr;
    if (k->encoding != Encoding::Flat) {
        materialized = dftu_series_materialize(k);
        k = materialized;
    }

    const TypeId key_kind = narrow_varwidth_type(k->type);
    bool str = (key_kind == TypeId::String || key_kind == TypeId::Binary);
    std::size_t width =
        str ? 0 : byte_width(k->type, k->fixed_size()).value_or(0);

    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> distinct;
    Groups g;
    g.group_of.resize(static_cast<std::size_t>(k->length));
    if (str) {
        if (is_wide_offset_type(k->type))
            hash_string_keys<std::int64_t>(*k, g.group_of, distinct);
        else
            hash_string_keys<std::int32_t>(*k, g.group_of, distinct);
    } else {
        dftracer::utils::dstd::HashMap<dftracer::utils::dstd::String,
                                       std::int32_t>
            idx;
        const char* data = reinterpret_cast<const char*>(k->data.data());
        for (std::int64_t i = 0; i < k->length; ++i) {
            dftracer::utils::dstd::String key(
                data + static_cast<std::size_t>(i) * width, width);
            auto [it, ins] = idx.try_emplace(
                std::move(key), static_cast<std::int32_t>(distinct.size()));
            if (ins) distinct.push_back(it->first);
            g.group_of[static_cast<std::size_t>(i)] = it->second;
        }
    }
    g.group_count = static_cast<std::int32_t>(distinct.size());

    if (str) {
        dftracer::utils::dstd::Vector<std::int32_t> off{0};
        dftracer::utils::dstd::String bytes;
        for (const dftracer::utils::dstd::String& s : distinct) {
            bytes += s;
            if (!fits_int32_offsets(bytes.size())) {
                if (materialized) dftu_series_free(materialized);
                return Groups{};
            }
            off.push_back(static_cast<std::int32_t>(bytes.size()));
        }
        g.keys = dftu_series_new_string(static_cast<dftu_dtype>(key_kind),
                                        off.data(), bytes.data(), g.group_count,
                                        nullptr);
    } else if (k->type == TypeId::FixedSizeBinary) {
        // dftu_series_new_flat validates via byte_width(type), which is 0 for
        // FixedSizeBinary (its width is a DataType parameter, not a per-TypeId
        // constant); build the column directly instead.
        auto* out = new dftu_series();
        out->type = TypeId::FixedSizeBinary;
        out->encoding = Encoding::Flat;
        out->length = g.group_count;
        out->set_fixed_size(k->fixed_size());
        dftracer::utils::dstd::String bytes;
        for (const dftracer::utils::dstd::String& s : distinct) bytes += s;
        out->data = dftracer::utils::dataframe::new_buffer(bytes.size());
        std::memcpy(writable(out->data), bytes.data(), bytes.size());
        g.keys = out;
    } else {
        dftracer::utils::dstd::String bytes;
        for (const dftracer::utils::dstd::String& s : distinct) bytes += s;
        auto* out = new dftu_series();
        dftracer::utils::dataframe::adopt_type_from(*out, *k);
        out->encoding = Encoding::Flat;
        out->length = g.group_count;
        out->data = dftracer::utils::dataframe::new_buffer(bytes.size());
        if (!bytes.empty())
            std::memcpy(writable(out->data), bytes.data(), bytes.size());
        g.keys = out;
    }
    if (materialized) dftu_series_free(materialized);
    return g;
}

// Accumulate sum/count/min/max per group in one scan, then emit one column per
// set flag in flag order (sum, min, max, count, mean).
template <class T>
void aggregate(const dftu_series& v,
               const dftracer::utils::dstd::Vector<std::int32_t>& group_of,
               std::int32_t ng, std::int32_t op_mask, dftu_series** out,
               std::int32_t max_values, std::int32_t& out_n) {
    using W = std::conditional_t<
        std::is_floating_point_v<T>, double,
        std::conditional_t<std::is_unsigned_v<T>, std::uint64_t, std::int64_t>>;
    std::size_t g_count = static_cast<std::size_t>(ng);
    dftracer::utils::dstd::Vector<W> sums(g_count, W{});
    dftracer::utils::dstd::Vector<std::int64_t> cnts(g_count, 0);
    dftracer::utils::dstd::Vector<T> mins(g_count, T{});
    dftracer::utils::dstd::Vector<T> maxs(g_count, T{});
    const T* p = reinterpret_cast<const T*>(v.data.data());
    for (std::int64_t i = 0; i < v.length; ++i) {
        if (!is_valid(v, i)) continue;
        std::size_t g =
            static_cast<std::size_t>(group_of[static_cast<std::size_t>(i)]);
        if (cnts[g] == 0) {
            mins[g] = maxs[g] = p[i];
        } else {
            if (p[i] < mins[g]) mins[g] = p[i];
            if (p[i] > maxs[g]) maxs[g] = p[i];
        }
        sums[g] += static_cast<W>(p[i]);
        cnts[g]++;
    }

    out_n = 0;
    auto emit = [&](dftu_series* c) {
        if (out_n < max_values) out[out_n++] = c;
    };
    if (op_mask & DFTU_REDUCE_SUM) emit(flat_from(sums));
    if (op_mask & DFTU_REDUCE_MIN) emit(flat_from(mins));
    if (op_mask & DFTU_REDUCE_MAX) emit(flat_from(maxs));
    if (op_mask & DFTU_REDUCE_COUNT) emit(flat_from(cnts));
    if (op_mask & DFTU_REDUCE_MEAN) {
        dftracer::utils::dstd::Vector<double> means(g_count, 0.0);
        for (std::size_t g = 0; g < g_count; ++g)
            means[g] = cnts[g] ? static_cast<double>(sums[g]) /
                                     static_cast<double>(cnts[g])
                               : 0.0;
        emit(flat_from(means));
    }
}

}  // namespace

int32_t dftu_series_group_by(const dftu_series* keys, const dftu_series* values,
                             int32_t op_mask, dftu_series** out_keys,
                             dftu_series** out_values, int32_t max_values) {
    *out_keys = nullptr;
    if (keys->length != values->length) return 0;
    if (values->encoding != Encoding::Flat) {
        dftu_series* flat = dftu_series_materialize(values);
        if (!flat) return 0;
        const int32_t n = dftu_series_group_by(keys, flat, op_mask, out_keys,
                                               out_values, max_values);
        dftu_series_free(flat);
        return n;
    }
    if (values->type == TypeId::Bool || values->type == TypeId::String ||
        values->type == TypeId::Binary)
        return 0;
    if (!dftracer::utils::dataframe::is_orderable_type(keys->type)) {
        DFTRACER_UTILS_LOG_ERROR(
            "group_by: key type '{}' has no per-row value to group on",
            dftracer::utils::dataframe::type_name(keys->type));
        return 0;
    }

    Groups g = build_groups(keys);
    if (!g.keys) return 0;
    *out_keys = g.keys;
    std::int32_t n = 0;
    DFTRACER_UTILS_NUMERIC_DISPATCH(values->type, aggregate, *values,
                                    g.group_of, g.group_count, op_mask,
                                    out_values, max_values, n)
    return n;
}

namespace dftracer::utils::dataframe {

DataFrame group_by(const Series& keys, const Series& values, std::int32_t ops) {
    dftu_series* k = nullptr;
    dftu_series* vals[5] = {};
    std::int32_t n =
        dftu_series_group_by(keys.handle(), values.handle(), ops, &k, vals, 5);

    DataFrame b;
    b.names.push_back("key");
    b.columns.push_back(Series{k});
    struct Named {
        std::int32_t flag;
        const char* name;
    };
    static const Named order[] = {{DFTU_REDUCE_SUM, "sum"},
                                  {DFTU_REDUCE_MIN, "min"},
                                  {DFTU_REDUCE_MAX, "max"},
                                  {DFTU_REDUCE_COUNT, "count"},
                                  {DFTU_REDUCE_MEAN, "mean"}};
    std::int32_t idx = 0;
    for (const Named& o : order)
        if ((ops & o.flag) && idx < n) {
            b.names.push_back(o.name);
            b.columns.push_back(Series{vals[idx++]});
        }
    return b;
}

}  // namespace dftracer::utils::dataframe
