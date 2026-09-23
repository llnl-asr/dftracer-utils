#include <dftracer/utils/core/common/hash/fnv1a.h>       // string cell hashing
#include <dftracer/utils/core/common/hash/splitmix64.h>  // row/cell hashing
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/containment.h>
#include <dftracer/utils/dataframe/internal/cell_ops.h>  // shared cell helpers
#include <dftracer/utils/dataframe/internal/column_data.h>  // dftu_series, Buffer
#include <dftracer/utils/dataframe/internal/column_read.h>  // read_u64
#include <dftracer/utils/dataframe/internal/float16.h>
#include <dftracer/utils/dataframe/internal/radix_dedup.h>  // parallel dedup
#include <dftracer/utils/dataframe/kernels/field_stat.h>
#include <dftracer/utils/dataframe/kernels/filter.h>
#include <dftracer/utils/dataframe/kernels/group_by.h>
#include <dftracer/utils/dataframe/kernels/sort.h>
#include <dftracer/utils/dataframe/parallel.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::dataframe {

namespace {

std::int64_t index_of(const DataFrame& b, const std::string& name) {
    for (std::size_t i = 0; i < b.names.size(); ++i)
        if (b.names[i] == name) return static_cast<std::int64_t>(i);
    return -1;
}

AggOp agg_op_of(const std::string& op) {
#define DFTU_AGG_OP(id, code, str) \
    if (op == str) return AggOp::id;
#include <dftracer/utils/dataframe/agg_ops.def>
#undef DFTU_AGG_OP
    throw std::out_of_range("group_by: unknown aggregate op " + op);
}

}  // namespace

const char* to_string(Agg agg) noexcept {
    switch (agg) {
#define DFTU_AGG_OP(id, code, str) \
    case Agg::id:                  \
        return str;
#include <dftracer/utils/dataframe/agg_ops.def>
#undef DFTU_AGG_OP
    }
    return "count";
}

Agg agg_from_string(std::string_view name) {
#define DFTU_AGG_OP(id, code, str) \
    if (name == str) return Agg::id;
#include <dftracer/utils/dataframe/agg_ops.def>
#undef DFTU_AGG_OP
    throw std::out_of_range("agg_from_string: unknown aggregate op " +
                            std::string(name));
}

// One gather per column, each fanning out over its rows. The result is
// FLAT: a caller reads the buffers directly, and a frame that is filtered
// once and read many times pays the gather once.
DataFrame take(const DataFrame& b, const std::vector<std::int64_t>& indices) {
    DataFrame out;
    out.names = b.names;
    out.columns = take_all(b.columns, indices.data(),
                           static_cast<std::int64_t>(indices.size()));
    return out;
}

// The same by an Int64 index column (an argsort's output), read in place.
DataFrame take(const DataFrame& b, const Series& indices) {
    DataFrame out;
    out.names = b.names;
    out.columns =
        take_all(b.columns, indices.data<std::int64_t>(), indices.length());
    return out;
}

DataFrame filter(const DataFrame& b, const Series& mask) {
    return take(b, mask_index_column(mask.data<std::uint8_t>(), mask.length()));
}

DataFrame slice(const DataFrame& b, std::int64_t offset, std::int64_t len) {
    const std::int64_t nrows = b.num_rows();
    offset = std::clamp<std::int64_t>(offset, 0, nrows);
    len = std::clamp<std::int64_t>(len, 0, nrows - offset);
    if (offset == 0 && len == nrows) {
        DataFrame out;
        out.names = b.names;
        out.columns.reserve(b.columns.size());
        for (const Series& c : b.columns) out.columns.push_back(c.share());
        return out;
    }
    std::vector<std::int64_t> indices;
    indices.reserve(static_cast<std::size_t>(len));
    for (std::int64_t i = 0; i < len; ++i) indices.push_back(offset + i);
    return take(b, indices);
}

DataFrame head(const DataFrame& b, std::int64_t n) { return slice(b, 0, n); }

DataFrame select(const DataFrame& b, const std::vector<std::string>& names) {
    DataFrame out;
    out.names = names;
    out.columns.reserve(names.size());
    for (const std::string& name : names) {
        std::int64_t i = index_of(b, name);
        if (i < 0) throw std::out_of_range("select: no column named " + name);
        out.columns.push_back(b.columns[static_cast<std::size_t>(i)].share());
    }
    return out;
}

DataFrame rename(const DataFrame& b,
                 const std::vector<std::string>& new_names) {
    if (new_names.size() != b.columns.size())
        throw std::invalid_argument("rename: name count must match columns");
    DataFrame out;
    out.names = new_names;
    out.columns.reserve(b.columns.size());
    for (const Series& c : b.columns) out.columns.push_back(c.share());
    return out;
}

DataFrame with_column(const DataFrame& b, const std::string& name,
                      const Series& col) {
    DataFrame out;
    out.names = b.names;
    out.columns.reserve(b.columns.size() + 1);
    for (const Series& c : b.columns) out.columns.push_back(c.share());
    std::int64_t i = index_of(b, name);
    if (i >= 0) {
        out.columns[static_cast<std::size_t>(i)] = col.share();
    } else {
        out.names.push_back(name);
        out.columns.push_back(col.share());
    }
    return out;
}

DataFrame sort_by(const DataFrame& b, const std::string& name,
                  bool descending) {
    std::int64_t k = index_of(b, name);
    if (k < 0) throw std::out_of_range("sort_by: no column named " + name);
    Series order = argsort(b.columns[static_cast<std::size_t>(k)], descending);
    if (!order.valid())
        throw std::invalid_argument("sort_by: column " + name +
                                    " has no order (nested type)");
    return take(b, order);
}

DataFrame topk(const DataFrame& b, const std::string& name, std::int64_t k,
               bool largest) {
    std::int64_t j = index_of(b, name);
    if (j < 0) throw std::out_of_range("topk: no column named " + name);
    Series ind =
        topk_indices(b.columns[static_cast<std::size_t>(j)], k, largest);
    if (!ind.valid())
        throw std::invalid_argument("topk: column " + name +
                                    " has no order (nested type)");
    return take(b, ind);
}

