#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/filter_simd.h>
#include <dftracer/utils/dataframe/internal/numeric_dispatch.h>
#include <dftracer/utils/dataframe/internal/scalar.h>
#include <dftracer/utils/dataframe/internal/varwidth_offsets.h>
#include <dftracer/utils/dataframe/kernels/filter.h>
#include <dftracer/utils/dataframe/parallel.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using dftracer::utils::dataframe::Buffer;
using dftracer::utils::dataframe::Encoding;
using dftracer::utils::dataframe::is_wide_offset_type;
using dftracer::utils::dataframe::narrow_varwidth_type;
using dftracer::utils::dataframe::offsets_of;
using dftracer::utils::dataframe::scalar_as;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;

namespace {

bool is_valid(const dftu_series& v, std::int64_t i) {
    if (!v.validity) return true;
    const std::uint8_t* bm = v.validity->data();
    return (bm[i >> 3] & (1u << (i & 7))) != 0;
}

// Compare in the column's own type domain; the scalar is converted to the
// column type T, so 64-bit integers are exact.
template <class T>
void select_gt(const dftu_series& v, dftu_scalar threshold,
               std::vector<std::int64_t>& sel) {
    const T* p = reinterpret_cast<const T*>(v.data->data());
    const T t = scalar_as<T>(threshold);
    for (std::int64_t i = 0; i < v.length; ++i) {
        if (is_valid(v, i) && p[i] > t) sel.push_back(i);
    }
}

// Build a SELECTION column over `base` from row indices `sel`. A view over a
// view composes: a SELECTION base contributes its own indices (still zero
// copy over the flat root), any other view is materialized first, so a
// selection is always one level over a FLAT column.
// `indices` is the index buffer the view adopts, shared between every
// column of one frame filter; a composed or materialized base gets its own.
dftu_series* make_selection(const dftu_series& base,
                            std::shared_ptr<Buffer> indices, std::int64_t n) {
    const std::int64_t* sel =
        reinterpret_cast<const std::int64_t*>(indices->data());
    if (base.encoding == Encoding::Selection && base.child) {
        const std::int64_t* inner =
            reinterpret_cast<const std::int64_t*>(base.data->data());
        auto composed = Buffer::allocate(static_cast<std::size_t>(n) *
                                         sizeof(std::int64_t));
        auto* c = reinterpret_cast<std::int64_t*>(composed->data());
        for (std::int64_t i = 0; i < n; ++i)
            c[i] = sel[i] < 0 ? -1 : inner[sel[i]];
        return make_selection(*base.child, std::move(composed), n);
    }
    if (base.encoding != Encoding::Flat) {
        dftu_series* flat = dftu_series_materialize(&base);
        if (!flat) return nullptr;
        dftu_series* out = make_selection(*flat, std::move(indices), n);
        dftu_series_free(flat);
        return out;
    }
    auto* out = new dftu_series();
    dftracer::utils::dataframe::adopt_type_from(*out, base);
    out->encoding = Encoding::Selection;
    out->length = n;
    out->data = std::move(indices);
    out->child = std::make_shared<dftu_series>(base);
    return out;
}

std::shared_ptr<Buffer> index_buffer(const std::vector<std::int64_t>& sel) {
    auto buf = Buffer::allocate(sel.size() * sizeof(std::int64_t));
    if (!sel.empty())
        std::memcpy(buf->data(), sel.data(), sel.size() * sizeof(std::int64_t));
    return buf;
}

dftu_series* make_selection(const dftu_series& base,
                            const std::vector<std::int64_t>& sel) {
    return make_selection(base, index_buffer(sel),
                          static_cast<std::int64_t>(sel.size()));
}

}  // namespace

