// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/internal/float_key.h>
#include <dftracer/utils/dataframe/internal/numeric_dispatch.h>
#include <dftracer/utils/dataframe/internal/string_reader.h>
#include <dftracer/utils/dataframe/internal/varwidth_offsets.h>
#include <dftracer/utils/dataframe/kernels/group_by.h>
#include <dftracer/utils/dstd/bits.h>
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

using dftracer::utils::dataframe::own_valid;

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
dftu_series* flat_from(dftracer::utils::dstd::Span<const X> vals) {
    return dftu_series_new_flat(
        static_cast<dftu_dtype>(typeid_of<X>()), vals.data(),
        static_cast<std::int64_t>(vals.size()), nullptr);
}

struct Groups {
    dftracer::utils::dstd::Vector<std::int32_t> group_of;
    std::int32_t group_count = 0;
    dftu_series* keys = nullptr;
};

// Every null key is one group, first seen where its first row is; its key
// bytes are zeros under a null bit, whatever bytes the rows hold.
std::int32_t null_group_of(
    std::int32_t& null_group,
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& distinct,
    std::size_t width) {
    if (null_group < 0) {
        null_group = static_cast<std::int32_t>(distinct.size());
        distinct.push_back(dftracer::utils::dstd::String(width, '\0'));
    }
    return null_group;
}

// String/Binary(/Large) key hashing at offset width `Off`: assigns each row
// its first-seen distinct-value group index.
template <class Off>
std::int32_t hash_string_keys(
    const dftu_series& k, dftracer::utils::dstd::Vector<std::int32_t>& group_of,
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& distinct) {
    const Off* offsets =
        reinterpret_cast<const Off*>(offsets_of<Off>(k).data());
    const char* data = reinterpret_cast<const char*>(k.data.data());
    dftracer::utils::dstd::HashMap<dftracer::utils::dstd::String, std::int32_t>
        idx;
    std::int32_t null_group = -1;
    for (std::int64_t i = 0; i < k.length; ++i) {
        if (!own_valid(k, i)) {
            group_of[static_cast<std::size_t>(i)] =
                null_group_of(null_group, distinct, 0);
            continue;
        }
        dftracer::utils::dstd::String key(
            data + offsets[i],
            static_cast<std::size_t>(offsets[i + 1] - offsets[i]));
        auto [it, ins] = idx.try_emplace(
            std::move(key), static_cast<std::int32_t>(distinct.size()));
        if (ins) distinct.push_back(it->first);
        group_of[static_cast<std::size_t>(i)] = it->second;
    }
    return null_group;
}

// One range of key rows hashed on its own: the range's distinct keys in
// first-seen order, the order they were created in (-1 standing for the null
// group) and the range's own group id per row (-1 for a null row).
struct KeyRange {
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> distinct;
    dftracer::utils::dstd::Vector<std::int32_t> order;
    dftracer::utils::dstd::Vector<std::int32_t> group_of;
};

KeyRange scan_key_range(const dftu_series& k, std::int64_t from,
                        std::int64_t to, bool str, bool is_float, bool is_bool,
                        std::size_t width) {
    using dftracer::utils::dataframe::ReadPath;
    using dftracer::utils::dstd::HashMap;
    using dftracer::utils::dstd::String;
    using dftracer::utils::dstd::StringView;
    KeyRange r;
    r.group_of.resize(static_cast<std::size_t>(to - from), -1);
    HashMap<String, std::int32_t> idx;
    bool seen_null = false;
    auto add_null = [&](std::int64_t i) {
        if (!seen_null) {
            seen_null = true;
            r.order.push_back(-1);
        }
        r.group_of[static_cast<std::size_t>(i - from)] = -1;
    };
    auto add = [&](StringView s, std::int64_t i) {
        String key(s.data(), s.size());
        if (is_float) {
            dftracer::utils::dataframe::canonicalize_float(
                key.data(), static_cast<unsigned>(width));
        }
        auto [it, ins] = idx.try_emplace(
            std::move(key), static_cast<std::int32_t>(r.distinct.size()));
        if (ins) {
            r.distinct.push_back(it->first);
            r.order.push_back(it->second);
        }
        r.group_of[static_cast<std::size_t>(i - from)] = it->second;
    };
    StringView s;
    // The layer chain is resolved once here, so a row is one indexed load in
    // place of a find_leaf walk and a str_at walk over the same layers.
    const ReadPath path(&k);
    const dftu_series* leaf = path.leaf();
    if (!path.ok() || leaf == nullptr) {
        for (std::int64_t i = from; i < to; ++i) {
            if (!dftracer::utils::dataframe::key_bytes(k, i, str, is_bool,
                                                       width, s)) {
                add_null(i);
                continue;
            }
            add(s, i);
        }
        return r;
    }
    const std::uint8_t* leaf_data =
        leaf->data.block() ? leaf->data.data() : nullptr;
    const bool leaf_const = leaf->encoding == Encoding::Constant;
    for (std::int64_t i = from; i < to; ++i) {
        std::int64_t j = 0;
        const bool ok = path.locate(i, j);
        if (leaf_const) j = 0;
        bool have = ok && (str || leaf_data != nullptr);
        if (have) {
            if (str && leaf_const)
                have = dftracer::utils::dataframe::str_flat_at(leaf, 0, s);
            else if (str)
                have = dftracer::utils::dataframe::str_at(leaf, j, s);
            else
                s = dftracer::utils::dataframe::leaf_key(leaf_data, j, width,
                                                         is_bool);
        }
        if (!have) {
            add_null(i);
            continue;
        }
        add(s, i);
    }
    return r;
}

