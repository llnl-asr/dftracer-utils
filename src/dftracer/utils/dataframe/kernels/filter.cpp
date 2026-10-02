#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/filter_simd.h>
#include <dftracer/utils/dataframe/internal/numeric_dispatch.h>
#include <dftracer/utils/dataframe/internal/scalar.h>
#include <dftracer/utils/dataframe/internal/varwidth_offsets.h>
#include <dftracer/utils/dataframe/kernels/filter.h>
#include <dftracer/utils/dataframe/parallel.h>

#include <algorithm>
#include <cstring>
#include <limits>
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

// The indices `idx` into a SELECTION column `sel`, resolved to rows of its
// base. A row the selection holds as null (outer fill or its own bitmap)
// becomes -1.
template <class Idx>
std::shared_ptr<Buffer> resolve_through(const dftu_series& sel, const Idx* idx,
                                        std::int64_t n) {
    const std::int64_t* inner =
        reinterpret_cast<const std::int64_t*>(sel.data->data());
    const std::uint8_t* own = sel.validity ? sel.validity->data() : nullptr;
    auto out =
        Buffer::allocate(static_cast<std::size_t>(n) * sizeof(std::int64_t));
    auto* c = reinterpret_cast<std::int64_t*>(out->data());
    for (std::int64_t i = 0; i < n; ++i) {
        const std::int64_t k = idx[i];
        c[i] =
            k < 0 || (own && !((own[k >> 3] >> (k & 7)) & 1)) ? -1 : inner[k];
    }
    return out;
}

// Build a column over `base` from row indices. A SELECTION base composes its
// indices (still zero copy over the flat root); any other non-FLAT base is
// gathered by its own layout, so a selection is always one level over a FLAT
// column. `indices` is the index buffer the selection adopts, shared between
// every column of one frame filter; a composed base gets its own.
dftu_series* make_selection(const dftu_series& base,
                            std::shared_ptr<Buffer> indices, std::int64_t n) {
    const std::int64_t* sel =
        reinterpret_cast<const std::int64_t*>(indices->data());
    if (base.encoding == Encoding::Selection && base.child())
        return make_selection(*base.child(), resolve_through(base, sel, n), n);
    if (base.encoding != Encoding::Flat) return dftu_series_take(&base, sel, n);
    auto* out = new dftu_series();
    dftracer::utils::dataframe::adopt_type_from(*out, base);
    out->encoding = Encoding::Selection;
    out->length = n;
    out->data = std::move(indices);
    out->set_child(std::make_shared<dftu_series>(base));
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

std::shared_ptr<Buffer> mask_to_indices(const dftu_series& mask,
                                        std::int64_t& count) {
    if (!mask.is_chunked()) {
        if (mask.encoding == Encoding::Flat)
            return mask_to_indices(mask.data->data(), mask.length, count);
        std::unique_ptr<dftu_series> flat(dftu_series_materialize(&mask));
        return mask_to_indices(flat->data->data(), flat->length, count);
    }
    const auto* starts =
        reinterpret_cast<const std::int64_t*>(mask.data->data());
    const std::size_t k = mask.nested.size();
    std::vector<std::shared_ptr<Buffer>> parts(k);
    std::vector<std::int64_t> counts(k + 1, 0);
    parallel_for(
        static_cast<std::int64_t>(k), 1, [&](std::int64_t b, std::int64_t e) {
            for (std::int64_t c = b; c < e; ++c) {
                const auto i = static_cast<std::size_t>(c);
                parts[i] =
                    mask_to_indices(*mask.nested[i].series, counts[i + 1]);
            }
        });
    for (std::size_t i = 0; i < k; ++i) counts[i + 1] += counts[i];
    count = counts[k];
    auto out = Buffer::allocate(static_cast<std::size_t>(count) *
                                sizeof(std::int64_t));
    auto* dst = reinterpret_cast<std::int64_t*>(out->data());
    parallel_for(
        static_cast<std::int64_t>(k), 1, [&](std::int64_t b, std::int64_t e) {
            for (std::int64_t c = b; c < e; ++c) {
                const auto i = static_cast<std::size_t>(c);
                const auto* src =
                    reinterpret_cast<const std::int64_t*>(parts[i]->data());
                for (std::int64_t j = 0; j < counts[i + 1] - counts[i]; ++j)
                    dst[counts[i] + j] = src[j] + starts[i];
            }
        });
    return out;
}

Series mask_index_column(const Series& mask) {
    std::int64_t count = 0;
    std::shared_ptr<Buffer> idx = mask_to_indices(*mask.handle(), count);
    auto* out = new dftu_series();
    out->type = TypeId::Int64;
    out->encoding = Encoding::Flat;
    out->length = count;
    out->data = std::move(idx);
    return Series{out};
}

}  // namespace dftracer::utils::dataframe