dftu_series* dftu_series_filter_gt(const dftu_series* v,
                                   dftu_scalar threshold) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_filter_gt(flat_v, threshold));

    if (v->encoding != Encoding::Flat) return nullptr;
    if (v->type == TypeId::Bool || v->type == TypeId::String ||
        v->type == TypeId::Binary)
        return nullptr;

    std::vector<std::int64_t> sel;
    if (!v->validity) {
        sel.resize(static_cast<std::size_t>(v->length));
        std::int64_t k =
            dftracer::utils::dataframe::compact_gt(*v, threshold, sel.data());
        if (k >= 0) {
            sel.resize(static_cast<std::size_t>(k));
            return make_selection(*v, sel);
        }
        sel.clear();
    }
    DF_NUMERIC_DISPATCH(v->type, select_gt, *v, threshold, sel)
    return make_selection(*v, sel);
}

namespace dftracer::utils::dataframe {

// Two passes over 64-bit words: a popcount per chunk gives each chunk its
// output offset, then every chunk writes its indices in place, in parallel.
std::shared_ptr<Buffer> mask_to_indices(const std::uint8_t* bits,
                                        std::int64_t n, std::int64_t& count) {
    constexpr std::int64_t GRAIN = std::int64_t{1} << 16;  // whole words
    const std::int64_t words = n / 64;
    const std::int64_t chunks = (words + GRAIN - 1) / GRAIN;
    std::vector<std::int64_t> counts(static_cast<std::size_t>(chunks) + 1, 0);
    parallel_for(words, GRAIN, [&](std::int64_t b, std::int64_t e) {
        std::int64_t c = 0;
        for (std::int64_t w = b; w < e; ++w) {
            std::uint64_t x;
            std::memcpy(&x, bits + w * 8, sizeof(x));
            c += __builtin_popcountll(x);
        }
        counts[static_cast<std::size_t>(b / GRAIN) + 1] = c;
    });
    for (std::int64_t c = 0; c < chunks; ++c)
        counts[static_cast<std::size_t>(c) + 1] +=
            counts[static_cast<std::size_t>(c)];
    std::int64_t tail = 0;
    for (std::int64_t i = words * 64; i < n; ++i)
        tail += (bits[i >> 3] >> (i & 7)) & 1;
    count = counts.back() + tail;
    auto out = Buffer::allocate(static_cast<std::size_t>(count) *
                                sizeof(std::int64_t));
    auto* sel = reinterpret_cast<std::int64_t*>(out->data());
    parallel_for(words, GRAIN, [&](std::int64_t b, std::int64_t e) {
        std::int64_t k = counts[static_cast<std::size_t>(b / GRAIN)];
        for (std::int64_t w = b; w < e; ++w) {
            std::uint64_t x;
            std::memcpy(&x, bits + w * 8, sizeof(x));
            while (x != 0) {
                sel[static_cast<std::size_t>(k++)] =
                    w * 64 + static_cast<unsigned>(__builtin_ctzll(x));
                x &= x - 1;
            }
        }
    });
    std::int64_t k = counts.back();
    for (std::int64_t i = words * 64; i < n; ++i)
        if ((bits[i >> 3] >> (i & 7)) & 1)
            sel[static_cast<std::size_t>(k++)] = i;
    return out;
}

Series mask_index_column(const std::uint8_t* bits, std::int64_t n) {
    std::int64_t count = 0;
    std::shared_ptr<Buffer> idx = mask_to_indices(bits, n, count);
    auto* out = new dftu_series();
    out->type = TypeId::Int64;
    out->encoding = Encoding::Flat;
    out->length = count;
    out->data = std::move(idx);
    return Series{out};
}

}  // namespace dftracer::utils::dataframe

dftu_series* dftu_series_filter(const dftu_series* v, const dftu_series* mask) {
    if (mask->type != TypeId::Bool || mask->encoding != Encoding::Flat)
        return nullptr;
    if (mask->length != v->length) return nullptr;
    std::int64_t count = 0;
    std::shared_ptr<Buffer> sel = dftracer::utils::dataframe::mask_to_indices(
        mask->data->data(), v->length, count);
    return make_selection(*v, std::move(sel), count);
}

using dftracer::utils::dataframe::buffer_bytes;