Series concat_columns(const std::vector<const Series*>& parts) {
    if (parts.empty()) return Series{};
    // Materialize each part to FLAT so the value buffers are contiguous; concat
    // is a merge, so it copies (a single-buffer column can't alias many
    // inputs).
    std::vector<Series> mats;
    mats.reserve(parts.size());
    for (const Series* p : parts)
        mats.emplace_back(p->encoding() == Encoding::Flat
                              ? p->share()
                              : Series{dftu_series_materialize(p->handle())});

    const TypeId t = mats.front().type();
    std::int64_t total = 0;
    bool any_null = false;
    for (const Series& m : mats) {
        if (m.type() != t)
            throw std::invalid_argument("concat: columns must share a type");
        // Every other branch below has an offset-aware path (String) or
        // memcpy's byte_width(t) bytes per row; refuse anything with neither.
        // FixedSizeBinary also refuses here: dftu_series_new_flat below has no
        // fixed_size parameter to record on the result, so it cannot build one
        // even when the width itself is known.
        if (t != TypeId::String && t != TypeId::Bool && !byte_width(t))
            throw std::invalid_argument(std::string("concat: column type '") +
                                        type_name(t) + "' is unsupported");
        total += m.length();
        any_null |= m.null_count() > 0;
    }

    // Row offsets of the parts. A part that starts on a byte boundary and
    // is the last one or a whole number of bytes long copies its bitmaps
    // (validity, Bool data) byte-wise; only an unaligned part sets bits.
    std::vector<std::int64_t> starts(mats.size() + 1, 0);
    for (std::size_t i = 0; i < mats.size(); ++i)
        starts[i + 1] = starts[i] + mats[i].length();
    auto copy_bits = [&](std::uint8_t* dst, std::size_t part,
                         const std::uint8_t* src, bool src_set_means_one) {
        const std::int64_t off = starts[part];
        const std::int64_t len = mats[part].length();
        if (off % 8 == 0 && (len % 8 == 0 || part + 1 == mats.size())) {
            std::memcpy(dst + (off >> 3), src,
                        static_cast<std::size_t>((len + 7) / 8));
            return;
        }
        for (std::int64_t i = 0; i < len; ++i) {
            const bool on = (src[i >> 3] >> (i & 7)) & 1;
            const std::int64_t r = off + i;
            if (on == src_set_means_one)
                dst[r >> 3] |= static_cast<std::uint8_t>(1u << (r & 7));
            else
                dst[r >> 3] &= static_cast<std::uint8_t>(~(1u << (r & 7)));
        }
    };

    std::vector<std::uint8_t> validity;
    if (any_null) {
        validity.assign(static_cast<std::size_t>((total + 7) / 8), 0xFF);
        // Serial: two unaligned neighbours share a byte at their seam, so
        // the parts cannot write concurrently; a bitmap is small anyway.
        for (std::size_t i = 0; i < mats.size(); ++i) {
            const dftu_series* h = mats[i].handle();
            // No bitmap: every row present; an unaligned part leaves the
            // 0xFF fill as is.
            if (!h->validity) continue;
            copy_bits(validity.data(), i, h->validity->data(), true);
        }
    }
    const std::uint8_t* vptr = any_null ? validity.data() : nullptr;

    if (t == TypeId::String) {
        std::string data;
        std::vector<std::int32_t> offs(static_cast<std::size_t>(total) + 1, 0);
        std::int64_t r = 0;
        for (const Series& m : mats)
            for (std::int64_t i = 0; i < m.length(); ++i) {
                if (!m.is_null(i)) data += m.string_at(i);
                offs[static_cast<std::size_t>(r) + 1] =
                    static_cast<std::int32_t>(data.size());
                ++r;
            }
        return Series{
            dftu_series_new_string(static_cast<dftu_dtype>(TypeId::String),
                                   offs.data(), data.data(), total, vptr)};
    }

    // The value buffer is built in place, one part per task, and adopted by
    // the result rather than copied into it a second time.
    auto* out = new dftu_series();
    dftracer::utils::dataframe::adopt_type_from(*out, *mats.front().handle());
    out->encoding = Encoding::Flat;
    out->length = total;
    if (vptr) {
        out->validity = Buffer::allocate(validity.size());
        std::memcpy(out->validity->data(), vptr, validity.size());
        std::int64_t nulls = 0;
        for (const Series& m : mats) nulls += m.null_count();
        out->null_count = nulls;
    }

    if (t == TypeId::Bool) {
        out->data = Buffer::allocate(buffer_bytes(TypeId::Bool, total));
        std::memset(out->data->data(), 0, out->data->size());
        std::uint8_t* bits = out->data->data();
        for (std::size_t i = 0; i < mats.size(); ++i)
            copy_bits(bits, i, mats[i].data<std::uint8_t>(), true);
        return Series{out};
    }

    const std::size_t w = byte_width(t).value_or(0);
    out->data = Buffer::allocate(static_cast<std::size_t>(total) * w);
    std::uint8_t* buf = out->data->data();
    parallel_for(
        static_cast<std::int64_t>(mats.size()), 1,
        [&](std::int64_t b, std::int64_t e) {
            for (std::int64_t i = b; i < e; ++i) {
                const Series& m = mats[static_cast<std::size_t>(i)];
                const std::size_t bytes =
                    static_cast<std::size_t>(m.length()) * w;
                if (bytes)
                    std::memcpy(buf + static_cast<std::size_t>(
                                          starts[static_cast<std::size_t>(i)]) *
                                          w,
                                m.data<std::uint8_t>(), bytes);
            }
        });
    return Series{out};
}

DataFrame group_by(const DataFrame& b, const std::vector<std::string>& keys,
                   const std::vector<GroupAgg>& aggs) {
    std::vector<const Series*> key_cols;
    key_cols.reserve(keys.size());
    for (const std::string& key : keys) {
        std::int64_t ki = index_of(b, key);
        if (ki < 0) throw std::out_of_range("group_by: no column named " + key);
        key_cols.push_back(&b.columns[static_cast<std::size_t>(ki)]);
    }

    // Lower to the fused aggregation engine: value columns referenced by index
    // (deduped), all aggregates computed in one parallel pass over the batch.
    std::vector<const Series*> values;
    std::map<std::string, std::int32_t> col_idx;
    auto resolve = [&](const std::string& name) -> std::int32_t {
        auto it = col_idx.find(name);
        if (it != col_idx.end()) return it->second;
        std::int64_t vi = index_of(b, name);
        if (vi < 0)
            throw std::out_of_range("group_by: no column named " + name);
        const std::int32_t idx = static_cast<std::int32_t>(values.size());
        values.push_back(&b.columns[static_cast<std::size_t>(vi)]);
        col_idx.emplace(name, idx);
        return idx;
    };
    std::vector<AggSpec> specs;
    specs.reserve(aggs.size());
    for (const GroupAgg& a : aggs) {
        AggSpec sp;
        sp.op = to_agg_op(a.op);
        sp.out = a.out;
        sp.param = a.param;
        sp.value_col = sp.op == AggOp::Count ? -1 : resolve(a.column);
        if (agg_uses_by_col(sp.op)) sp.by_col = resolve(a.by);
        specs.push_back(std::move(sp));
    }
    return group_agg(key_cols, values, std::move(specs), keys);
}

DataFrame group_by(const DataFrame& b, const std::string& key,
                   const std::vector<GroupAgg>& aggs) {
    return group_by(b, std::vector<std::string>{key}, aggs);
}

namespace {

using dftracer::utils::hash::splitmix64;

std::uint64_t read_f64_bits(const Series& c, std::int64_t i) {
    if (c.type() == TypeId::Float32)
        return std::bit_cast<std::uint32_t>(c.data<float>()[i]);
    return std::bit_cast<std::uint64_t>(c.data<double>()[i]);
}

// A stable hash of one cell, so the same value hashes identically on every
// node.
std::uint64_t hash_cell(const Series& c, std::int64_t i) {
    switch (c.type()) {
        case TypeId::String:
            return hash::fnv1a_hash(c.string_at(i));
        case TypeId::Bool: {
            const std::uint8_t* b = c.data<std::uint8_t>();
            return splitmix64((b[i >> 3] >> (i & 7)) & 1);
        }
        case TypeId::Float32:
        case TypeId::Float64:
            return splitmix64(static_cast<std::uint64_t>(read_f64_bits(c, i)));
        default:
            return splitmix64(read_u64(c, i));
    }
}

}  // namespace