// Group each row of a key column in any layout by its key bytes, one range of
// rows per task and the ranges merged in row order, so the distinct values and
// the group ids match the row-by-row fold. The distinct values are copied once
// per range (bounded by the range's distinct keys): the group key set owns its
// bytes.
std::int32_t hash_key_cells(
    const dftu_series& k, bool str, bool is_float, bool is_bool,
    std::size_t width, dftracer::utils::dstd::Vector<std::int32_t>& group_of,
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& distinct) {
    using dftracer::utils::dstd::HashMap;
    using dftracer::utils::dstd::String;
    constexpr std::int64_t GRAIN = 32768;
    const std::int64_t n = k.length;
    if (n <= 0) return -1;
    const std::int64_t ranges = (n + GRAIN - 1) / GRAIN;
    dftracer::utils::dstd::Vector<KeyRange> parts(
        static_cast<std::size_t>(ranges));
    dftracer::utils::dataframe::parallel_for(
        n, GRAIN, [&](std::int64_t b, std::int64_t e) {
            parts[static_cast<std::size_t>(b / GRAIN)] =
                scan_key_range(k, b, e, str, is_float, is_bool, width);
        });
    HashMap<String, std::int32_t> idx;
    std::int32_t null_group = -1;
    for (std::int64_t p = 0; p < ranges; ++p) {
        const KeyRange& r = parts[static_cast<std::size_t>(p)];
        dftracer::utils::dstd::Vector<std::int32_t> remap(r.distinct.size());
        for (const std::int32_t local : r.order) {
            if (local < 0) {
                null_group = null_group_of(null_group, distinct, width);
                continue;
            }
            auto [it, ins] = idx.try_emplace(
                String(r.distinct[static_cast<std::size_t>(local)]),
                static_cast<std::int32_t>(distinct.size()));
            if (ins) distinct.push_back(it->first);
            remap[static_cast<std::size_t>(local)] = it->second;
        }
        const std::int64_t from = p * GRAIN;
        for (std::size_t i = 0; i < r.group_of.size(); ++i) {
            const std::int32_t local = r.group_of[i];
            group_of[static_cast<std::size_t>(from) + i] =
                local < 0 ? null_group : remap[static_cast<std::size_t>(local)];
        }
    }
    return null_group;
}