// Gather `base`'s validity bitmap by `idx`; sets `null_count`. A negative index
// gathers a null (the join OUTER-fill sentinel), so validity is materialized
// when the base has nulls OR any index is negative. Null otherwise.
// What one pass over an index list establishes: its largest value and
// whether any is negative (the null sentinel). A frame gather computes it
// once and hands it to every column; a lone column gather computes its own.
struct IdxInfo {
    std::int64_t top = -1;
    bool has_neg = false;
};

template <class Idx>
static IdxInfo scan_indices(const Idx* idx, std::int64_t n) {
    struct Acc {
        std::int64_t top = -1;
        std::int64_t neg = 0;
    };
    const Acc acc = dftracer::utils::dataframe::parallel_reduce<Acc>(
        n, std::int64_t{1} << 16, Acc{},
        [&](std::int64_t b, std::int64_t e) {
            Acc a;
            for (std::int64_t i = b; i < e; ++i) {
                const auto v = static_cast<std::int64_t>(idx[i]);
                a.top = std::max(a.top, v);
                a.neg |= v < 0;
            }
            return a;
        },
        [](Acc a, Acc b) {
            return Acc{std::max(a.top, b.top), a.neg | b.neg};
        });
    return IdxInfo{acc.top, acc.neg != 0};
}