dftu_series* dftu_series_filter(const dftu_series* v, const dftu_series* mask) {
    if (mask->type != TypeId::Bool || mask->length != v->length) return nullptr;
    std::int64_t count = 0;
    std::shared_ptr<Buffer> sel =
        dftracer::utils::dataframe::mask_to_indices(*mask, count);
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
    // Whether the non-negative indices never decrease. Computed on demand,
    // for a gather of a CHUNKED column.
    bool sorted = false;
    bool sorted_known = false;
};

template <class Idx>
static bool indices_sorted(const Idx* idx, std::int64_t n) {
    struct Acc {
        std::int64_t first = -1;
        std::int64_t last = -1;
        bool sorted = true;
    };
    const Acc acc = dftracer::utils::dataframe::parallel_reduce<Acc>(
        n, std::int64_t{1} << 16, Acc{},
        [&](std::int64_t b, std::int64_t e) {
            Acc a;
            for (std::int64_t i = b; i < e; ++i) {
                const auto v = static_cast<std::int64_t>(idx[i]);
                if (v < 0) continue;
                if (a.first < 0) a.first = v;
                a.sorted &= v >= a.last;
                a.last = v;
            }
            return a;
        },
        [](Acc a, Acc b) {
            if (a.first < 0) return b;
            if (b.first < 0) return a;
            return Acc{a.first, b.last,
                       a.sorted && b.sorted && a.last <= b.first};
        });
    return acc.sorted;
}

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
    return IdxInfo{acc.top, acc.neg != 0, false, false};
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
// `Off` is the output offset width and `BOff` the base's. A narrow base whose
// gathered bytes pass INT32_MAX gathers again with `Off` int64 into a
// LargeString/LargeBinary column, so the offsets never wrap.
template <class Off, class BOff, class Idx>
static dftu_series* gather_varwidth_w(const dftu_series& base, const Idx* idx,
                                      std::int64_t n, const IdxInfo& info) {
    const BOff* boff =
        reinterpret_cast<const BOff*>(offsets_of<BOff>(base)->data());
    const char* bdata = reinterpret_cast<const char*>(base.data->data());

    auto* result = new dftu_series();
    dftracer::utils::dataframe::adopt_type_from(*result, base);
    if (sizeof(Off) > sizeof(BOff))
        result->type = base.type == TypeId::String ? TypeId::LargeString
                                                   : TypeId::LargeBinary;
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
        const BOff first = boff[idx[0]];
        const std::size_t total =
            static_cast<std::size_t>(boff[idx[0] + n] - first);
        dftracer::utils::dataframe::parallel_for(
            n + 1, GRAIN, [&](std::int64_t b, std::int64_t e) {
                for (std::int64_t i = b; i < e; ++i)
                    off[i] = static_cast<Off>(boff[idx[0] + i] - first);
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
                off[i + 1] =
                    k >= 0 ? static_cast<Off>(boff[k + 1] - boff[k]) : Off{0};
            }
        });
    if (!offsets_from_lengths(off, n)) {
        delete result;
        return nullptr;
    }
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
    if (is_wide_offset_type(base.type))
        return gather_varwidth_w<std::int64_t, std::int64_t, Idx>(base, idx, n,
                                                                  info);
    dftu_series* out =
        gather_varwidth_w<std::int32_t, std::int32_t, Idx>(base, idx, n, info);
    return out ? out
               : gather_varwidth_w<std::int64_t, std::int32_t, Idx>(base, idx,
                                                                    n, info);
}