// Assign each row to a group by hashing the key's raw bytes (numeric value or
// string). Returns the assignment and a distinct-key column in first-seen
// order. A String/Binary key column's distinct-value output is always
// narrow: the group key set is bounded by the row count, a fresh sizing
// question independent of the source column's own offset width. Keys past
// INT32_MAX bytes return a Groups with null keys.
Groups build_groups(const dftu_series* k) {
    const TypeId key_kind = narrow_varwidth_type(k->type);
    const bool str = (key_kind == TypeId::String || key_kind == TypeId::Binary);
    const std::size_t width =
        str ? 0 : byte_width(k->type, k->fixed_size()).value_or(0);
    // A float keys as pandas and Polars key it: -0.0 is 0.0 and every NaN
    // is one value, so the key bytes are the canonical bits.
    const bool is_float = k->type == TypeId::Float16 ||
                          k->type == TypeId::Float32 ||
                          k->type == TypeId::Float64;
    const bool is_bool = k->type == TypeId::Bool;

    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> distinct;
    Groups g;
    g.group_of.resize(static_cast<std::size_t>(k->length));
    std::int32_t null_group = -1;
    if (k->encoding != Encoding::Flat) {
        null_group = hash_key_cells(*k, str, is_float, is_bool, width,
                                    g.group_of, distinct);
    } else if (str) {
        if (is_wide_offset_type(k->type))
            null_group =
                hash_string_keys<std::int64_t>(*k, g.group_of, distinct);
        else
            null_group =
                hash_string_keys<std::int32_t>(*k, g.group_of, distinct);
    } else {
        dftracer::utils::dstd::HashMap<dftracer::utils::dstd::String,
                                       std::int32_t>
            idx;
        const std::uint8_t* data = k->data.data();
        for (std::int64_t i = 0; i < k->length; ++i) {
            if (!own_valid(*k, i)) {
                g.group_of[static_cast<std::size_t>(i)] =
                    null_group_of(null_group, distinct, width);
                continue;
            }
            const auto kv =
                dftracer::utils::dataframe::leaf_key(data, i, width, is_bool);
            dftracer::utils::dstd::String key(kv.data(), kv.size());
            if (is_float)
                dftracer::utils::dataframe::canonicalize_float(
                    key.data(), static_cast<unsigned>(width));
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
            if (!fits_int32_offsets(bytes.size())) return Groups{};
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
        if (is_bool) {
            // A Bool column is bit-packed; the keys here are one byte each.
            bytes.assign(static_cast<std::size_t>(g.group_count + 7) / 8, '\0');
            for (std::size_t j = 0; j < distinct.size(); ++j)
                if (distinct[j][0] != '\0')
                    bytes[j >> 3] =
                        static_cast<char>(bytes[j >> 3] | (1u << (j & 7)));
        } else {
            for (const dftracer::utils::dstd::String& s : distinct) bytes += s;
        }
        auto* out = new dftu_series();
        dftracer::utils::dataframe::adopt_type_from(*out, *k);
        out->encoding = Encoding::Flat;
        out->length = g.group_count;
        out->data = dftracer::utils::dataframe::new_buffer(bytes.size());
        if (!bytes.empty())
            std::memcpy(writable(out->data), bytes.data(), bytes.size());
        g.keys = out;
    }
    if (null_group >= 0 && g.keys) {
        g.keys->validity = dftracer::utils::dataframe::new_buffer(
            static_cast<std::size_t>(g.group_count + 7) / 8);
        std::uint8_t* bits = writable(g.keys->validity);
        std::memset(bits, 0xff, g.keys->validity.size());
        bits[null_group >> 3] &=
            static_cast<std::uint8_t>(~(1u << (null_group & 7)));
        g.keys->null_count = 1;
    }
    return g;
}

// A group sum wraps in two's complement, so a partial that passes the range
// of W where the serial order does not still merges to the serial value.
template <class W>
inline W add_wrapping(W a, W b) {
    if constexpr (std::is_floating_point_v<W>) {
        return a + b;
    } else {
        W sum;
        dftracer::utils::dstd::bits::add_overflow(a, b, &sum);
        return sum;
    }
}

// One group's running aggregate: sum/count and the first-seen min and max.
template <class T, class W>
struct GroupAgg {
    dftracer::utils::dstd::Vector<W> sums;
    dftracer::utils::dstd::Vector<std::int64_t> cnts;
    dftracer::utils::dstd::Vector<T> mins;
    dftracer::utils::dstd::Vector<T> maxs;

    void reset(std::size_t ng) {
        sums.assign(ng, W{});
        cnts.assign(ng, 0);
        mins.assign(ng, T{});
        maxs.assign(ng, T{});
    }

    void add(std::size_t g, T x) {
        if (cnts[g] == 0) {
            mins[g] = maxs[g] = x;
        } else {
            if (x < mins[g]) mins[g] = x;
            if (x > maxs[g]) maxs[g] = x;
        }
        sums[g] = add_wrapping<W>(sums[g], static_cast<W>(x));
        cnts[g]++;
    }
};

// Fold rows [from, to) of `cells` into `acc`, one group per row.
template <class T, class W>
void fold_rows(const dftracer::utils::dataframe::ColumnCells<T>& cells,
               dftracer::utils::dstd::Span<const std::int32_t> group_of,
               std::int64_t from, std::int64_t to, GroupAgg<T, W>& acc) {
    for (std::int64_t i = from; i < to; ++i) {
        if (!cells.valid(i)) continue;
        const T x = cells.at(i);
        const std::size_t g =
            static_cast<std::size_t>(group_of[static_cast<std::size_t>(i)]);
        acc.add(g, x);
    }
}

// Per-range folds over gathered blocks, merged group by group: exact for an
// integer column, whose addition is associative, so it keeps the widths and the
// values of the row-by-row fold. A float column keeps the serial fold, whose
// rounding order the merge would change; false leaves the caller to run it.
template <class T, class W>
bool fold_rows_blocks(const dftu_series& v,
                      dftracer::utils::dstd::Span<const std::int32_t> group_of,
                      std::size_t g_count, GroupAgg<T, W>& acc) {
    using dftracer::utils::dataframe::each_block;
    using dftracer::utils::dataframe::RowBlock;
    if constexpr (std::is_floating_point_v<T>) {
        return false;
    } else {
        if (g_count == 0 || v.length <= 0) return false;
        const RowBlock<T> block(v);
        if (!block.usable()) return false;
        // Each range keeps a group-sized fold, so the fan-out is capped by the
        // group count the merge has to touch.
        const std::int64_t ranges =
            (v.length + dftracer::utils::dataframe::BLOCK_GRAIN<T> - 1) /
            dftracer::utils::dataframe::BLOCK_GRAIN<T>;
        if (ranges <= 1 ||
            static_cast<std::int64_t>(g_count) * ranges > (1 << 20))
            return false;
        GroupAgg<T, W> identity;
        identity.reset(g_count);
        acc = each_block<T>(
            block, 0, v.length, std::move(identity),
            [&](GroupAgg<T, W>& part, std::int64_t from, std::int64_t count,
                const T* buf, const std::uint8_t* valid, std::int64_t) {
                for (std::int64_t k = 0; k < count; ++k) {
                    if (((valid[k >> 3] >> (k & 7)) & 1) == 0) continue;
                    const T x = buf[k];
                    const std::size_t g = static_cast<std::size_t>(
                        group_of[static_cast<std::size_t>(from + k)]);
                    part.add(g, x);
                }
            },
            [g_count](GroupAgg<T, W> a, const GroupAgg<T, W>& b) {
                for (std::size_t g = 0; g < g_count; ++g) {
                    if (b.cnts[g] == 0) continue;
                    if (a.cnts[g] == 0) {
                        a.mins[g] = b.mins[g];
                        a.maxs[g] = b.maxs[g];
                    } else {
                        if (b.mins[g] < a.mins[g]) a.mins[g] = b.mins[g];
                        if (b.maxs[g] > a.maxs[g]) a.maxs[g] = b.maxs[g];
                    }
                    a.sums[g] = add_wrapping<W>(a.sums[g], b.sums[g]);
                    a.cnts[g] += b.cnts[g];
                }
                return a;
            });
        return true;
    }
}

// Accumulate sum/count/min/max per group in one scan, then emit one column per
// set flag in flag order (sum, min, max, count, mean).
template <class T>
void aggregate(const dftu_series& v,
               dftracer::utils::dstd::Span<const std::int32_t> group_of,
               std::int32_t ng, std::int32_t op_mask, dftu_series** out,
               std::int32_t max_values, std::int32_t& out_n) {
    using W = std::conditional_t<
        std::is_floating_point_v<T>, double,
        std::conditional_t<std::is_unsigned_v<T>, std::uint64_t, std::int64_t>>;
    std::size_t g_count = static_cast<std::size_t>(ng);
    GroupAgg<T, W> agg;
    agg.reset(g_count);
    const dftracer::utils::dataframe::ColumnCells<T> cells(v);
    // A non-FLAT values column reads through the layers, so its ranges are
    // folded in parallel; a FLAT one keeps the serial scan it always had.
    if (v.encoding == dftracer::utils::dataframe::Encoding::Flat ||
        !fold_rows_blocks<T, W>(v, group_of, g_count, agg))
        fold_rows<T, W>(cells, group_of, 0, v.length, agg);
    dftracer::utils::dstd::Vector<W>& sums = agg.sums;
    dftracer::utils::dstd::Vector<std::int64_t>& cnts = agg.cnts;
    dftracer::utils::dstd::Vector<T>& mins = agg.mins;
    dftracer::utils::dstd::Vector<T>& maxs = agg.maxs;

    out_n = 0;
    auto emit = [&](dftu_series* c) {
        if (out_n < max_values) out[out_n++] = c;
    };
    if (op_mask & DFTU_REDUCE_SUM) emit(flat_from<W>(sums));
    if (op_mask & DFTU_REDUCE_MIN) emit(flat_from<T>(mins));
    if (op_mask & DFTU_REDUCE_MAX) emit(flat_from<T>(maxs));
    if (op_mask & DFTU_REDUCE_COUNT) emit(flat_from<std::int64_t>(cnts));
    if (op_mask & DFTU_REDUCE_MEAN) {
        dftracer::utils::dstd::Vector<double> means(g_count, 0.0);
        for (std::size_t g = 0; g < g_count; ++g)
            means[g] = cnts[g] ? static_cast<double>(sums[g]) /
                                     static_cast<double>(cnts[g])
                               : 0.0;
        emit(flat_from<double>(means));
    }
}

}  // namespace

std::int32_t dftu_series_group_by(const dftu_series* keys,
                                  const dftu_series* values,
                                  std::int32_t op_mask, dftu_series** out_keys,
                                  dftu_series** out_values,
                                  std::int32_t max_values) {
    *out_keys = nullptr;
    if (keys->length != values->length) return 0;
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
    b.names.push_back(dstd::SharedString{"key"});
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
            b.names.push_back(dstd::SharedString{o.name});
            b.columns.push_back(Series{vals[idx++]});
        }
    return b;
}

}  // namespace dftracer::utils::dataframe