Series partition_id(const DataFrame& b, const std::vector<std::string>& keys,
                    std::int64_t n_parts) {
    if (keys.empty()) throw std::invalid_argument("partition_id: no keys");
    if (n_parts < 1) throw std::invalid_argument("partition_id: n_parts < 1");
    const std::int64_t n = b.num_rows();

    std::vector<const Series*> key_cols;
    for (const std::string& k : keys) {
        std::int64_t ki = index_of(b, k);
        if (ki < 0)
            throw std::out_of_range("partition_id: no column named " + k);
        key_cols.push_back(&b.columns[static_cast<std::size_t>(ki)]);
    }

    // Bucket id per row is independent of every other row, so it fans out
    // through parallel_for; the scatter into per-bucket index lists then runs
    // as one pass per thread over private buckets, merged at the end (rows
    // within a bucket need not keep their original order - only co-location).
    std::vector<std::int32_t> bucket_of(static_cast<std::size_t>(n));
    constexpr std::int64_t GRAIN = 1 << 15;
    auto compute_bucket = [&](std::int64_t b0, std::int64_t e0) {
        for (std::int64_t i = b0; i < e0; ++i) {
            std::uint64_t h = dftracer::utils::hash::FNV1A_OFFSET_BASIS_LEGACY;
            for (const Series* c : key_cols)
                h = splitmix64(h ^ hash_cell(*c, i));
            bucket_of[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(
                h % static_cast<std::uint64_t>(n_parts));
        }
    };
    if (parallel_backend_installed() && n > GRAIN) {
        parallel_for(n, GRAIN, compute_bucket);
    } else {
        compute_bucket(0, n);
    }
    return Series::flat(TypeId::Int32, bucket_of.data(), n);
}

std::vector<DataFrame> hash_partition(const DataFrame& b,
                                      const std::vector<std::string>& keys,
                                      std::int64_t n_parts) {
    Series ids = partition_id(b, keys, n_parts);
    const std::int64_t n = b.num_rows();
    const std::int32_t* bucket_of = ids.data<std::int32_t>();

    std::vector<std::vector<std::int64_t>> buckets(
        static_cast<std::size_t>(n_parts));
    for (std::int64_t i = 0; i < n; ++i)
        buckets[static_cast<std::size_t>(bucket_of[i])].push_back(i);

    std::vector<DataFrame> parts;
    parts.reserve(static_cast<std::size_t>(n_parts));
    for (auto& idx : buckets) parts.push_back(take(b, idx));
    return parts;
}

namespace {

// One row of a scalar (numeric/bool) column rendered as text, matching
// build_row_frame's own stringification of numbers.
std::string scalar_cell_to_string(const Series& s, std::int64_t i) {
    switch (s.type()) {
        case TypeId::Bool:
            return s.data<std::uint8_t>()
                       ? (((s.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1)
                              ? "true"
                              : "false")
                       : "false";
        case TypeId::Int8:
            return std::to_string(s.data<std::int8_t>()[i]);
        case TypeId::Int16:
            return std::to_string(s.data<std::int16_t>()[i]);
        case TypeId::Int32:
            return std::to_string(s.data<std::int32_t>()[i]);
        case TypeId::Int64:
            return std::to_string(s.data<std::int64_t>()[i]);
        case TypeId::Uint8:
            return std::to_string(s.data<std::uint8_t>()[i]);
        case TypeId::Uint16:
            return std::to_string(s.data<std::uint16_t>()[i]);
        case TypeId::Uint32:
            return std::to_string(s.data<std::uint32_t>()[i]);
        case TypeId::Uint64:
            return std::to_string(s.data<std::uint64_t>()[i]);
        case TypeId::Float32:
            return std::to_string(s.data<float>()[i]);
        case TypeId::Float64:
            return std::to_string(s.data<double>()[i]);
        case TypeId::String:
        case TypeId::Binary:
            return std::string(s.string_at(i));
        case TypeId::Float16:
            return std::to_string(half_to_float(s.data<std::uint16_t>()[i]));
        case TypeId::Date32:
        case TypeId::Time32:
            return std::to_string(s.data<std::int32_t>()[i]);
        case TypeId::Date64:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
            return std::to_string(s.data<std::int64_t>()[i]);
        case TypeId::List:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Struct:
        case TypeId::Map:
        case TypeId::Decimal128:
        case TypeId::Decimal256:
        case TypeId::FixedSizeBinary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
            return std::string();
        case TypeId::Unknown:
            break;  // schema-only marker, never a real Series' type
    }
    return std::string();
}

// Render a scalar/bool column as a String column, preserving per-row validity.
// Diagonal concat uses this where a column's type varies across parts and one
// part is String: dftu_series_cast has no numeric->String path, so the numeric
// parts are stringified here (numbers as text) to unify the column on String.
Series to_string_series(const Series& s) {
    const std::int64_t n = s.length();
    std::vector<std::string> owned(static_cast<std::size_t>(n));
    std::vector<std::string_view> vals(static_cast<std::size_t>(n));
    std::vector<std::uint8_t> vbits(static_cast<std::size_t>((n + 7) / 8), 0);
    bool any_null = false;
    for (std::int64_t i = 0; i < n; ++i) {
        if (s.is_null(i)) {
            any_null = true;
            continue;
        }
        owned[static_cast<std::size_t>(i)] = scalar_cell_to_string(s, i);
        vals[static_cast<std::size_t>(i)] = owned[static_cast<std::size_t>(i)];
        vbits[static_cast<std::size_t>(i >> 3)] |=
            static_cast<std::uint8_t>(1u << (i & 7));
    }
    return Series::strings(std::span<const std::string_view>(vals),
                           any_null ? vbits.data() : nullptr);
}

bool is_numeric_type(TypeId t) {
    return t != TypeId::String && t != TypeId::Binary && t != TypeId::Bool &&
           t != TypeId::List && t != TypeId::Struct;
}

// The type the same-named column takes across parts. Same type stays; a mix of
// numeric types widens to Float64; a scalar String/number clash unifies on
// String; any other clash (nested vs scalar) has no common type and throws.
TypeId promote_type(TypeId a, TypeId b, const std::string& name) {
    if (a == b) return a;
    if (is_numeric_type(a) && is_numeric_type(b)) return TypeId::Float64;
    // A scalar arg whose value is a string in one part and a number in another
    // (build_row_frame infers a column's type per batch) unifies on String,
    // numbers stringified, rather than aborting the whole concat.
    const bool a_scalar = a != TypeId::List && a != TypeId::Struct;
    const bool b_scalar = b != TypeId::List && b != TypeId::Struct;
    if ((a == TypeId::String && b_scalar) || (b == TypeId::String && a_scalar))
        return TypeId::String;
    throw std::invalid_argument("concat(diagonal): column '" + name +
                                "' has incompatible types across parts");
}

DataFrame concat_diagonal(const std::vector<const DataFrame*>& parts) {
    // Column order = first appearance; track each column's promoted type.
    std::vector<std::string> names;
    std::vector<TypeId> types;
    std::unordered_map<std::string, std::size_t> idx;
    for (const DataFrame* p : parts)
        for (std::size_t c = 0; c < p->names.size(); ++c) {
            const std::string& nm = p->names[c];
            const TypeId t = p->columns[c].type();
            auto it = idx.find(nm);
            if (it == idx.end()) {
                idx.emplace(nm, names.size());
                names.push_back(nm);
                types.push_back(t);
            } else {
                types[it->second] = promote_type(types[it->second], t, nm);
            }
        }

    DataFrame out;
    out.names = names;
    out.columns.reserve(names.size());
    for (std::size_t c = 0; c < names.size(); ++c) {
        const TypeId target = types[c];
        // Hold cast/null-filled parts alive while concat_columns reads them.
        std::vector<Series> owned;
        owned.reserve(parts.size());
        std::vector<const Series*> cols;
        cols.reserve(parts.size());
        for (const DataFrame* p : parts) {
            std::int64_t at = -1;
            for (std::size_t j = 0; j < p->names.size(); ++j)
                if (p->names[j] == names[c]) {
                    at = static_cast<std::int64_t>(j);
                    break;
                }
            if (at < 0) {
                owned.push_back(Series::nulls(target, p->num_rows()));
            } else {
                const Series& src = p->columns[static_cast<std::size_t>(at)];
                if (src.type() == target)
                    owned.push_back(src.share());
                else if (target == TypeId::String)
                    owned.push_back(to_string_series(src));
                else
                    owned.push_back(src.cast(target));
            }
            cols.push_back(&owned.back());
        }
        out.columns.push_back(concat_columns(cols));
    }
    return out;
}

}  // namespace

DataFrame concat(const std::vector<const DataFrame*>& parts, ConcatHow how) {
    DataFrame out;
    if (parts.empty()) return out;
    // A single part is the identity: share its columns zero-copy, no merge.
    if (parts.size() == 1) {
        out.names = parts.front()->names;
        out.columns.reserve(parts.front()->columns.size());
        for (const Series& c : parts.front()->columns)
            out.columns.push_back(c.share());
        return out;
    }
    if (how == ConcatHow::Diagonal) return concat_diagonal(parts);
    const DataFrame& first = *parts.front();
    out.names = first.names;
    const std::size_t ncols = first.names.size();
    for (const DataFrame* p : parts)
        if (p->names != first.names)
            throw std::invalid_argument("concat: batches must share a schema");
    out.columns.reserve(ncols);
    for (std::size_t c = 0; c < ncols; ++c) {
        std::vector<const Series*> cols;
        cols.reserve(parts.size());
        for (const DataFrame* p : parts) cols.push_back(&p->columns[c]);
        out.columns.push_back(concat_columns(cols));
    }
    return out;
}

namespace {

// A FLAT copy of every column (sharing already-FLAT ones), so cell accessors
// below can read the value buffers directly.
std::vector<Series> materialized_columns(const DataFrame& b) {
    std::vector<Series> cols;
    cols.reserve(b.columns.size());
    for (const Series& c : b.columns) {
        if (c.encoding() == Encoding::Flat)
            cols.push_back(c.share());
        else
            cols.emplace_back(dftu_series_materialize(c.handle()));
    }
    return cols;
}

// Composite dedupe key per row over `cols` (already FLAT), built in parallel
// (each row's key is independent scalar string work).
std::vector<std::string> row_keys(const std::vector<Series>& cols,
                                  std::int64_t n) {
    std::vector<std::string> keys(static_cast<std::size_t>(n));
    parallel_for(n, std::int64_t{1} << 13, [&](std::int64_t b, std::int64_t e) {
        for (std::int64_t i = b; i < e; ++i)
            keys[static_cast<std::size_t>(i)] = row_key(cols, i);
    });
    return keys;
}

Series row_mask(const DataFrame& b, bool want_unique) {
    const std::int64_t n = b.num_rows();
    std::vector<Series> cols = materialized_columns(b);
    std::vector<std::string> keys = row_keys(cols, n);
    std::vector<std::int64_t> counts = radix_key_counts(keys, n);
    const std::int64_t nbytes = (n + 7) / 8;
    std::vector<std::uint8_t> bits(static_cast<std::size_t>(nbytes), 0);
    parallel_for(
        nbytes, std::int64_t{1} << 10, [&](std::int64_t bb, std::int64_t be) {
            for (std::int64_t byte = bb; byte < be; ++byte) {
                const std::int64_t base = byte * 8;
                const std::int64_t lim = std::min(base + 8, n);
                std::uint8_t v = 0;
                for (std::int64_t i = base; i < lim; ++i) {
                    bool unique = counts[static_cast<std::size_t>(i)] == 1;
                    if (want_unique == unique)
                        v |= static_cast<std::uint8_t>(1u << (i - base));
                }
                bits[static_cast<std::size_t>(byte)] = v;
            }
        });
    return Series{dftu_series_new_flat(static_cast<dftu_dtype>(TypeId::Bool),
                                       bits.data(), n, nullptr)};
}

}  // namespace

DataFrame drop_nulls(const DataFrame& b) {
    const std::int64_t n = b.num_rows();
    std::vector<std::int64_t> idx;
    for (std::int64_t i = 0; i < n; ++i) {
        bool keep = true;
        for (const Series& c : b.columns)
            if (c.is_null(i)) {
                keep = false;
                break;
            }
        if (keep) idx.push_back(i);
    }
    return take(b, idx);
}

DataFrame fill_null(const DataFrame& b, dftu_scalar value) {
    DataFrame out;
    out.names = b.names;
    out.columns.reserve(b.columns.size());
    for (const Series& c : b.columns) {
        Series f = c.fillna(value);
        out.columns.push_back(f.valid() ? std::move(f) : c.share());
    }
    return out;
}

DataFrame unique(const DataFrame& b, const std::vector<std::string>& subset) {
    const std::int64_t n = b.num_rows();
    std::vector<Series> cols;
    if (subset.empty()) {
        cols = materialized_columns(b);
    } else {
        cols.reserve(subset.size());
        for (const std::string& s : subset) {
            std::int64_t k = index_of(b, s);
            if (k < 0) throw std::out_of_range("unique: no column named " + s);
            cols.push_back(
                b.columns[static_cast<std::size_t>(k)].materialize());
        }
    }
    std::vector<std::string> keys = row_keys(cols, n);
    std::vector<std::uint8_t> keep = radix_first_seen_mask(keys, n);
    std::vector<std::int64_t> idx;
    idx.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        if (keep[static_cast<std::size_t>(i)]) idx.push_back(i);
    return take(b, idx);
}

namespace {

// Parallel stable sort of `order` by `less`: chunks are stable_sort'd in
// parallel, then merged bottom-up (std::merge is itself stable), so the
// overall result matches a single std::stable_sort over the whole range.
// Falls back to one std::stable_sort when no backend is installed or `n` is
// small.
template <class Less>
void parallel_stable_sort_indices(std::vector<std::int64_t>& order, Less less) {
    const std::int64_t n = static_cast<std::int64_t>(order.size());
    constexpr std::int64_t RUN = 1 << 16;
    if (!parallel_backend_installed() || n <= RUN) {
        std::stable_sort(order.begin(), order.end(), less);
        return;
    }
    const std::int64_t nruns = (n + RUN - 1) / RUN;
    parallel_for(nruns, 1, [&](std::int64_t c0, std::int64_t c1) {
        for (std::int64_t c = c0; c < c1; ++c) {
            const std::int64_t lo = c * RUN;
            const std::int64_t hi = std::min(n, lo + RUN);
            std::stable_sort(order.begin() + lo, order.begin() + hi, less);
        }
    });
    std::vector<std::int64_t> scratch(static_cast<std::size_t>(n));
    std::int64_t* src = order.data();
    std::int64_t* dst = scratch.data();
    for (std::int64_t width = RUN; width < n; width *= 2) {
        const std::int64_t step = width * 2;
        const std::int64_t npairs = (n + step - 1) / step;
        parallel_for(npairs, 1, [&](std::int64_t p0, std::int64_t p1) {
            for (std::int64_t p = p0; p < p1; ++p) {
                const std::int64_t lo = p * step;
                const std::int64_t mid = std::min(n, lo + width);
                const std::int64_t hi = std::min(n, lo + step);
                std::merge(src + lo, src + mid, src + mid, src + hi, dst + lo,
                           less);
            }
        });
        std::swap(src, dst);
    }
    if (src != order.data()) std::copy(src, src + n, order.data());
}

}  // namespace

DataFrame sort_by_multi(const DataFrame& b,
                        const std::vector<std::string>& names,
                        const std::vector<bool>& descending) {
    if (descending.empty())
        throw std::invalid_argument(
            "sort_by_multi: descending must not be empty");
    if (descending.size() != 1 && descending.size() != names.size())
        throw std::invalid_argument(
            "sort_by_multi: descending must have size 1 or match names.size()");
    std::vector<const Series*> keys;
    keys.reserve(names.size());
    for (const std::string& name : names) {
        std::int64_t k = index_of(b, name);
        if (k < 0)
            throw std::out_of_range("sort_by_multi: no column named " + name);
        keys.push_back(&b.columns[static_cast<std::size_t>(k)]);
    }
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (!is_orderable_type(keys[i]->type()))
            throw std::invalid_argument(std::string("sort_by_multi: column '") +
                                        names[i] + "' has type '" +
                                        type_name(keys[i]->type()) +
                                        "' with no per-row order");
    const bool broadcast = descending.size() == 1;
    const std::int64_t n = b.num_rows();
    std::vector<std::int64_t> order(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) order[static_cast<std::size_t>(i)] = i;
    parallel_stable_sort_indices(order, [&](std::int64_t a, std::int64_t bb) {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const Series* c = keys[i];
            bool na = c->is_null(a);
            bool nb = c->is_null(bb);
            if (na || nb) {
                if (na && nb) continue;
                return !na;  // nulls last in both directions
            }
            int r = compare_rows(*c, a, bb);
            if (broadcast ? descending[0] : descending[i]) r = -r;
            if (r != 0) return r < 0;
        }
        return false;
    });
    return take(b, order);
}

DataFrame sort_by_multi(const DataFrame& b,
                        const std::vector<std::string>& names,
                        bool descending) {
    return sort_by_multi(b, names, std::vector<bool>{descending});
}

DataFrame tail(const DataFrame& b, std::int64_t n) {
    const std::int64_t len = b.num_rows();
    n = std::clamp<std::int64_t>(n, 0, len);
    return slice(b, len - n, n);
}

DataFrame reverse(const DataFrame& b) {
    const std::int64_t n = b.num_rows();
    std::vector<std::int64_t> idx(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        idx[static_cast<std::size_t>(i)] = n - 1 - i;
    return take(b, idx);
}

DataFrame sample(const DataFrame& b, std::int64_t n, std::uint64_t seed) {
    const std::int64_t len = b.num_rows();
    n = std::clamp<std::int64_t>(n, 0, len);
    std::vector<std::int64_t> order(static_cast<std::size_t>(len));
    for (std::int64_t i = 0; i < len; ++i)
        order[static_cast<std::size_t>(i)] = i;
    auto key = [seed](std::int64_t i) {
        return splitmix64(static_cast<std::uint64_t>(i) + seed);
    };
    if (n < len)
        std::nth_element(
            order.begin(), order.begin() + n, order.end(),
            [&](std::int64_t a, std::int64_t c) { return key(a) < key(c); });
    order.resize(static_cast<std::size_t>(n));
    std::sort(order.begin(), order.end());
    return take(b, order);
}

DataFrame with_row_index(const DataFrame& b, const std::string& name) {
    const std::int64_t n = b.num_rows();
    std::vector<std::int64_t> ids(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) ids[static_cast<std::size_t>(i)] = i;
    DataFrame out;
    out.names.reserve(b.names.size() + 1);
    out.columns.reserve(b.columns.size() + 1);
    out.names.push_back(name);
    out.columns.push_back(Series::flat_i64(ids.data(), n));
    for (std::size_t i = 0; i < b.columns.size(); ++i) {
        out.names.push_back(b.names[i]);
        out.columns.push_back(b.columns[i].share());
    }
    return out;
}

DataFrame describe(const DataFrame& b) {
    const std::vector<std::string> stat_names = {"count", "null_count", "mean",
                                                 "std",   "min",        "max"};
    DataFrame out;
    out.names.push_back("statistic");
    out.columns.push_back(Series::strings(stat_names));
    const std::int64_t n = b.num_rows();
    for (std::size_t i = 0; i < b.columns.size(); ++i) {
        const Series& c = b.columns[i];
        if (!is_numeric(c.type())) continue;
        const FieldStat fs = field_stat_reduce(c);
        double vals[6];
        vals[0] = static_cast<double>(fs.n);
        vals[1] = static_cast<double>(n - static_cast<std::int64_t>(fs.n));
        vals[2] = fs.mean();
        vals[3] = fs.stddev();
        vals[4] = fs.n ? fs.min : 0.0;
        vals[5] = fs.n ? fs.max : 0.0;
        out.names.push_back(b.names[i]);
        out.columns.push_back(Series::flat_f64(vals, 6));
    }
    return out;
}

DataFrame null_count(const DataFrame& b) {
    DataFrame out;
    out.names = b.names;
    out.columns.reserve(b.columns.size());
    for (const Series& c : b.columns) {
        std::int64_t nc = c.null_count();
        out.columns.push_back(Series::flat_i64(&nc, 1));
    }
    return out;
}

// The per-column reductions a whole-frame broadcast makes sense for: one
// value column, no `by`, no `param`. Anything else (corr, pct, topk, ...) is
// a group_by with no key and an explicit spec: reduce(b, aggs).
bool reduce_broadcasts(Agg agg) noexcept {
    switch (agg) {
        case Agg::Sum:
        case Agg::Min:
        case Agg::Max:
        case Agg::CountValid:
        case Agg::Mean:
        case Agg::Var:
        case Agg::Std:
        case Agg::Skew:
        case Agg::Kurt:
        case Agg::SumSq:
        case Agg::First:
        case Agg::Last:
        case Agg::BitOr:
        case Agg::Prod:
            return true;
        case Agg::Count:
        case Agg::Pct:
        case Agg::Hist:
        case Agg::ArgMax:
        case Agg::SetUnion:
        case Agg::Busy:
        case Agg::Concurrency:
        case Agg::Utilization:
        case Agg::Active:
        case Agg::ArgMin:
        case Agg::Distinct:
        case Agg::ListSorted:
        case Agg::TopK:
        case Agg::BottomK:
        case Agg::ApproxTopK:
        case Agg::Sample:
        case Agg::Corr:
        case Agg::CovarPop:
        case Agg::CovarSamp:
        case Agg::RegrSlope:
        case Agg::RegrIntercept:
        case Agg::RegrR2:
            return false;
    }
    return false;
}

std::vector<GroupAgg> reduce_specs(const std::vector<std::string>& names,
                                   const std::vector<TypeId>& types, Agg agg,
                                   const std::vector<std::string>& keys) {
    if (!reduce_broadcasts(agg))
        throw std::invalid_argument(
            std::string("reduce: '") + to_string(agg) +
            "' takes a second column or a parameter; give it as a spec");
    const bool any_type =
        agg == Agg::CountValid || agg == Agg::First || agg == Agg::Last;
    std::vector<GroupAgg> out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (std::find(keys.begin(), keys.end(), names[i]) != keys.end())
            continue;
        if (any_type || types[i] == TypeId::Unknown ||
            is_numeric_dispatchable(types[i]))
            out.push_back(GroupAgg{agg, names[i], names[i]});
    }
    return out;
}

DataFrame reduce(const DataFrame& b, const std::vector<GroupAgg>& aggs) {
    return group_by(b, std::vector<std::string>{}, aggs);
}

std::vector<GroupAgg> reduce_specs(const DataFrame& b, Agg agg,
                                   const std::vector<std::string>& keys) {
    std::vector<TypeId> types;
    types.reserve(b.columns.size());
    for (const Series& c : b.columns) types.push_back(c.type());
    return reduce_specs(b.names, types, agg, keys);
}

DataFrame reduce(const DataFrame& b, Agg agg) {
    return reduce(b, reduce_specs(b, agg, {}));
}

Series is_duplicated(const DataFrame& b) { return row_mask(b, false); }
Series is_unique(const DataFrame& b) { return row_mask(b, true); }

DataFrame value_counts(const Series& v) {
    if (!is_orderable_type(v.type()))
        throw std::invalid_argument(std::string("value_counts: type '") +
                                    type_name(v.type()) +
                                    "' has no per-row value to count");
    Series mat = v.encoding() == Encoding::Flat
                     ? v.share()
                     : Series{dftu_series_materialize(v.handle())};
    const std::int64_t n = mat.length();
    const bool has_nulls = mat.null_count() > 0;

    std::vector<std::int64_t> nonnull_idx;
    if (has_nulls) {
        nonnull_idx.reserve(static_cast<std::size_t>(n));
        for (std::int64_t i = 0; i < n; ++i)
            if (!mat.is_null(i)) nonnull_idx.push_back(i);
    }
    const std::int64_t m =
        has_nulls ? static_cast<std::int64_t>(nonnull_idx.size()) : n;
    auto idx_at = [&](std::int64_t j) {
        return has_nulls ? nonnull_idx[static_cast<std::size_t>(j)] : j;
    };
    auto key_of = [&](std::int64_t j) {
        std::string k;
        append_cell(k, mat, idx_at(j));
        return k;
    };

    // Radix-partitioned first-occurrence detection + counting: the same
    // shape as unique()/row_mask(), so distinct values fan out through
    // disjoint hash buckets instead of one serial map.
    std::vector<std::uint8_t> keep =
        radix_first_seen_by<std::string>(m, key_of);
    std::vector<std::int64_t> cnts = radix_counts_by<std::string>(m, key_of);

    std::vector<std::int64_t> first_index;
    std::vector<std::int64_t> counts;
    for (std::int64_t j = 0; j < m; ++j)
        if (keep[static_cast<std::size_t>(j)]) {
            first_index.push_back(idx_at(j));
            counts.push_back(cnts[static_cast<std::size_t>(j)]);
        }
    DataFrame df;
    df.names = {"value", "count"};
    df.columns.push_back(mat.take(first_index));
    df.columns.push_back(Series::flat_i64(
        counts.data(), static_cast<std::int64_t>(counts.size())));
    return sort_by(df, "count", true);
}

namespace {

// A FLAT copy of `c` (sharing an already-FLAT column), so offset/value buffers
// can be read directly.
Series flat_copy(const Series& c) {
    return c.encoding() == Encoding::Flat
               ? c.share()
               : Series{dftu_series_materialize(c.handle())};
}

// Human-readable rendering of cell (c, i) for a dummy column label. `c` is
// FLAT.
bool is_float_type(TypeId t) {
    return t == TypeId::Float32 || t == TypeId::Float64;
}

}  // namespace

DataFrame unpivot(const DataFrame& b, const std::vector<std::string>& id_vars,
                  const std::vector<std::string>& value_vars) {
    if (value_vars.empty())
        throw std::invalid_argument("unpivot: value_vars must be non-empty");

    for (const std::string& name : id_vars)
        if (index_of(b, name) < 0)
            throw std::out_of_range("unpivot: no column named " + name);
    std::vector<std::int64_t> val_idx;
    val_idx.reserve(value_vars.size());
    for (const std::string& name : value_vars) {
        std::int64_t i = index_of(b, name);
        if (i < 0) throw std::out_of_range("unpivot: no column named " + name);
        val_idx.push_back(i);
    }

    const TypeId first = b.columns[static_cast<std::size_t>(val_idx[0])].type();
    bool all_same = true, any_float = false, all_numeric = true;
    for (std::int64_t vi : val_idx) {
        TypeId t = b.columns[static_cast<std::size_t>(vi)].type();
        if (t != first) all_same = false;
        if (is_float_type(t)) any_float = true;
        if (!is_numeric(t)) all_numeric = false;
    }
    TypeId common;
    if (all_same) {
        common = first;
    } else if (all_numeric) {
        common = any_float ? TypeId::Float64 : TypeId::Int64;
    } else {
        throw std::invalid_argument(
            "unpivot: value_vars must share a type or all be numeric");
    }

    const std::int64_t n = b.num_rows();
    std::vector<std::int64_t> identity(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        identity[static_cast<std::size_t>(i)] = i;

    DataFrame id_df = select(b, id_vars);
    std::vector<DataFrame> blocks;
    blocks.reserve(value_vars.size());
    for (std::size_t j = 0; j < value_vars.size(); ++j) {
        DataFrame block = take(id_df, identity);
        std::vector<std::string> varnames(static_cast<std::size_t>(n),
                                          value_vars[j]);
        block = with_column(block, "variable", Series::strings(varnames));

        const Series& src = b.columns[static_cast<std::size_t>(val_idx[j])];
        Series matv = flat_copy(src);
        Series valcol =
            matv.type() == common ? std::move(matv) : matv.cast(common);
        block = with_column(block, "value", valcol);
        blocks.push_back(std::move(block));
    }

    std::vector<const DataFrame*> parts;
    parts.reserve(blocks.size());
    for (const DataFrame& blk : blocks) parts.push_back(&blk);
    return concat(parts);
}

namespace {

DataFrame explode_impl(const DataFrame& b, const std::string& column,
                       bool keep_empty, bool flatten_struct, const char* who) {
    std::int64_t ci = index_of(b, column);
    if (ci < 0)
        throw std::out_of_range(std::string(who) + ": no column named " +
                                column);
    Series lst = flat_copy(b.columns[static_cast<std::size_t>(ci)]);
    if (lst.type() != TypeId::List)
        throw std::invalid_argument(std::string(who) + ": " + column +
                                    " is not a List column");

    const std::int64_t n = lst.length();
    const std::int32_t* offs = lst.offsets();
    Series child = lst.child(0);

    std::vector<std::int64_t> rep_idx;
    std::vector<std::int64_t> child_idx;
    for (std::int64_t i = 0; i < n; ++i) {
        std::int32_t s = offs[i];
        std::int32_t e = offs[i + 1];
        if (e == s) {
            if (!keep_empty) continue;
            rep_idx.push_back(i);
            child_idx.push_back(-1);
        } else {
            for (std::int32_t j = s; j < e; ++j) {
                rep_idx.push_back(i);
                child_idx.push_back(j);
            }
        }
    }

    const bool flatten = flatten_struct && child.type() == TypeId::Struct;
    DataFrame out;
    out.names.reserve(b.names.size());
    out.columns.reserve(b.columns.size());
    for (std::size_t k = 0; k < b.columns.size(); ++k) {
        if (static_cast<std::int64_t>(k) != ci) {
            out.names.push_back(b.names[k]);
            out.columns.push_back(b.columns[k].take(rep_idx));
        } else if (flatten) {
            const std::int64_t nf = child.num_children();
            for (std::int64_t f = 0; f < nf; ++f) {
                out.names.emplace_back(child.field_name(f));
                out.columns.push_back(child.child(f).take(child_idx));
            }
        } else {
            out.names.push_back(b.names[k]);
            out.columns.push_back(child.take(child_idx));
        }
    }
    return out;
}

}  // namespace

DataFrame explode(const DataFrame& b, const std::string& column) {
    return explode_impl(b, column, true, false, "explode");
}

DataFrame unnest(const DataFrame& b, const std::string& column,
                 bool keep_empty) {
    return explode_impl(b, column, keep_empty, true, "unnest");
}

DataFrame to_dummies(const DataFrame& b, const std::string& column) {
    std::int64_t ci = index_of(b, column);
    if (ci < 0)
        throw std::out_of_range("to_dummies: no column named " + column);
    Series mat = flat_copy(b.columns[static_cast<std::size_t>(ci)]);
    if (!is_orderable_type(mat.type()))
        throw std::invalid_argument(
            std::string("to_dummies: column '") + column + "' has type '" +
            type_name(mat.type()) + "' with no per-row value to key on");
    const std::int64_t n = mat.length();

    // Distinct non-null values, ascending, for a deterministic column order.
    Series uniq = flat_copy(mat.unique());
    const std::int64_t d = uniq.length();

    std::vector<std::string> row_key(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        append_cell(row_key[static_cast<std::size_t>(i)], mat, i);

    // Bucket id of each row (its index into `uniq`), one hash lookup per row,
    // instead of comparing every row against every distinct value.
    std::unordered_map<std::string, std::int64_t> bucket_of;
    bucket_of.reserve(static_cast<std::size_t>(d));
    for (std::int64_t u = 0; u < d; ++u) {
        std::string ukey;
        append_cell(ukey, uniq, u);
        bucket_of.emplace(std::move(ukey), u);
    }
    std::vector<std::int64_t> bucket(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        bucket[static_cast<std::size_t>(i)] =
            bucket_of.at(row_key[static_cast<std::size_t>(i)]);

    // One flag byte per (row, dummy) is set once, in row-major order.
    std::vector<std::vector<std::int8_t>> bits(
        static_cast<std::size_t>(d),
        std::vector<std::int8_t>(static_cast<std::size_t>(n), 0));
    for (std::int64_t i = 0; i < n; ++i)
        bits[static_cast<std::size_t>(bucket[static_cast<std::size_t>(i)])]
            [static_cast<std::size_t>(i)] = 1;

    DataFrame out;
    out.names.reserve(b.names.size() + static_cast<std::size_t>(d) - 1);
    out.columns.reserve(out.names.capacity());
    for (std::size_t k = 0; k < b.columns.size(); ++k) {
        if (static_cast<std::int64_t>(k) != ci) {
            out.names.push_back(b.names[k]);
            out.columns.push_back(b.columns[k].share());
            continue;
        }
        for (std::int64_t u = 0; u < d; ++u) {
            out.names.push_back(column + "_" + cell_to_string(uniq, u));
            out.columns.push_back(Series::flat(
                TypeId::Int8, bits[static_cast<std::size_t>(u)].data(), n));
        }
    }
    return out;
}

namespace {

enum class PivotMode { First, Last, Reduce };

PivotMode pivot_mode_of(const std::string& agg, AggOp& op) {
    if (agg == "first") return PivotMode::First;
    if (agg == "last") return PivotMode::Last;
    op = agg_op_of(agg);  // sum|min|max|mean (and the other AggOps)
    return PivotMode::Reduce;
}

}  // namespace

DataFrame pivot(const DataFrame& b, const std::string& index_name,
                const std::string& columns_name, const std::string& values_name,
                const std::string& agg) {
    std::int64_t ii = index_of(b, index_name);
    std::int64_t ci = index_of(b, columns_name);
    std::int64_t vi = index_of(b, values_name);
    if (ii < 0) throw std::out_of_range("pivot: no column named " + index_name);
    if (ci < 0)
        throw std::out_of_range("pivot: no column named " + columns_name);
    if (vi < 0)
        throw std::out_of_range("pivot: no column named " + values_name);

    AggOp reduce_op = AggOp::Sum;
    PivotMode mode = pivot_mode_of(agg, reduce_op);

    Series idx_col = flat_copy(b.columns[static_cast<std::size_t>(ii)]);
    Series col_col = flat_copy(b.columns[static_cast<std::size_t>(ci)]);
    Series val_col = flat_copy(b.columns[static_cast<std::size_t>(vi)]);
    if (!is_orderable_type(idx_col.type()))
        throw std::invalid_argument(
            std::string("pivot: index column '") + index_name + "' has type '" +
            type_name(idx_col.type()) + "' with no per-row value to key on");
    if (!is_orderable_type(col_col.type()))
        throw std::invalid_argument(std::string("pivot: columns column '") +
                                    columns_name + "' has type '" +
                                    type_name(col_col.type()) +
                                    "' with no per-row value to key on");
    const std::int64_t n = idx_col.length();

    // Output rows = distinct index values (sorted asc, non-null); output value
    // columns = distinct column values (sorted asc, non-null).
    Series uniq_idx = flat_copy(idx_col.unique());
    Series uniq_col = flat_copy(col_col.unique());
    const std::int64_t R = uniq_idx.length();
    const std::int64_t C = uniq_col.length();

    std::unordered_map<std::string, std::int64_t> row_of, col_of;
    row_of.reserve(static_cast<std::size_t>(R));
    col_of.reserve(static_cast<std::size_t>(C));
    for (std::int64_t r = 0; r < R; ++r) {
        std::string k;
        append_cell(k, uniq_idx, r);
        row_of.emplace(std::move(k), r);
    }
    for (std::int64_t c = 0; c < C; ++c) {
        std::string k;
        append_cell(k, uniq_col, c);
        col_of.emplace(std::move(k), c);
    }

    // For each output cell (r, c), the source row into `source` (or -1 = absent
    // -> null). first/last take a row of `val_col` directly; the numeric modes
    // aggregate via group_agg, then take a row of the aggregate result. Both
    // reduce to a type-generic gather.
    Series source;
    std::vector<std::int64_t> cell_src(static_cast<std::size_t>(R * C), -1);

    if (mode == PivotMode::Reduce) {
        // Key each valid row by its flattened cell id; invalid (null-key) rows
        // go to a sentinel bucket that is dropped after aggregation.
        const std::int64_t sentinel = R * C;
        std::vector<std::int64_t> keyv(static_cast<std::size_t>(n), sentinel);
        for (std::int64_t i = 0; i < n; ++i) {
            if (idx_col.is_null(i) || col_col.is_null(i)) continue;
            std::string rk, ck;
            append_cell(rk, idx_col, i);
            append_cell(ck, col_col, i);
            keyv[static_cast<std::size_t>(i)] =
                row_of.at(rk) * C + col_of.at(ck);
        }
        Series keyc = Series::flat_i64(keyv.data(), n);
        std::vector<const Series*> values{&val_col};
        std::vector<AggSpec> specs;
        specs.push_back(AggSpec{reduce_op, 0, "v"});
        DataFrame agg_res = group_agg(keyc, values, std::move(specs), "cell");
        source = agg_res.column("v");
        Series cells_col = agg_res.column("cell");
        const std::int64_t m = cells_col.length();
        const std::int64_t* cells = cells_col.data<std::int64_t>();
        for (std::int64_t p = 0; p < m; ++p) {
            std::int64_t cell = cells[p];
            if (cell == sentinel) continue;
            cell_src[static_cast<std::size_t>(cell)] = p;
        }
    } else {
        source = val_col.share();
        for (std::int64_t i = 0; i < n; ++i) {
            if (idx_col.is_null(i) || col_col.is_null(i)) continue;
            std::string rk, ck;
            append_cell(rk, idx_col, i);
            append_cell(ck, col_col, i);
            std::int64_t cell = row_of.at(rk) * C + col_of.at(ck);
            std::int64_t& slot = cell_src[static_cast<std::size_t>(cell)];
            if (mode == PivotMode::First) {
                if (slot < 0) slot = i;
            } else {
                slot = i;  // last wins
            }
        }
    }

    DataFrame out;
    out.names.reserve(static_cast<std::size_t>(C) + 1);
    out.columns.reserve(static_cast<std::size_t>(C) + 1);
    out.names.push_back(index_name);
    out.columns.push_back(uniq_idx.share());
    for (std::int64_t c = 0; c < C; ++c) {
        std::vector<std::int64_t> take_idx(static_cast<std::size_t>(R));
        for (std::int64_t r = 0; r < R; ++r)
            take_idx[static_cast<std::size_t>(r)] =
                cell_src[static_cast<std::size_t>(r * C + c)];
        out.names.push_back(cell_to_string(uniq_col, c));
        out.columns.push_back(source.take(take_idx));
    }
    return out;
}

DataFrame group_by_dynamic(const DataFrame& b, const std::string& time_col,
                           std::int64_t every, std::int64_t period,
                           const std::vector<GroupAgg>& aggs,
                           std::int64_t origin, bool origin_min) {
    if (every <= 0)
        throw std::invalid_argument("group_by_dynamic: every must be > 0");
    if (period <= 0) period = every;

    std::int64_t ti = index_of(b, time_col);
    if (ti < 0)
        throw std::out_of_range("group_by_dynamic: no column named " +
                                time_col);
    Series tcol = flat_copy(b.columns[static_cast<std::size_t>(ti)]);
    if (tcol.type() != TypeId::Int64)
        throw std::invalid_argument("group_by_dynamic: " + time_col +
                                    " must be an Int64 column");
    const std::int64_t n = tcol.length();
    const std::int64_t* t = tcol.data<std::int64_t>();

    // `origin_min` aligns the grid to the minimum time value (buckets begin
    // exactly at min ts), the frame-native analogue of time_bucket("min").
    if (origin_min) {
        bool seen = false;
        for (std::int64_t i = 0; i < n; ++i)
            if (!tcol.is_null(i)) {
                if (!seen || t[i] < origin) origin = t[i];
                seen = true;
            }
        if (!seen) origin = 0;
    }

    // The window grid is `origin + k*every`; the first non-null time picks the
    // first window (origin defaults to 0 = the classic ts-floored grid).
    std::int64_t start0 = 0;
    bool have_anchor = false;
    for (std::int64_t i = 0; i < n; ++i)
        if (!tcol.is_null(i)) {
            start0 = origin + floor_to_multiple(t[i] - origin, every);
            have_anchor = true;
            break;
        }

    // Tumbling windows over a null-free time column: every row is in
    // exactly one window, so the window start is a key column computed in
    // parallel and the value columns are the frame's own, no pair list and
    // no gather.
    if (have_anchor && period == every && tcol.null_count() == 0) {
        auto* kc = new dftu_series();
        kc->type = TypeId::Int64;
        kc->encoding = Encoding::Flat;
        kc->length = n;
        kc->data = Buffer::allocate(static_cast<std::size_t>(n) *
                                    sizeof(std::int64_t));
        auto* kd = reinterpret_cast<std::int64_t*>(kc->data->data());
        parallel_for(n, std::int64_t{1} << 16,
                     [&](std::int64_t lo, std::int64_t hi) {
                         for (std::int64_t i = lo; i < hi; ++i)
                             kd[i] = start0 + ((t[i] - start0) / every) * every;
                     });
        Series keyc{kc};
        std::vector<AggSpec> specs;
        std::vector<const Series*> values;
        std::map<std::string, std::int32_t> col_idx;
        auto resolve = [&](const std::string& name) -> std::int32_t {
            auto it = col_idx.find(name);
            if (it != col_idx.end()) return it->second;
            std::int64_t vidx = index_of(b, name);
            if (vidx < 0)
                throw std::out_of_range("group_by_dynamic: no column named " +
                                        name);
            const std::int32_t idx = static_cast<std::int32_t>(values.size());
            values.push_back(&b.columns[static_cast<std::size_t>(vidx)]);
            col_idx.emplace(name, idx);
            return idx;
        };
        specs.reserve(aggs.size());
        for (const GroupAgg& a : aggs) {
            AggSpec sp;
            sp.op = to_agg_op(a.op);
            sp.out = a.out;
            sp.param = a.param;
            sp.value_col = sp.op == AggOp::Count ? -1 : resolve(a.column);
            if (agg_uses_by_col(sp.op)) sp.by_col = resolve(a.by);
            specs.push_back(std::move(sp));
        }
        DataFrame res = group_agg(keyc, values, std::move(specs), time_col);
        return sort_by(res, time_col, false);
    }

    // (window start, source row) pairs: a row belongs to every window whose
    // start S = start0 + k*every satisfies S <= t < S + period.
    std::vector<std::int64_t> keyv;
    std::vector<std::int64_t> rowsv;
    if (have_anchor) {
        for (std::int64_t i = 0; i < n; ++i) {
            if (tcol.is_null(i)) continue;
            const std::int64_t ts = t[i];
            std::int64_t k = (ts - start0) / every;  // largest S <= ts
            for (; k >= 0; --k) {
                std::int64_t s = start0 + k * every;
                if (s <= ts - period) break;  // smaller k only makes s smaller
                keyv.push_back(s);
                rowsv.push_back(i);
            }
        }
    }

    const std::int64_t pairs = static_cast<std::int64_t>(keyv.size());
    Series keyc = Series::flat_i64(keyv.data(), pairs);

    // Lower aggs to specs + deduped value columns, gathered at the emitted rows
    // (mirrors dfops::group_by's lowering).
    std::vector<Series> gathered;
    std::vector<AggSpec> specs;
    std::map<std::string, std::int32_t> col_idx;
    auto resolve = [&](const std::string& name) -> std::int32_t {
        auto it = col_idx.find(name);
        if (it != col_idx.end()) return it->second;
        std::int64_t vidx = index_of(b, name);
        if (vidx < 0)
            throw std::out_of_range("group_by_dynamic: no column named " +
                                    name);
        const std::int32_t idx = static_cast<std::int32_t>(gathered.size());
        gathered.push_back(
            b.columns[static_cast<std::size_t>(vidx)].take(rowsv));
        col_idx.emplace(name, idx);
        return idx;
    };
    specs.reserve(aggs.size());
    for (const GroupAgg& a : aggs) {
        AggSpec sp;
        sp.op = to_agg_op(a.op);
        sp.out = a.out;
        sp.param = a.param;
        sp.value_col = sp.op == AggOp::Count ? -1 : resolve(a.column);
        if (agg_uses_by_col(sp.op)) sp.by_col = resolve(a.by);
        specs.push_back(std::move(sp));
    }
    std::vector<const Series*> values;
    values.reserve(gathered.size());
    for (const Series& g : gathered) values.push_back(&g);

    DataFrame res = group_agg(keyc, values, std::move(specs), time_col);
    return sort_by(res, time_col, false);
}

}  // namespace dftracer::utils::dataframe