// VIEW: copy the selected 16-byte views and share the data buffers. A
// negative index gathers an all-zero view (null).
template <class Idx>
static dftu_series* gather_view(const dftu_series& base, const Idx* idx,
                                std::int64_t n, const IdxInfo& info) {
    constexpr std::size_t VIEW_BYTES = 16;
    auto* out = new dftu_series();
    dftracer::utils::dataframe::adopt_type_from(*out, base);
    out->encoding = Encoding::View;
    out->length = n;
    out->blobs = base.blobs;
    out->data = Buffer::allocate(static_cast<std::size_t>(n) * VIEW_BYTES);
    const std::uint8_t* src = base.data->data();
    std::uint8_t* dst = out->data->data();
    dftracer::utils::dataframe::parallel_for(
        n, std::int64_t{1} << 15, [&](std::int64_t b, std::int64_t e) {
            for (std::int64_t i = b; i < e; ++i) {
                std::uint8_t* d =
                    dst + static_cast<std::size_t>(i) * VIEW_BYTES;
                if (idx[i] < 0)
                    std::memset(d, 0, VIEW_BYTES);
                else
                    std::memcpy(
                        d, src + static_cast<std::size_t>(idx[i]) * VIEW_BYTES,
                        VIEW_BYTES);
            }
        });
    out->validity =
        gather_validity(base, idx, n, out->null_count, info.has_neg);
    return out;
}