template <class Idx>
static std::shared_ptr<Buffer> gather_validity(const dftu_series& base,
                                               const Idx* idx, std::int64_t n,
                                               std::int64_t& null_count,
                                               bool has_null_idx) {
    null_count = 0;
    if (!base.validity && !has_null_idx) return nullptr;
    const std::uint8_t* bm = base.validity ? base.validity->data() : nullptr;
    auto out = Buffer::allocate(buffer_bytes(TypeId::Bool, n));
    std::memset(out->data(), 0, out->size());
    std::uint8_t* ob = out->data();
    // Whole bytes per task (8 rows), so no two tasks write one byte.
    const std::int64_t bytes = (n + 7) / 8;
    null_count = dftracer::utils::dataframe::parallel_reduce<std::int64_t>(
        bytes, std::int64_t{1} << 13, std::int64_t{0},
        [&](std::int64_t b0, std::int64_t b1) {
            std::int64_t nulls = 0;
            for (std::int64_t i = b0 * 8; i < std::min(n, b1 * 8); ++i) {
                const std::int64_t k = idx[i];
                const bool valid =
                    k >= 0 && (!bm || ((bm[k >> 3] >> (k & 7)) & 1));
                if (valid)
                    ob[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
                else
                    ++nulls;
            }
            return nulls;
        },
        [](std::int64_t a, std::int64_t b) { return a + b; });
    return out;
}

template <class Idx>
static dftu_series* gather_column(const dftu_series& base, const Idx* idx,
                                  std::int64_t n,
                                  const IdxInfo* known = nullptr);

// String/Binary(/Large): concatenate the selected slices, rebuild offsets at
// the same width as `base` (int32 for String/Binary, int64 for LargeString/
// LargeBinary) - take/gather reproduces the source column reordered, so it
// must not narrow a Large column's offsets.
// Two passes straight into the result's buffers: the lengths become the
// offsets by a prefix sum, then every slice is copied into its exact place,
// so the copy fans out over disjoint ranges and nothing is appended and
// moved again.
template <class Off, class Idx>
static dftu_series* gather_varwidth_w(const dftu_series& base, const Idx* idx,
                                      std::int64_t n, const IdxInfo& info) {
    const Off* boff =
        reinterpret_cast<const Off*>(offsets_of<Off>(base)->data());
    const char* bdata = reinterpret_cast<const char*>(base.data->data());

    auto* result = new dftu_series();
    result->type = base.type;
    result->encoding = Encoding::Flat;
    result->length = n;
    offsets_of<Off>(*result) =
        Buffer::allocate((static_cast<std::size_t>(n) + 1) * sizeof(Off));
    Off* off = reinterpret_cast<Off*>(offsets_of<Off>(*result)->data());
    constexpr std::int64_t GRAIN = std::int64_t{1} << 15;

    // A run of consecutive rows (a slice, a filter that kept a block) is one
    // rebased offset copy and one data block copy.
    bool contiguous = n > 0 && idx[0] >= 0;
    for (std::int64_t i = 1; contiguous && i < n; ++i)
        contiguous = idx[i] == idx[i - 1] + 1;
    if (contiguous) {
        const Off first = boff[idx[0]];
        const std::size_t total =
            static_cast<std::size_t>(boff[idx[0] + n] - first);
        dftracer::utils::dataframe::parallel_for(
            n + 1, GRAIN, [&](std::int64_t b, std::int64_t e) {
                for (std::int64_t i = b; i < e; ++i)
                    off[i] = boff[idx[0] + i] - first;
            });
        result->data = Buffer::allocate(total);
        if (total) std::memcpy(result->data->data(), bdata + first, total);
        result->validity =
            gather_validity(base, idx, n, result->null_count, info.has_neg);
        return result;
    }
    dftracer::utils::dataframe::parallel_for(
        n, GRAIN, [&](std::int64_t b, std::int64_t e) {
            for (std::int64_t i = b; i < e; ++i) {
                const std::int64_t k = idx[i];
                off[i + 1] = k >= 0 ? boff[k + 1] - boff[k] : Off{0};
            }
        });
    off[0] = 0;
    for (std::int64_t i = 0; i < n; ++i) off[i + 1] += off[i];
    const std::size_t total = static_cast<std::size_t>(off[n]);

    result->data = Buffer::allocate(total);
    char* dst = reinterpret_cast<char*>(result->data->data());
    dftracer::utils::dataframe::parallel_for(
        n, GRAIN, [&](std::int64_t b, std::int64_t e) {
            for (std::int64_t i = b; i < e; ++i) {
                const std::int64_t k = idx[i];
                if (k < 0) continue;  // negative = null: an empty slice
                const std::size_t len =
                    static_cast<std::size_t>(boff[k + 1] - boff[k]);
                if (len) std::memcpy(dst + off[i], bdata + boff[k], len);
            }
        });
    result->validity =
        gather_validity(base, idx, n, result->null_count, info.has_neg);
    return result;
}

template <class Idx>
static dftu_series* gather_varwidth(const dftu_series& base, const Idx* idx,
                                    std::int64_t n, const IdxInfo& info) {
    return is_wide_offset_type(base.type)
               ? gather_varwidth_w<std::int64_t, Idx>(base, idx, n, info)
               : gather_varwidth_w<std::int32_t, Idx>(base, idx, n, info);
}

// Struct: gather every field column by the same row indices.
template <class Idx>
static dftu_series* gather_struct(const dftu_series& base, const Idx* idx,
                                  std::int64_t n, const IdxInfo& info) {
    auto* out = new dftu_series();
    out->type = TypeId::Struct;
    out->encoding = Encoding::Flat;
    out->length = n;
    out->field_names = base.field_names;
    out->children.reserve(base.children.size());
    for (const auto& ch : base.children)
        out->children.push_back(
            std::shared_ptr<dftu_series>(gather_column(*ch, idx, n, &info)));
    out->validity =
        gather_validity(base, idx, n, out->null_count, info.has_neg);
    return out;
}

// List(/LargeList): expand the selected rows to child-row indices (each row's
// sublist), gather the child by them, and rebuild per-row offsets at the same
// width as `base`.
template <class Off, class Idx>
static dftu_series* gather_list_w(const dftu_series& base, const Idx* idx,
                                  std::int64_t n, const IdxInfo& info) {
    const Off* boff =
        reinterpret_cast<const Off*>(offsets_of<Off>(base)->data());
    std::vector<std::int64_t> child_idx;
    std::vector<Off> offsets{0};
    offsets.reserve(static_cast<std::size_t>(n) + 1);
    for (std::int64_t i = 0; i < n; ++i) {
        const std::int64_t r = idx[i];
        if (r >= 0)  // negative = null: an empty sublist
            for (Off j = boff[r]; j < boff[r + 1]; ++j) child_idx.push_back(j);
        offsets.push_back(static_cast<Off>(child_idx.size()));
    }
    auto* out = new dftu_series();
    out->type = base.type;
    out->encoding = Encoding::Flat;
    out->length = n;
    const std::size_t off_bytes = offsets.size() * sizeof(Off);
    offsets_of<Off>(*out) = Buffer::allocate(off_bytes);
    std::memcpy(offsets_of<Off>(*out)->data(), offsets.data(), off_bytes);
    out->child = std::shared_ptr<dftu_series>(
        gather_column(*base.child, child_idx.data(),
                      static_cast<std::int64_t>(child_idx.size())));
    out->validity =
        gather_validity(base, idx, n, out->null_count, info.has_neg);
    return out;
}

template <class Idx>
static dftu_series* gather_list(const dftu_series& base, const Idx* idx,
                                std::int64_t n, const IdxInfo& info) {
    return is_wide_offset_type(base.type)
               ? gather_list_w<std::int64_t, Idx>(base, idx, n, info)
               : gather_list_w<std::int32_t, Idx>(base, idx, n, info);
}

// Bool: bit-packed, so a row is a bit, not a byte. A negative index gathers a
// cleared (masked) bit.
template <class Idx>
static dftu_series* gather_bool(const dftu_series& base, const Idx* idx,
                                std::int64_t n, const IdxInfo& info) {
    auto* out = new dftu_series();
    dftracer::utils::dataframe::adopt_type_from(*out, base);
    out->encoding = Encoding::Flat;
    out->length = n;
    out->data = Buffer::allocate(buffer_bytes(TypeId::Bool, n));
    std::memset(out->data->data(), 0, out->data->size());
    const std::uint8_t* src = base.data->data();
    std::uint8_t* dst = out->data->data();
    for (std::int64_t i = 0; i < n; ++i) {
        const std::int64_t k = idx[i];
        if (k >= 0 && ((src[k >> 3] >> (k & 7)) & 1))
            dst[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
    out->validity =
        gather_validity(base, idx, n, out->null_count, info.has_neg);
    return out;
}

// Gather `n` rows of a FLAT `base` at row indices `idx`, recursively for nested
// types. Propagates validity. Returns null for a base with no gatherable
// buffer.
template <class Idx>
static dftu_series* gather_column(const dftu_series& base, const Idx* idx,
                                  std::int64_t n, const IdxInfo* known) {
    // A negative index is the null sentinel; past the end is a caller error,
    // not a read of whatever lies there.
    const IdxInfo info = known ? *known : scan_indices(idx, n);
    if (info.top >= base.length) return nullptr;
    // The identity gather (every row, in order: a join whose left side all
    // matched, a filter that kept everything) is the column itself.
    if (n == base.length && n > 0 && idx[0] == 0 && idx[n - 1] == n - 1) {
        bool identity = true;
        for (std::int64_t i = 1; identity && i < n; ++i) identity = idx[i] == i;
        if (identity) return dftu_series_share(&base);
    }
    const TypeId base_kind = narrow_varwidth_type(base.type);
    if (base_kind == TypeId::String || base_kind == TypeId::Binary)
        return gather_varwidth(base, idx, n, info);
    if (base.type == TypeId::Struct) return gather_struct(base, idx, n, info);
    if (base_kind == TypeId::List) return gather_list(base, idx, n, info);
    if (base.type == TypeId::Bool) return gather_bool(base, idx, n, info);

    const std::size_t width =
        byte_width(base.type, base.fixed_size).value_or(0);
    if (width == 0) return nullptr;

    auto* out = new dftu_series();
    dftracer::utils::dataframe::adopt_type_from(*out, base);
    out->encoding = Encoding::Flat;
    out->length = n;
    out->data = Buffer::allocate(static_cast<std::size_t>(n) * width);
    const std::uint8_t* src = base.data->data();
    std::uint8_t* dst = out->data->data();

    // Contiguous ascending indices (head/tail/slice, and any filter that keeps
    // a run of rows) collapse to a single block copy. The check short-circuits
    // on the first gap, so random indices pay only O(1) before falling through.
    bool contiguous = n > 0 && idx[0] >= 0;
    for (std::int64_t i = 1; contiguous && i < n; ++i)
        contiguous = idx[i] == idx[i - 1] + 1;
    if (contiguous && idx[n - 1] < base.length) {
        std::memcpy(dst, src + static_cast<std::size_t>(idx[0]) * width,
                    static_cast<std::size_t>(n) * width);
    } else {
        // Random gather: disjoint output slices, read-only source. Latency-
        // bound (random reads), so fanning out hides the misses. Runs serial
        // below the grain (the seam's dispatch guards small n).
        // The 8- and 4-byte widths (every Int64 / Float64 / Int32 column)
        // gather as typed loads and stores; the rest copies `width` bytes.
        auto gather_as = [&](auto tag) {
            using T = decltype(tag);
            const T* s = reinterpret_cast<const T*>(src);
            T* d = reinterpret_cast<T*>(dst);
            dftracer::utils::dataframe::parallel_for(
                n, std::int64_t{1} << 16, [&](std::int64_t b, std::int64_t e) {
                    for (std::int64_t i = b; i < e; ++i)
                        d[i] = idx[i] < 0 ? T{} : s[idx[i]];
                });
        };
        if (width == 8) {
            gather_as(std::uint64_t{});
        } else if (width == 4) {
            gather_as(std::uint32_t{});
        } else {
            dftracer::utils::dataframe::parallel_for(
                n, std::int64_t{1} << 16, [&](std::int64_t b, std::int64_t e) {
                    for (std::int64_t i = b; i < e; ++i) {
                        if (idx[i] < 0)  // negative = null: zero the cell
                            std::memset(
                                dst + static_cast<std::size_t>(i) * width, 0,
                                width);
                        else
                            std::memcpy(
                                dst + static_cast<std::size_t>(i) * width,
                                src + static_cast<std::size_t>(idx[i]) * width,
                                width);
                    }
                });
        }
    }
    out->validity =
        gather_validity(base, idx, n, out->null_count, info.has_neg);
    return out;
}

dftu_series* dftu_series_materialize(const dftu_series* v) {
    if (v->encoding == Encoding::Flat) return new dftu_series(*v);
    if ((v->encoding != Encoding::Selection &&
         v->encoding != Encoding::Dictionary) ||
        !v->child)
        return nullptr;

    const dftu_series& base = *v->child;
    if (base.encoding != Encoding::Flat) return nullptr;

    if (v->encoding == Encoding::Selection) {
        const std::int64_t* idx =
            reinterpret_cast<const std::int64_t*>(v->data->data());
        return gather_column(base, idx, v->length);
    }
    // Dictionary codes are int32 (they index a small distinct-value set);
    // the gather reads them as they are.
    const std::int32_t* codes =
        reinterpret_cast<const std::int32_t*>(v->data->data());
    return gather_column(base, codes, v->length);
}

namespace dftracer::utils::dataframe {
Series select_rows(const Series& v, const std::vector<std::int64_t>& indices) {
    return Series{make_selection(*v.handle(), indices)};
}
std::vector<Series> select_rows(const std::vector<Series>& columns,
                                const std::vector<std::int64_t>& indices) {
    std::shared_ptr<Buffer> shared = index_buffer(indices);
    const auto n = static_cast<std::int64_t>(indices.size());
    std::vector<Series> out;
    out.reserve(columns.size());
    for (const Series& c : columns)
        out.emplace_back(make_selection(*c.handle(), shared, n));
    return out;
}
}  // namespace dftracer::utils::dataframe

dftu_series* dftu_series_take(const dftu_series* v, const std::int64_t* idx,
                              std::int64_t n) {
    if (v->encoding == Encoding::Flat) return gather_column(*v, idx, n);
    // Resolve indirection to FLAT first, then gather.
    dftu_series* flat = dftu_series_materialize(v);
    if (!flat) return nullptr;
    dftu_series* out = gather_column(*flat, idx, n);
    dftu_series_free(flat);
    return out;
}

namespace {

// A null condition is false (SQL CASE: a NULL test takes the ELSE arm).
bool mask_bit(const dftu_series& m, std::int64_t i) {
    if (!is_valid(m, i)) return false;
    const std::uint8_t* b = m.data->data();
    return (b[i >> 3] & (1u << (i & 7))) != 0;
}

// Per-row pick between two FLAT columns of one type. The validity bit follows
// the picked side, so a null on the other side never leaks through.
std::shared_ptr<Buffer> where_validity(const dftu_series& m,
                                       const dftu_series& a,
                                       const dftu_series& b, std::int64_t n,
                                       std::int64_t& null_count) {
    null_count = 0;
    if (!a.validity && !b.validity) return nullptr;
    auto out = Buffer::allocate(buffer_bytes(TypeId::Bool, n));
    std::memset(out->data(), 0, out->size());
    std::uint8_t* ob = out->data();
    for (std::int64_t i = 0; i < n; ++i) {
        const bool valid = mask_bit(m, i) ? is_valid(a, i) : is_valid(b, i);
        if (valid)
            ob[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        else
            ++null_count;
    }
    return out;
}

template <class Off>
dftu_series* where_varwidth(const dftu_series& m, const dftu_series& a,
                            const dftu_series& b, std::int64_t n) {
    const Off* ao = reinterpret_cast<const Off*>(offsets_of<Off>(a)->data());
    const Off* bo = reinterpret_cast<const Off*>(offsets_of<Off>(b)->data());
    const char* ad = reinterpret_cast<const char*>(a.data->data());
    const char* bd = reinterpret_cast<const char*>(b.data->data());
    std::vector<Off> offsets{0};
    offsets.reserve(static_cast<std::size_t>(n) + 1);
    std::string out;
    for (std::int64_t i = 0; i < n; ++i) {
        if (mask_bit(m, i))
            out.append(ad + ao[i], static_cast<std::size_t>(ao[i + 1] - ao[i]));
        else
            out.append(bd + bo[i], static_cast<std::size_t>(bo[i + 1] - bo[i]));
        offsets.push_back(static_cast<Off>(out.size()));
    }
    auto* r = new dftu_series();
    dftracer::utils::dataframe::adopt_type_from(*r, a);
    r->encoding = Encoding::Flat;
    r->length = n;
    const std::size_t off_bytes = offsets.size() * sizeof(Off);
    offsets_of<Off>(*r) = Buffer::allocate(off_bytes);
    std::memcpy(offsets_of<Off>(*r)->data(), offsets.data(), off_bytes);
    r->data = Buffer::allocate(out.size());
    if (!out.empty()) std::memcpy(r->data->data(), out.data(), out.size());
    r->validity = where_validity(m, a, b, n, r->null_count);
    return r;
}

}  // namespace

dftu_series* dftu_series_where(const dftu_series* mask, const dftu_series* a,
                               const dftu_series* b) {
    if (!mask || !a || !b) return nullptr;
    if (mask->type != TypeId::Bool || mask->encoding != Encoding::Flat)
        return nullptr;
    const std::int64_t n = mask->length;
    if (a->length != n || b->length != n || a->type != b->type) return nullptr;
    dftu_series* fa = dftu_series_materialize(a);
    dftu_series* fb = dftu_series_materialize(b);
    if (!fa || !fb) {
        if (fa) dftu_series_free(fa);
        if (fb) dftu_series_free(fb);
        return nullptr;
    }
    dftu_series* out = nullptr;
    const TypeId kind = narrow_varwidth_type(fa->type);
    if (kind == TypeId::String || kind == TypeId::Binary) {
        out = is_wide_offset_type(fa->type)
                  ? where_varwidth<std::int64_t>(*mask, *fa, *fb, n)
                  : where_varwidth<std::int32_t>(*mask, *fa, *fb, n);
    } else if (fa->type == TypeId::Bool) {
        out = new dftu_series();
        dftracer::utils::dataframe::adopt_type_from(*out, *fa);
        out->encoding = Encoding::Flat;
        out->length = n;
        out->data = Buffer::allocate(buffer_bytes(TypeId::Bool, n));
        std::memset(out->data->data(), 0, out->data->size());
        std::uint8_t* dst = out->data->data();
        const std::uint8_t* ad = fa->data->data();
        const std::uint8_t* bd = fb->data->data();
        for (std::int64_t i = 0; i < n; ++i) {
            const std::uint8_t* src = mask_bit(*mask, i) ? ad : bd;
            if (src[i >> 3] & (1u << (i & 7)))
                dst[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        }
        out->validity = where_validity(*mask, *fa, *fb, n, out->null_count);
    } else if (const std::size_t width =
                   byte_width(fa->type, fa->fixed_size).value_or(0);
               width != 0 && fa->type != TypeId::Struct &&
               kind != TypeId::List) {
        out = new dftu_series();
        dftracer::utils::dataframe::adopt_type_from(*out, *fa);
        out->encoding = Encoding::Flat;
        out->length = n;
        out->data = Buffer::allocate(static_cast<std::size_t>(n) * width);
        std::uint8_t* dst = out->data->data();
        const std::uint8_t* ad = fa->data->data();
        const std::uint8_t* bd = fb->data->data();
        for (std::int64_t i = 0; i < n; ++i) {
            const std::uint8_t* src = mask_bit(*mask, i) ? ad : bd;
            std::memcpy(dst + static_cast<std::size_t>(i) * width,
                        src + static_cast<std::size_t>(i) * width, width);
        }
        out->validity = where_validity(*mask, *fa, *fb, n, out->null_count);
    }
    dftu_series_free(fa);
    dftu_series_free(fb);
    return out;
}

namespace dftracer::utils::dataframe {

Series filter(const Series& v, const Series& mask) {
    return Series{dftu_series_filter(v.handle(), mask.handle())};
}

Series materialize(const Series& v) {
    return Series{dftu_series_materialize(v.handle())};
}

Series take(const Series& v, const std::vector<std::int64_t>& indices) {
    return Series{dftu_series_take(v.handle(), indices.data(),
                                   static_cast<std::int64_t>(indices.size()))};
}

std::vector<Series> take_all(const std::vector<Series>& columns,
                             const std::int64_t* idx, std::int64_t n) {
    const IdxInfo info = scan_indices(idx, n);
    std::vector<Series> out;
    out.reserve(columns.size());
    for (const Series& c : columns) {
        const dftu_series* h = c.handle();
        if (h->encoding == Encoding::Flat) {
            out.emplace_back(gather_column(*h, idx, n, &info));
            continue;
        }
        dftu_series* flat = dftu_series_materialize(h);
        out.emplace_back(flat ? gather_column(*flat, idx, n, &info) : nullptr);
        if (flat) dftu_series_free(flat);
    }
    return out;
}

Series take32(const Series& v, const std::vector<std::int32_t>& indices) {
    const auto n = static_cast<std::int64_t>(indices.size());
    const dftu_series* h = v.handle();
    if (h->encoding == Encoding::Flat)
        return Series{gather_column(*h, indices.data(), n)};
    dftu_series* flat = dftu_series_materialize(h);
    if (!flat) return Series{};
    Series out{gather_column(*flat, indices.data(), n)};
    dftu_series_free(flat);
    return out;
}

}  // namespace dftracer::utils::dataframe