// Struct: gather every field column by the same row indices.
template <class Idx>
static dftu_series* gather_struct(const dftu_series& base, const Idx* idx,
                                  std::int64_t n, const IdxInfo& info) {
    auto* out = new dftu_series();
    out->type = TypeId::Struct;
    out->encoding = Encoding::Flat;
    out->length = n;
    out->nested.reserve(base.num_fields());
    for (std::size_t i = 0; i < base.num_fields(); ++i)
        out->nested.push_back({std::shared_ptr<dftu_series>(gather_column(
                                   *base.nested[i].series, idx, n, &info)),
                               base.nested[i].name});
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
    out->set_child(std::shared_ptr<dftu_series>(
        gather_column(*base.child(), child_idx.data(),
                      static_cast<std::int64_t>(child_idx.size()))));
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

// The chunks of a CHUNKED column joined into one column: FLAT, or for text the
// view or dictionary layout concat gives.
static dftu_series* rechunk(const dftu_series& v) {
    using dftracer::utils::dataframe::Series;
    std::vector<Series> owned;
    owned.reserve(v.nested.size());
    std::vector<const Series*> parts;
    parts.reserve(v.nested.size());
    for (const dftu_nested& c : v.nested) {
        owned.emplace_back(dftu_series_share(c.series.get()));
        parts.push_back(&owned.back());
    }
    return dftracer::utils::dataframe::concat_columns(parts).release();
}

// Non-decreasing indices over a CHUNKED column: each chunk gathers its own run
// of them, and the pieces are the chunks of the result (the piece itself when
// there is one). A negative index (a null row) stays in the run it follows.
// Null when the pieces' types differ, so the caller joins and gathers.
template <class Idx>
static dftu_series* gather_chunked_runs(const dftu_series& base, const Idx* idx,
                                        std::int64_t n, const IdxInfo& info) {
    const auto* starts =
        reinterpret_cast<const std::int64_t*>(base.data->data());
    const std::size_t chunks = base.nested.size();
    struct Run {
        std::int64_t begin, end, top;
        std::size_t chunk;
    };
    std::vector<Run> runs;
    std::size_t k = 0;
    std::int64_t begin = 0;
    std::int64_t top = -1;
    for (std::int64_t i = 0; i < n; ++i) {
        const std::int64_t v = idx[i];
        if (v < 0) continue;
        if (v >= starts[k + 1]) {
            if (i > begin) runs.push_back({begin, i, top, k});
            begin = i;
            top = -1;
            while (k + 1 < chunks && v >= starts[k + 1]) ++k;
        }
        top = v;
    }
    if (n > begin) runs.push_back({begin, n, top, k});
    if (runs.empty()) return nullptr;
    std::vector<std::shared_ptr<dftu_series>> pieces(runs.size());
    dftracer::utils::dataframe::parallel_for(
        static_cast<std::int64_t>(runs.size()), 1,
        [&](std::int64_t rb, std::int64_t re) {
            std::vector<Idx> local;
            for (std::int64_t r = rb; r < re; ++r) {
                const Run& run = runs[static_cast<std::size_t>(r)];
                const std::int64_t shift = starts[run.chunk];
                const Idx* at = idx + run.begin;
                if (shift != 0) {
                    local.resize(static_cast<std::size_t>(run.end - run.begin));
                    for (std::int64_t i = run.begin; i < run.end; ++i)
                        local[static_cast<std::size_t>(i - run.begin)] =
                            idx[i] < 0 ? idx[i]
                                       : static_cast<Idx>(idx[i] - shift);
                    at = local.data();
                }
                const IdxInfo run_info{run.top < 0 ? -1 : run.top - shift,
                                       info.has_neg, true, true};
                pieces[static_cast<std::size_t>(r)].reset(
                    gather_column(*base.nested[run.chunk].series, at,
                                  run.end - run.begin, &run_info));
            }
        });
    for (const auto& p : pieces)
        if (!p || p->type != pieces.front()->type) return nullptr;
    if (pieces.size() == 1) return new dftu_series(*pieces.front());
    return dftracer::utils::dataframe::make_chunked(std::move(pieces));
}

// The chunk holding a row of a CHUNKED column: a table keyed by the high bits
// of the row names a chunk at or before it, and a short forward walk lands on
// it (no binary search per row).
class ChunkLocator {
   public:
    explicit ChunkLocator(const dftu_series& base)
        : starts_(reinterpret_cast<const std::int64_t*>(base.data->data())) {
        const std::size_t chunks = base.nested.size();
        while ((starts_[chunks] >> shift_) >
               static_cast<std::int64_t>(chunks) * 4)
            ++shift_;
        first_.resize(static_cast<std::size_t>(starts_[chunks] >> shift_) + 1);
        for (std::size_t b = 0, c = 0; b < first_.size(); ++b) {
            while (c + 1 < chunks &&
                   starts_[c + 1] <= static_cast<std::int64_t>(b << shift_))
                ++c;
            first_[b] = static_cast<std::uint32_t>(c);
        }
    }
    std::int64_t locate(std::int64_t v, std::size_t& c) const {
        c = first_[static_cast<std::size_t>(v >> shift_)];
        while (v >= starts_[c + 1]) ++c;
        return v - starts_[c];
    }

   private:
    const std::int64_t* starts_;
    int shift_ = 0;
    std::vector<std::uint32_t> first_;
};

// Any indices over a CHUNKED String or Binary column whose chunks are FLAT
// (int32 offsets), VIEW, or DICTIONARY over either: a VIEW column whose views
// point into the chunks' own buffers, so no string bytes are copied. Null for
// any other layout.
template <class Idx>
static dftu_series* gather_chunked_text(const dftu_series& base, const Idx* idx,
                                        std::int64_t n) {
    constexpr std::size_t VIEW_BYTES = 16;
    constexpr std::int32_t VIEW_INLINE = 12;
    if (base.type != TypeId::String && base.type != TypeId::Binary)
        return nullptr;
    struct Src {
        const dftu_series* chunk = nullptr;
        const dftu_series* text = nullptr;
        const std::int32_t* codes = nullptr;
        std::int32_t blob_base = 0;
    };
    const std::size_t chunks = base.nested.size();
    // All-dictionary chunks join into one dictionary whose gather moves only
    // the int32 codes, which is cheaper than writing 16-byte views.
    bool all_dictionary = true;
    for (const dftu_nested& c : base.nested)
        all_dictionary &= c.series->encoding == Encoding::Dictionary;
    if (all_dictionary) return nullptr;
    std::vector<Src> srcs(chunks);
    auto blobs = std::make_shared<std::vector<std::shared_ptr<Buffer>>>();
    for (std::size_t c = 0; c < chunks; ++c) {
        Src& src = srcs[c];
        src.chunk = base.nested[c].series.get();
        src.text = src.chunk;
        if (src.chunk->encoding == Encoding::Dictionary) {
            if (!src.chunk->child() || !src.chunk->data) return nullptr;
            src.codes =
                reinterpret_cast<const std::int32_t*>(src.chunk->data->data());
            src.text = src.chunk->child().get();
        }
        const dftu_series& t = *src.text;
        src.blob_base = static_cast<std::int32_t>(blobs->size());
        if (t.encoding == Encoding::Flat) {
            if (!t.offsets || t.wide_offsets()) return nullptr;
            if (t.data)
                blobs->push_back(t.data);
            else
                blobs->push_back(Buffer::allocate(0));
        } else if (t.encoding == Encoding::View) {
            if (!t.data) return nullptr;
            if (t.blobs)
                for (const auto& b : *t.blobs) blobs->push_back(b);
        } else {
            return nullptr;
        }
        if (blobs->size() >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            return nullptr;
    }
    const ChunkLocator locator(base);
    auto* out = new dftu_series();
    dftracer::utils::dataframe::adopt_type_from(*out, base);
    out->encoding = Encoding::View;
    out->length = n;
    out->blobs = std::move(blobs);
    out->data = Buffer::allocate(static_cast<std::size_t>(n) * VIEW_BYTES);
    auto valid = Buffer::allocate(static_cast<std::size_t>((n + 7) / 8));
    std::uint8_t* dst = out->data->data();
    std::uint8_t* vbits = valid->data();
    constexpr std::int64_t BLOCK = std::int64_t{1} << 13;
    const std::int64_t blocks = (n + BLOCK - 1) / BLOCK;
    std::vector<std::int64_t> nulls(static_cast<std::size_t>(blocks), 0);
    auto is_null = [](const dftu_series& c, std::int64_t r) {
        return c.validity && !((c.validity->data()[r >> 3] >> (r & 7)) & 1);
    };
    dftracer::utils::dataframe::parallel_for(
        blocks, 1, [&](std::int64_t bb, std::int64_t be) {
            for (std::int64_t blk = bb; blk < be; ++blk) {
                const std::int64_t lo = blk * BLOCK;
                const std::int64_t hi = std::min(n, lo + BLOCK);
                std::memset(vbits + (lo >> 3), 0,
                            static_cast<std::size_t>((hi - lo + 7) / 8));
                std::int64_t blk_nulls = 0;
                std::size_t c = 0;
                for (std::int64_t i = lo; i < hi; ++i) {
                    std::uint8_t* d =
                        dst + static_cast<std::size_t>(i) * VIEW_BYTES;
                    std::memset(d, 0, VIEW_BYTES);
                    const std::int64_t v = static_cast<std::int64_t>(idx[i]);
                    bool present = v >= 0;
                    if (present) {
                        std::int64_t r = locator.locate(v, c);
                        const Src& src = srcs[c];
                        if (is_null(*src.chunk, r)) {
                            present = false;
                        } else {
                            if (src.codes) r = src.codes[r];
                            const dftu_series& t = *src.text;
                            if (r < 0 || is_null(t, r)) {
                                present = false;
                            } else if (t.encoding == Encoding::View) {
                                std::memcpy(d,
                                            t.data->data() +
                                                static_cast<std::size_t>(r) *
                                                    VIEW_BYTES,
                                            VIEW_BYTES);
                                std::int32_t size;
                                std::memcpy(&size, d, sizeof(size));
                                if (size > VIEW_INLINE) {
                                    std::int32_t index;
                                    std::memcpy(&index, d + 8, sizeof(index));
                                    index += src.blob_base;
                                    std::memcpy(d + 8, &index, sizeof(index));
                                }
                            } else {
                                const auto* off =
                                    reinterpret_cast<const std::int32_t*>(
                                        t.offsets->data());
                                const std::int32_t size = off[r + 1] - off[r];
                                const char* bytes =
                                    t.data ? reinterpret_cast<const char*>(
                                                 t.data->data()) +
                                                 off[r]
                                           : nullptr;
                                std::memcpy(d, &size, sizeof(size));
                                if (size <= VIEW_INLINE) {
                                    if (size > 0)
                                        std::memcpy(
                                            d + 4, bytes,
                                            static_cast<std::size_t>(size));
                                } else {
                                    std::memcpy(d + 4, bytes, 4);
                                    std::memcpy(d + 8, &src.blob_base, 4);
                                    std::memcpy(d + 12, &off[r], 4);
                                }
                            }
                        }
                    }
                    if (present)
                        vbits[i >> 3] |=
                            static_cast<std::uint8_t>(1u << (i & 7));
                    else
                        ++blk_nulls;
                }
                nulls[static_cast<std::size_t>(blk)] = blk_nulls;
            }
        });
    for (std::int64_t k : nulls) out->null_count += k;
    if (out->null_count > 0) out->validity = std::move(valid);
    return out;
}

// Any indices over a CHUNKED column of FLAT fixed-width chunks: one FLAT
// column read through the chunk lookup, with no join of the chunks. Null for
// any other layout.
template <class Idx>
static dftu_series* gather_chunked_lookup(const dftu_series& base,
                                          const Idx* idx, std::int64_t n,
                                          const IdxInfo& info) {
    // Past a quarter of the rows, joining the chunks and gathering once is
    // faster than a chunk lookup per row.
    if (n >= base.length / 4) return nullptr;
    const TypeId kind = narrow_varwidth_type(base.type);
    if (kind == TypeId::String || kind == TypeId::Binary ||
        base.type == TypeId::Struct || kind == TypeId::List ||
        base.type == TypeId::Bool)
        return nullptr;
    const std::size_t width =
        byte_width(base.type, base.fixed_size()).value_or(0);
    if (width == 0) return nullptr;
    const std::size_t chunks = base.nested.size();
    std::vector<const std::uint8_t*> data(chunks);
    std::vector<const std::uint8_t*> bitmaps(chunks);
    bool any_validity = false;
    for (std::size_t c = 0; c < chunks; ++c) {
        const dftu_series& chunk = *base.nested[c].series;
        if (chunk.encoding != Encoding::Flat || !chunk.data) return nullptr;
        data[c] = chunk.data->data();
        bitmaps[c] = chunk.validity ? chunk.validity->data() : nullptr;
        any_validity |= chunk.validity != nullptr;
    }
    const ChunkLocator locator(base);
    auto locate = [&](std::int64_t v, std::size_t& c) {
        return locator.locate(v, c);
    };

    auto* out = new dftu_series();
    dftracer::utils::dataframe::adopt_type_from(*out, base);
    out->encoding = Encoding::Flat;
    out->length = n;
    out->data = Buffer::allocate(static_cast<std::size_t>(n) * width);
    std::uint8_t* dst = out->data->data();
    constexpr std::int64_t GRAIN = std::int64_t{1} << 16;
    auto read_as = [&](auto tag) {
        using T = decltype(tag);
        T* d = reinterpret_cast<T*>(dst);
        dftracer::utils::dataframe::parallel_for(
            n, GRAIN, [&](std::int64_t b, std::int64_t e) {
                std::size_t c = 0;
                for (std::int64_t i = b; i < e; ++i) {
                    const std::int64_t v = idx[i];
                    if (v < 0) {
                        d[i] = T{};
                        continue;
                    }
                    const std::int64_t row = locate(v, c);
                    T cell;
                    std::memcpy(&cell, data[c] + row * sizeof(T), sizeof(T));
                    d[i] = cell;
                }
            });
    };
    if (width == 8) {
        read_as(std::uint64_t{});
    } else if (width == 4) {
        read_as(std::uint32_t{});
    } else {
        dftracer::utils::dataframe::parallel_for(
            n, GRAIN, [&](std::int64_t b, std::int64_t e) {
                std::size_t c = 0;
                for (std::int64_t i = b; i < e; ++i) {
                    std::uint8_t* cell =
                        dst + static_cast<std::size_t>(i) * width;
                    const std::int64_t v = idx[i];
                    if (v < 0) {
                        std::memset(cell, 0, width);
                        continue;
                    }
                    const std::int64_t row = locate(v, c);
                    std::memcpy(cell,
                                data[c] + static_cast<std::size_t>(row) * width,
                                width);
                }
            });
    }
    if (!any_validity && !info.has_neg) return out;
    auto valid = Buffer::allocate(buffer_bytes(TypeId::Bool, n));
    std::memset(valid->data(), 0, valid->size());
    std::uint8_t* ob = valid->data();
    const std::int64_t bytes = (n + 7) / 8;
    out->null_count = dftracer::utils::dataframe::parallel_reduce<std::int64_t>(
        bytes, std::int64_t{1} << 13, std::int64_t{0},
        [&](std::int64_t b0, std::int64_t b1) {
            std::int64_t nulls = 0;
            std::size_t c = 0;
            for (std::int64_t i = b0 * 8; i < std::min(n, b1 * 8); ++i) {
                bool is_valid = idx[i] >= 0;
                if (is_valid) {
                    const std::int64_t local = locate(idx[i], c);
                    is_valid = !bitmaps[c] ||
                               ((bitmaps[c][local >> 3] >> (local & 7)) & 1);
                }
                if (is_valid)
                    ob[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
                else
                    ++nulls;
            }
            return nulls;
        },
        [](std::int64_t a, std::int64_t b) { return a + b; });
    out->validity = std::move(valid);
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
    if (base.is_chunked()) {
        if (n == 0)
            return gather_column(*base.nested.front().series, idx, 0, &info);
        IdxInfo with = info;
        if (!with.sorted_known) {
            with.sorted = indices_sorted(idx, n);
            with.sorted_known = true;
        }
        dftu_series* out =
            with.sorted ? gather_chunked_runs(base, idx, n, with) : nullptr;
        if (!out && !with.sorted) out = gather_chunked_text(base, idx, n);
        if (!out && !with.sorted)
            out = gather_chunked_lookup(base, idx, n, with);
        if (out) return out;
        std::unique_ptr<dftu_series> one(rechunk(base));
        if (!one) return nullptr;
        return gather_column(*one, idx, n, &info);
    }
    if (base.encoding == Encoding::Selection && base.child()) {
        const auto resolved = resolve_through(base, idx, n);
        return gather_column(
            *base.child(),
            reinterpret_cast<const std::int64_t*>(resolved->data()), n);
    }
    if (base.encoding == Encoding::Dictionary && base.child()) {
        dftu_series codes;
        codes.type = TypeId::Int32;
        codes.encoding = Encoding::Flat;
        codes.length = base.length;
        codes.null_count = base.null_count;
        codes.data = base.data;
        codes.validity = base.validity;
        dftu_series* out = gather_column(codes, idx, n, &info);
        if (!out) return nullptr;
        dftracer::utils::dataframe::adopt_type_from(*out, base);
        out->encoding = Encoding::Dictionary;
        out->set_child(base.child());
        return out;
    }
    if (base.encoding == Encoding::View) return gather_view(base, idx, n, info);
    const TypeId base_kind = narrow_varwidth_type(base.type);
    if (base_kind == TypeId::String || base_kind == TypeId::Binary)
        return gather_varwidth(base, idx, n, info);
    if (base.type == TypeId::Struct) return gather_struct(base, idx, n, info);
    if (base_kind == TypeId::List) return gather_list(base, idx, n, info);
    if (base.type == TypeId::Bool) return gather_bool(base, idx, n, info);

    const std::size_t width =
        byte_width(base.type, base.fixed_size()).value_or(0);
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

namespace {

dftu_series* materialize_view(const dftu_series& v) {
    constexpr std::size_t VIEW_BYTES = 16;
    constexpr std::int32_t INLINE_BYTES = 12;
    const std::int64_t n = v.length;
    const std::uint8_t* views = v.data->data();
    auto size_of = [&](std::int64_t i) {
        std::int32_t size;
        std::memcpy(&size, views + static_cast<std::size_t>(i) * VIEW_BYTES,
                    sizeof(size));
        return size;
    };
    auto valid = [&](std::int64_t i) {
        return !v.validity || ((v.validity->data()[i >> 3] >> (i & 7)) & 1);
    };
    std::uint64_t total = 0;
    for (std::int64_t i = 0; i < n; ++i)
        if (valid(i)) total += static_cast<std::uint32_t>(size_of(i));
    const bool wide = total > static_cast<std::uint64_t>(INT32_MAX);

    auto* out = new dftu_series();
    dftracer::utils::dataframe::adopt_type_from(*out, v);
    if (wide)
        out->type = v.type == TypeId::String ? TypeId::LargeString
                                             : TypeId::LargeBinary;
    out->encoding = Encoding::Flat;
    out->length = n;
    out->validity = v.validity;
    out->null_count = v.null_count;
    out->data = Buffer::allocate(static_cast<std::size_t>(total));
    out->offsets =
        Buffer::allocate((static_cast<std::size_t>(n) + 1) *
                         (wide ? sizeof(std::int64_t) : sizeof(std::int32_t)));
    std::uint8_t* dst = out->data->data();
    std::uint64_t pos = 0;
    auto put_offset = [&](std::int64_t i) {
        if (wide)
            reinterpret_cast<std::int64_t*>(out->offsets->data())[i] =
                static_cast<std::int64_t>(pos);
        else
            reinterpret_cast<std::int32_t*>(out->offsets->data())[i] =
                static_cast<std::int32_t>(pos);
    };
    for (std::int64_t i = 0; i < n; ++i) {
        put_offset(i);
        if (!valid(i)) continue;
        const std::int32_t size = size_of(i);
        const std::uint8_t* view =
            views + static_cast<std::size_t>(i) * VIEW_BYTES;
        const std::uint8_t* src = view + 4;
        if (size > INLINE_BYTES) {
            std::int32_t index, offset;
            std::memcpy(&index, view + 8, sizeof(index));
            std::memcpy(&offset, view + 12, sizeof(offset));
            src = (*v.blobs)[static_cast<std::size_t>(index)]->data() + offset;
        }
        if (size > 0)
            std::memcpy(dst + pos, src, static_cast<std::size_t>(size));
        pos += static_cast<std::uint32_t>(size);
    }
    put_offset(n);
    return out;
}

}  // namespace

dftu_series* dftu_series_materialize(const dftu_series* v) {
    if (v->encoding == Encoding::Flat) return new dftu_series(*v);
    if (v->is_chunked()) {
        std::unique_ptr<dftu_series> one(rechunk(*v));
        if (!one) return nullptr;
        if (one->encoding == Encoding::Flat) return one.release();
        return dftu_series_materialize(one.get());
    }
    if (v->encoding == Encoding::View) return materialize_view(*v);
    if ((v->encoding != Encoding::Selection &&
         v->encoding != Encoding::Dictionary) ||
        !v->child())
        return nullptr;

    const dftu_series& base = *v->child();
    if (base.encoding != Encoding::Flat) {
        std::shared_ptr<dftu_series> flat_base(dftu_series_materialize(&base));
        if (!flat_base) return nullptr;
        dftu_series with_flat_base(*v);
        with_flat_base.set_child(std::move(flat_base));
        return dftu_series_materialize(&with_flat_base);
    }

    // A null row of the view itself (a dictionary's code for a null value) is
    // null in the result, whatever the base entry the code points at holds.
    auto with_own_nulls = [&](dftu_series* out) {
        if (!out || !v->validity) return out;
        const std::int64_t n = out->length;
        const std::size_t bytes = static_cast<std::size_t>((n + 7) / 8);
        auto bits = Buffer::allocate(bytes);
        const std::uint8_t* own = v->validity->data();
        const std::uint8_t* gathered =
            out->validity ? out->validity->data() : nullptr;
        for (std::size_t k = 0; k < bytes; ++k)
            bits->data()[k] = static_cast<std::uint8_t>(
                (gathered ? gathered[k] : 0xFF) & own[k]);
        std::int64_t valid = 0;
        for (std::int64_t i = 0; i < n; ++i)
            valid += (bits->data()[i >> 3] >> (i & 7)) & 1;
        out->validity = std::move(bits);
        out->null_count = n - valid;
        return out;
    };

    if (v->encoding == Encoding::Selection) {
        const std::int64_t* idx =
            reinterpret_cast<const std::int64_t*>(v->data->data());
        return with_own_nulls(gather_column(base, idx, v->length));
    }
    // Dictionary codes are int32 (they index a small distinct-value set);
    // the gather reads them as they are.
    const std::int32_t* codes =
        reinterpret_cast<const std::int32_t*>(v->data->data());
    return with_own_nulls(gather_column(base, codes, v->length));
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
    return gather_column(*v, idx, n);
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

template <class Off, class BOff>
dftu_series* where_varwidth(const dftu_series& m, const dftu_series& a,
                            const dftu_series& b, std::int64_t n) {
    const BOff* ao = reinterpret_cast<const BOff*>(offsets_of<BOff>(a)->data());
    const BOff* bo = reinterpret_cast<const BOff*>(offsets_of<BOff>(b)->data());
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
        if (sizeof(Off) == sizeof(std::int32_t) &&
            !fits_int32_offsets(out.size()))
            return nullptr;
        offsets.push_back(static_cast<Off>(out.size()));
    }
    auto* r = new dftu_series();
    dftracer::utils::dataframe::adopt_type_from(*r, a);
    if (sizeof(Off) > sizeof(BOff))
        r->type = a.type == TypeId::String ? TypeId::LargeString
                                           : TypeId::LargeBinary;
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
    DFTU_FLAT_OPERAND(mask, flat_m, dftu_series_where(flat_m, a, b));
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
        if (is_wide_offset_type(fa->type)) {
            out =
                where_varwidth<std::int64_t, std::int64_t>(*mask, *fa, *fb, n);
        } else {
            out =
                where_varwidth<std::int32_t, std::int32_t>(*mask, *fa, *fb, n);
            if (!out)
                out = where_varwidth<std::int64_t, std::int32_t>(*mask, *fa,
                                                                 *fb, n);
        }
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
                   byte_width(fa->type, fa->fixed_size()).value_or(0);
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
    IdxInfo info = scan_indices(idx, n);
    for (const Series& c : columns)
        if (c.handle()->is_chunked() && !info.sorted_known) {
            info.sorted = indices_sorted(idx, n);
            info.sorted_known = true;
        }
    std::vector<Series> out;
    out.reserve(columns.size());
    for (const Series& c : columns)
        out.emplace_back(gather_column(*c.handle(), idx, n, &info));
    return out;
}

Series take32(const Series& v, const std::vector<std::int32_t>& indices) {
    return Series{gather_column(*v.handle(), indices.data(),
                                static_cast<std::int64_t>(indices.size()))};
}

}  // namespace dftracer::utils::dataframe
