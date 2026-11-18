// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/errors.h>
#include <dftracer/utils/dataframe/internal/label_index.h>
#include <dftracer/utils/dataframe/kernels/cast.h>
#include <dftracer/utils/dataframe/kernels/filter.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <utility>

// The row labels of a Series: the accessors, the row-selecting methods that
// move the labels with the rows, the alignment of two labelled columns, and
// reindex and align by label for a Series and a DataFrame.
namespace dftracer::utils::dataframe {

namespace {

struct Level {
    dstd::String name;
    Series column;
};

dstd::String default_level_name(std::int32_t i) {
    return dstd::fmt<"__index_level_{}__">(i);
}

Series positions(std::int64_t n) {
    dstd::Vector<std::int64_t> v(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) v[static_cast<std::size_t>(i)] = i;
    return Series::flat_i64(v.data(), n);
}

// The labels of `s`; a positional column is 0 .. length-1 under the default
// name, as the pandas RangeIndex.
dstd::Vector<Level> levels_of(const Series& s) {
    dstd::Vector<Level> out;
    const std::int32_t n = s.index_count();
    if (n == 0) {
        out.push_back({default_level_name(0), positions(s.length())});
        return out;
    }
    out.reserve(static_cast<std::size_t>(n));
    for (std::int32_t i = 0; i < n; ++i)
        out.push_back({dstd::String(s.index_name(i)), s.index_level(i)});
    return out;
}

// Equal row by row, a null equal to a null: the pandas index treats NaN
// labels as equal, so two columns labelled by one NaN are the same rows. The
// comparison is null where either side is, so it has as many nulls as each
// side exactly when they are null in the same rows.
bool same_values(const Series& a, const Series& b) {
    if (a.type() != b.type() || a.length() != b.length()) return false;
    if (a.null_count() != b.null_count()) return false;
    // The columns of one frame share the buffers of its index columns.
    const dftu_series& x = *a.handle();
    const dftu_series& y = *b.handle();
    if (x.encoding == Encoding::Flat && y.encoding == Encoding::Flat &&
        x.data.data() != nullptr && x.data.data() == y.data.data() &&
        x.offsets.data() == y.offsets.data() &&
        x.validity.data() == y.validity.data())
        return true;
    const Series eq{
        dftu_series_compare_series(a.handle(), b.handle(), DFTU_CMP_EQ)};
    if (!eq.valid() || eq.null_count() != a.null_count()) return false;
    if (a.null_count() == 0) return eq.all();
    const Series settled = eq.fillna(true);
    return settled.valid() && settled.all();
}

bool same_levels(dstd::Span<const Level> a, dstd::Span<const Level> b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!same_values(a[i].column, b[i].column)) return false;
    return true;
}

bool is_integer(TypeId t) {
    switch (t) {
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
            return true;
        default:
            return false;
    }
}

bool is_signed_integer(TypeId t) {
    return t == TypeId::Int8 || t == TypeId::Int16 || t == TypeId::Int32 ||
           t == TypeId::Int64;
}

bool is_unsigned_integer(TypeId t) {
    return t == TypeId::Uint8 || t == TypeId::Uint16 || t == TypeId::Uint32 ||
           t == TypeId::Uint64;
}

int integer_bits(TypeId t) {
    switch (t) {
        case TypeId::Int8:
        case TypeId::Uint8:
            return 8;
        case TypeId::Int16:
        case TypeId::Uint16:
            return 16;
        case TypeId::Int32:
        case TypeId::Uint32:
            return 32;
        default:
            return 64;
    }
}

TypeId signed_of_bits(int bits) {
    switch (bits) {
        case 8:
            return TypeId::Int8;
        case 16:
            return TypeId::Int16;
        case 32:
            return TypeId::Int32;
        default:
            return TypeId::Int64;
    }
}

// The type numeric labels of types `a` and `b` meet in, as numpy promotes the
// pandas index: the wider integer, the smallest signed type that holds a
// signed and an unsigned one, Float64 for any other mix with a float. Unknown
// when either is not a plain number (a string never meets a number).
TypeId common_label_type(TypeId a, TypeId b) {
    if (a == b) return a;
    const bool float_a = a == TypeId::Float32 || a == TypeId::Float64;
    const bool float_b = b == TypeId::Float32 || b == TypeId::Float64;
    const bool num_a = float_a || (is_integer(a) && a != TypeId::Bool);
    const bool num_b = float_b || (is_integer(b) && b != TypeId::Bool);
    if (!num_a || !num_b) return TypeId::Unknown;
    if (float_a || float_b) return TypeId::Float64;
    const bool same_sign = is_signed_integer(a) == is_signed_integer(b);
    const int bits_a = integer_bits(a);
    const int bits_b = integer_bits(b);
    if (same_sign) {
        const int bits = bits_a > bits_b ? bits_a : bits_b;
        return is_signed_integer(a) ? signed_of_bits(bits)
                                    : (bits == 8    ? TypeId::Uint8
                                       : bits == 16 ? TypeId::Uint16
                                       : bits == 32 ? TypeId::Uint32
                                                    : TypeId::Uint64);
    }
    const int signed_bits = is_signed_integer(a) ? bits_a : bits_b;
    const int unsigned_bits = is_unsigned_integer(a) ? bits_a : bits_b;
    if (unsigned_bits < signed_bits) return signed_of_bits(signed_bits);
    if (unsigned_bits >= 64) return TypeId::Float64;
    return signed_of_bits(unsigned_bits * 2);
}

// Casts the label levels of two columns to one numeric type where their types
// differ. False when a pair of levels cannot meet; `bad` is that level.
bool promote_levels(dstd::Vector<Level>& a, dstd::Vector<Level>& b,
                    std::size_t& bad) {
    for (std::size_t i = 0; i < a.size(); ++i) {
        const TypeId ta = a[i].column.type();
        const TypeId tb = b[i].column.type();
        if (ta == tb) continue;
        const TypeId common = common_label_type(ta, tb);
        bad = i;
        if (common == TypeId::Unknown) return false;
        if (ta != common) a[i].column = cast(a[i].column, common);
        if (tb != common) b[i].column = cast(b[i].column, common);
        if (!a[i].column.valid() || !b[i].column.valid()) return false;
    }
    return true;
}

// A Bool column of `n` rows, every row `value`.
Series bool_column(std::int64_t n, bool value) {
    const dstd::Vector<std::uint8_t> bytes(
        static_cast<std::size_t>((n + 7) / 8), value ? 0xFF : 0x00);
    return Series::flat(TypeId::Bool, bytes.data(), n);
}

// `joined` is `original` plus the rows the other side had alone, which are
// null: an integer column becomes Float64. A Bool column takes false in those
// rows only (the pandas logical ops), found by `present`, a column that is
// non-null on every row `original` had and null elsewhere; a null the column
// already held stays null.
Series fill_missing(Series joined, const Series& original,
                    const Series& present) {
    if (!joined.valid() || joined.null_count() <= original.null_count())
        return joined;
    if (original.type() == TypeId::Bool)
        return bool_column(joined.length(), false)
            .where(present.null_mask(), joined);
    if (is_integer(original.type())) return cast(joined, TypeId::Float64);
    return joined;
}

// The values and the labels of a column as one frame, so a frame op moves them
// together. `op` maps that frame to a Result<DataFrame>; an invalid Series
// comes back when it fails.
template <class Op>
Series through_frame(const Series& s, Op op) {
    DataFrame f;
    f.names.emplace_back("v");
    f.columns.push_back(s.without_index());
    const std::int32_t n = s.index_count();
    for (std::int32_t i = 0; i < n; ++i) {
        f.names.emplace_back(dstd::fmt<"l{}">(i));
        f.columns.push_back(s.index_level(i));
    }
    dstd::Result<DataFrame> r = op(static_cast<const DataFrame&>(f));
    if (!r) return Series{};
    DataFrame& out = *r;
    dstd::Vector<dstd::String> names;
    dstd::Vector<dstd::StringView> views;
    dstd::Vector<Series> levels;
    names.reserve(static_cast<std::size_t>(n));
    for (std::int32_t i = 0; i < n; ++i) {
        names.emplace_back(s.index_name(i));
        levels.push_back(
            std::move(out.columns[static_cast<std::size_t>(i) + 1]));
    }
    for (const dstd::String& name : names) views.emplace_back(name);
    return out.columns.front()
        .rename(s.handle()->name)
        .with_index(views, levels);
}

}  // namespace

Series Series::with_index(dstd::Span<const dstd::StringView> names,
                          dstd::Span<const Series> levels) const {
    if (names.size() != levels.size()) return Series{};
    dstd::Vector<const char*> pointers;
    dstd::Vector<std::size_t> lengths;
    dstd::Vector<const dftu_series*> handles;
    pointers.reserve(names.size());
    lengths.reserve(names.size());
    handles.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        pointers.push_back(names[i].data());
        lengths.push_back(names[i].size());
        handles.push_back(levels[i].handle());
    }
    return Series{dftu_series_with_index(
        handle_, pointers.data(), lengths.data(), handles.data(),
        static_cast<std::int32_t>(names.size()))};
}

Series Series::without_index() const {
    return Series{
        dftu_series_with_index(handle_, nullptr, nullptr, nullptr, 0)};
}

Series Series::with_index_of(const Series& source) const {
    Series out = share();
    if (out.valid() && source.valid() && source.has_index() &&
        source.length() == length())
        out.handle()->labels = source.handle()->labels;
    return out;
}

DataFrame Series::index_frame() const {
    DataFrame f;
    const std::int32_t n = index_count();
    dstd::Vector<dstd::StringView> views;
    for (std::int32_t i = 0; i < n; ++i) {
        f.names.emplace_back(index_name(i));
        f.columns.push_back(index_level(i));
        views.emplace_back(f.names.back());
    }
    if (n == 0) return f;
    return std::move(*f.with_index(views));
}

namespace {

dstd::Vector<Series> label_levels(const Series& s) {
    dstd::Vector<Series> levels;
    const std::int32_t n = s.index_count();
    levels.reserve(static_cast<std::size_t>(n));
    for (std::int32_t i = 0; i < n; ++i) levels.push_back(s.index_level(i));
    return levels;
}

}  // namespace

dstd::Result<Located> Series::locate(dstd::Span<const Series> keys) const {
    const dstd::Vector<Series> levels = label_levels(*this);
    return locate_labels(handle_->labels ? &handle_->labels->cache : nullptr,
                         levels, length(), keys);
}

dstd::Result<LabelSpan> Series::locate_range(
    dstd::Span<const Series> start, dstd::Span<const Series> stop) const {
    const dstd::Vector<Series> levels = label_levels(*this);
    return locate_label_range(
        handle_->labels ? &handle_->labels->cache : nullptr, levels, length(),
        start, stop);
}

Series Series::derived(dftu_series* result) const {
    if (result != nullptr && handle_ != nullptr) {
        result->name = handle_->name;
        result->labels =
            result->length == handle_->length ? handle_->labels : nullptr;
    }
    return Series{result};
}

Series Series::derived(dftu_series* result, const Series& other) const {
    if (result == nullptr) return Series{};
    if (handle_ != nullptr && other.handle_ != nullptr &&
        name() == other.name())
        result->name = handle_->name;
    else
        result->name = {};
    const dftu_series* from = handle_ != nullptr && handle_->labels ? handle_
                              : other.handle_ != nullptr ? other.handle_
                                                         : nullptr;
    result->labels = from != nullptr && result->length == from->length
                         ? from->labels
                         : nullptr;
    return Series{result};
}

Series Series::sort(bool descending, bool nulls_last) const {
    if (!has_index())
        return derived(dftu_series_sort_nulls(handle_, descending ? 1 : 0,
                                              nulls_last ? 1 : 0));
    return through_frame(*this, [&](const DataFrame& f) {
        return f.sort_by("v", descending, nulls_last);
    });
}

Series Series::head(std::int64_t n) const {
    if (!has_index()) return derived(dftu_series_head(handle_, n));
    return through_frame(*this, [&](const DataFrame& f) {
        return dstd::Result<DataFrame>(f.head(n));
    });
}

Series Series::tail(std::int64_t n) const {
    if (!has_index()) return derived(dftu_series_tail(handle_, n));
    return through_frame(*this, [&](const DataFrame& f) {
        return dstd::Result<DataFrame>(f.tail(n));
    });
}

Series Series::reverse() const {
    if (!has_index()) return derived(dftu_series_reverse(handle_));
    return through_frame(*this, [&](const DataFrame& f) {
        return dstd::Result<DataFrame>(f.reverse());
    });
}

Series Series::top_k(std::int64_t k) const {
    if (!has_index()) return derived(dftu_series_top_k(handle_, k));
    return through_frame(
        *this, [&](const DataFrame& f) { return f.topk("v", k, true); });
}

Series Series::bottom_k(std::int64_t k) const {
    if (!has_index()) return derived(dftu_series_bottom_k(handle_, k));
    return through_frame(
        *this, [&](const DataFrame& f) { return f.topk("v", k, false); });
}

Series Series::sample(std::int64_t n, std::uint64_t seed) const {
    if (!has_index()) return derived(dftu_series_sample(handle_, n, seed));
    return through_frame(*this, [&](const DataFrame& f) {
        return dstd::Result<DataFrame>(f.sample(n, seed));
    });
}

Series Series::take(dstd::Span<const std::int64_t> indices) const {
    if (!has_index())
        return derived(
            dftu_series_take(handle_, indices.data(),
                             static_cast<std::int64_t>(indices.size())));
    for (const std::int64_t i : indices)
        if (i >= length()) return Series{};
    return through_frame(*this, [&](const DataFrame& f) {
        return dstd::Result<DataFrame>(f.take(indices));
    });
}

Series Series::filter(const Series& mask) const {
    if (!has_index()) return derived(dftu_series_filter(handle_, mask.handle_));
    if (!mask.valid() || mask.type() != TypeId::Bool ||
        mask.length() != length())
        return Series{};
    return through_frame(*this, [&](const DataFrame& f) {
        return dstd::Result<DataFrame>(f.filter(mask));
    });
}

Series Series::drop_nulls() const {
    if (!has_index()) return derived(dftu_series_drop_nulls(handle_));
    const Series valid = without_index().valid_mask();
    return through_frame(*this, [&](const DataFrame& f) {
        return dstd::Result<DataFrame>(f.filter(valid));
    });
}

Series Series::unique(bool maintain_order) const {
    Series out =
        derived(dftu_series_unique_nulls(handle_, maintain_order ? 1 : 0));
    return out.has_index() ? out.without_index() : std::move(out);
}

bool same_labels(const Series& a, const Series& b) {
    if (a.has_index() && b.has_index() &&
        a.handle()->labels == b.handle()->labels)
        return true;
    dstd::Vector<Level> la = levels_of(a);
    dstd::Vector<Level> lb = levels_of(b);
    std::size_t bad = 0;
    return la.size() == lb.size() && promote_levels(la, lb, bad) &&
           same_levels(la, lb);
}

dstd::Result<AlignedSeries> align_labels(const Series& a, const Series& b) {
    if (!a.has_index() && !b.has_index())
        return AlignedSeries{a.share(), b.share()};
    if (a.has_index() && b.has_index() &&
        a.handle()->labels == b.handle()->labels)
        return AlignedSeries{a.share(), b.with_index_of(a)};
    dstd::Vector<Level> la = levels_of(a);
    dstd::Vector<Level> lb = levels_of(b);
    if (la.size() != lb.size())
        return invalid_argument<
            "cannot align a row index of {} levels with one of {}">(la.size(),
                                                                    lb.size());
    std::size_t bad = 0;
    if (!promote_levels(la, lb, bad))
        return invalid_argument<
            "cannot align row labels of type {} with labels of type {}">(
            type_name(a.has_index()
                          ? a.index_level(static_cast<std::int32_t>(bad)).type()
                          : TypeId::Int64),
            type_name(b.has_index()
                          ? b.index_level(static_cast<std::int32_t>(bad)).type()
                          : TypeId::Int64));
    if (same_levels(la, lb))
        return a.has_index() ? AlignedSeries{a.share(), b.with_index_of(a)}
                             : AlignedSeries{a.share(), b.without_index()};

    const std::size_t k = la.size();
    dstd::Vector<dstd::String> keys;
    DataFrame left;
    DataFrame right;
    for (std::size_t i = 0; i < k; ++i) {
        keys.push_back(dstd::fmt<"l{}">(i));
        left.names.emplace_back(keys.back());
        left.columns.push_back(la[i].column.share());
        right.names.emplace_back(keys.back());
        right.columns.push_back(lb[i].column.share());
    }
    left.names.emplace_back("va");
    left.columns.push_back(a.without_index());
    if (a.type() == TypeId::Bool) {
        left.names.emplace_back("pa");
        left.columns.push_back(bool_column(a.length(), true));
    }
    right.names.emplace_back("vb");
    right.columns.push_back(b.without_index());
    if (b.type() == TypeId::Bool) {
        right.names.emplace_back("pb");
        right.columns.push_back(bool_column(b.length(), true));
    }
    auto joined = left.join(right, keys, JoinHow::Outer, "_right", true);
    if (!joined) return joined.error();
    auto sorted = joined->sort_by_multi(keys, false, true);
    if (!sorted) return sorted.error();

    dstd::Vector<dstd::String> names;
    dstd::Vector<dstd::StringView> views;
    dstd::Vector<Series> levels;
    for (std::size_t i = 0; i < k; ++i) {
        names.push_back(la[i].name);
        levels.push_back(sorted->columns[i].share());
    }
    for (const dstd::String& name : names) views.emplace_back(name);

    std::size_t next = k;
    Series va = sorted->columns[next++].share();
    const Series pa =
        a.type() == TypeId::Bool ? sorted->columns[next++].share() : Series{};
    Series vb = sorted->columns[next++].share();
    const Series pb =
        b.type() == TypeId::Bool ? sorted->columns[next++].share() : Series{};
    va = fill_missing(std::move(va), a, pa);
    vb = fill_missing(std::move(vb), b, pb);
    if (!va.valid() || !vb.valid())
        return invalid_argument<"cannot align: widening to Float64 failed">();
    return AlignedSeries{va.rename(a.handle()->name).with_index(views, levels),
                         vb.rename(b.handle()->name).with_index(views, levels)};
}

namespace {

bool is_integer_number(TypeId t) { return is_integer(t) && t != TypeId::Bool; }

// The range of an integer type: a signed one holds [lo, hi], an unsigned one
// [0, hi].
void integer_range(TypeId t, std::int64_t& lo, std::uint64_t& hi) {
    const int bits = integer_bits(t);
    if (is_signed_integer(t)) {
        hi = bits == 64 ? static_cast<std::uint64_t>(INT64_MAX)
                        : (std::uint64_t{1} << (bits - 1)) - 1;
        lo = -static_cast<std::int64_t>(hi) - 1;
    } else {
        lo = 0;
        hi = bits == 64 ? UINT64_MAX : (std::uint64_t{1} << bits) - 1;
    }
}

// Whether the one value of `cell` converts to a column of type `to` without
// loss, as a pandas reindex fill keeps the dtype.
bool fill_fits(TypeId to, TypeId own, const CellColumn& cell) {
    const bool float_to =
        to == TypeId::Float32 || to == TypeId::Float64 || to == TypeId::Float16;
    switch (cell.kind(0)) {
        case CellKind::Int: {
            if (float_to) return true;
            if (!is_integer_number(to)) return false;
            std::int64_t lo = 0;
            std::uint64_t hi = 0;
            integer_range(to, lo, hi);
            const auto v = static_cast<std::int64_t>(cell.bits(0));
            return v >= lo && (v < 0 || static_cast<std::uint64_t>(v) <= hi);
        }
        case CellKind::Uint: {
            if (float_to) return true;
            if (!is_integer_number(to)) return false;
            std::int64_t lo = 0;
            std::uint64_t hi = 0;
            integer_range(to, lo, hi);
            return cell.bits(0) <= hi;
        }
        case CellKind::Float:
            return float_to;
        case CellKind::Bool:
            return to == TypeId::Bool;
        case CellKind::Text:
            return to == TypeId::String || to == TypeId::LargeString;
        case CellKind::Blob:
            return to == TypeId::Binary || to == TypeId::LargeBinary;
        case CellKind::Null:
            return false;
        default:
            return to == own;
    }
}

// A Bool column, bit i set where `pos[i]` names a row.
Series found_mask(dstd::Span<const std::int64_t> pos) {
    dstd::Vector<std::uint8_t> bytes((pos.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < pos.size(); ++i)
        if (pos[i] >= 0)
            bytes[i >> 3] =
                static_cast<std::uint8_t>(bytes[i >> 3] | (1u << (i & 7)));
    return Series::flat(TypeId::Bool, bytes.data(),
                        static_cast<std::int64_t>(pos.size()));
}

// `taken`, a column gathered at `pos` (-1 is a null row): where `pos` has a -1
// the null stays, or becomes the one value of `fill` (a one-row column) as
// `reindex` fills it. Without a fill an integer column that gained nulls is
// Float64, as pandas. `what` names the column in an error.
dstd::Result<Series> fill_rows(Series taken, dstd::Span<const std::int64_t> pos,
                               const Series& fill, dstd::StringView what) {
    if (!taken.valid())
        return invalid_argument<"{}: the rows cannot be gathered">(what);
    bool added = false;
    for (const std::int64_t p : pos)
        if (p < 0) {
            added = true;
            break;
        }
    if (!added) return taken;
    const TypeId type = taken.type();
    dstd::Optional<CellColumn> cell;
    if (fill.valid() && fill.length() == 1) {
        auto made = CellColumn::make(fill);
        if (!made) return dstd::Unexpected<dstd::Error>(made.error());
        if (made->kind(0) != CellKind::Null) cell = std::move(*made);
    }
    if (!cell) {
        if (!is_integer_number(type)) return taken;
        Series wide = cast(taken, TypeId::Float64);
        if (!wide.valid())
            return invalid_argument<"{}: widening to Float64 failed">(what);
        return wide;
    }
    TypeId target = type;
    if (!fill_fits(type, fill.type(), *cell)) {
        const TypeId common =
            is_integer_number(type) && cell->kind(0) != CellKind::Bool
                ? common_label_type(type, fill.type())
                : TypeId::Unknown;
        if (common == TypeId::Unknown || (cell->kind(0) != CellKind::Int &&
                                          cell->kind(0) != CellKind::Uint &&
                                          cell->kind(0) != CellKind::Float))
            return invalid_argument<
                "{}: a fill value of type {} does not fit a column of type "
                "{}">(what, type_name(fill.type()), type_name(type));
        target = common;
    }
    Series base = target == type ? std::move(taken) : cast(taken, target);
    const Series value =
        fill.type() == target ? fill.share() : cast(fill, target);
    if (!base.valid() || !value.valid())
        return invalid_argument<"{}: the fill value cannot be converted">(what);
    // A column with no nulls of its own gained exactly the -1 rows, so
    // filling its nulls is the same one pass, with no mask and no spread.
    std::int64_t missing = 0;
    for (const std::int64_t p : pos) missing += p < 0 ? 1 : 0;
    if (base.null_count() == missing && base.type() == value.type()) {
        const Series flat =
            value.is_flat() ? value.share() : value.materialize();
        Series quick;
        if (base.type() == TypeId::Int64)
            quick = base.fillna(flat.data<std::int64_t>()[0]);
        else if (base.type() == TypeId::Float64)
            quick = base.fillna(flat.data<double>()[0]);
        if (quick.valid() && quick.type() == base.type()) return quick;
    }
    const dstd::Vector<std::int64_t> zeros(pos.size(), 0);
    const Series spread = take(value, zeros);
    Series out = base.where(found_mask(pos), spread);
    if (!out.valid())
        return invalid_argument<"{}: the fill cannot be applied">(what);
    return out;
}

// An integer column that gained nulls is Float64, as pandas.
Series widen_gained(Series taken, const Series& original) {
    if (!taken.valid() || !is_integer_number(taken.type()) ||
        taken.null_count() <= original.null_count())
        return taken;
    return cast(taken, TypeId::Float64);
}

dstd::Vector<std::int64_t> positions_of(const Series& column) {
    const Series settled = column.fillna(std::int64_t{-1});
    const Series flat =
        settled.is_flat() ? settled.share() : settled.materialize();
    const auto* d = flat.data<std::int64_t>();
    return dstd::Vector<std::int64_t>(d, d + flat.length());
}

struct LabelJoin {
    dstd::Vector<dstd::String> names;
    dstd::Vector<Series> levels;
    dstd::Vector<std::int64_t> left;   // row of the left side per row, or -1
    dstd::Vector<std::int64_t> right;  // row of the right side per row, or -1
    bool same = false;                 // the labels are equal, rows stay put
};

// The join of two label sets on their rows: Outer (sorted, nulls last), Inner
// or Left (in the order of `la`). `levels` are the joined label columns.
dstd::Result<LabelJoin> join_rows(dstd::Span<const Level> la,
                                  dstd::Span<const Level> lb, JoinHow how) {
    const std::size_t k = la.size();
    dstd::Vector<dstd::String> keys;
    DataFrame left;
    DataFrame right;
    for (std::size_t i = 0; i < k; ++i) {
        keys.push_back(dstd::fmt<"l{}">(i));
        left.names.emplace_back(keys.back());
        left.columns.push_back(la[i].column.share());
        right.names.emplace_back(keys.back());
        right.columns.push_back(lb[i].column.share());
    }
    left.names.emplace_back("pa");
    left.columns.push_back(positions(la.front().column.length()));
    right.names.emplace_back("pb");
    right.columns.push_back(positions(lb.front().column.length()));
    auto joined = left.join(right, keys, how, "_right", true);
    if (!joined) return joined.error();
    DataFrame result = std::move(*joined);
    if (how == JoinHow::Outer) {
        auto sorted = result.sort_by_multi(keys, false, true);
        if (!sorted) return sorted.error();
        result = std::move(*sorted);
    }
    LabelJoin out;
    for (std::size_t i = 0; i < k; ++i)
        out.levels.push_back(result.columns[i].share());
    out.left = positions_of(result.columns[k]);
    out.right = positions_of(result.columns[k + 1]);
    return out;
}

// Joins two label sets as `Series.align(join=how)`: the shared prefix of
// align_labels (the level counts, the common numeric type) and the join.
dstd::Result<LabelJoin> join_labels(dstd::Vector<Level> la,
                                    dstd::Vector<Level> lb, JoinHow how) {
    if (how != JoinHow::Inner && how != JoinHow::Left &&
        how != JoinHow::Right && how != JoinHow::Outer)
        return invalid_argument<
            "align: join must be inner, left, right or outer">();
    if (la.size() != lb.size())
        return invalid_argument<
            "cannot align a row index of {} levels with one of {}">(la.size(),
                                                                    lb.size());
    std::size_t bad = 0;
    if (!promote_levels(la, lb, bad))
        return invalid_argument<
            "cannot align row labels of type {} with labels of type {}">(
            type_name(la[bad].column.type()), type_name(lb[bad].column.type()));
    LabelJoin out;
    for (const Level& l : la) out.names.push_back(l.name);
    if (same_levels(la, lb)) {
        out.same = true;
        return out;
    }
    auto joined = how == JoinHow::Right ? join_rows(lb, la, JoinHow::Left)
                                        : join_rows(la, lb, how);
    if (!joined) return joined.error();
    joined->names = std::move(out.names);
    if (how == JoinHow::Right) std::swap(joined->left, joined->right);
    return joined;
}

dstd::Vector<Level> frame_levels(const DataFrame& f) {
    dstd::Vector<Level> out;
    const dstd::Span<const dstd::SharedString> names = f.index_names();
    if (names.empty()) {
        out.push_back({default_level_name(0), positions(f.num_rows())});
        return out;
    }
    for (const dstd::SharedString& n : names)
        out.push_back({dstd::String(dstd::StringView(n)), f.column(n)});
    return out;
}

// The frame `f` on the rows `pos` (-1 a null row), its index columns replaced
// by `levels` named `names`.
dstd::Result<DataFrame> gathered_frame(const DataFrame& f,
                                       dstd::Span<const std::int64_t> pos,
                                       dstd::Span<const dstd::String> names,
                                       dstd::Span<const Series> levels,
                                       const Series& fill) {
    const dstd::Span<const dstd::SharedString> own = f.index_names();
    dstd::Vector<Series> data;
    dstd::Vector<dstd::StringView> data_names;
    for (std::size_t i = 0; i < f.columns.size(); ++i) {
        bool is_index = false;
        for (const dstd::SharedString& n : own)
            if (n == f.names[i]) is_index = true;
        if (is_index) continue;
        data.push_back(f.columns[i].share());
        data_names.emplace_back(f.names[i]);
    }
    for (const dstd::String& n : names)
        for (const dstd::StringView d : data_names)
            if (d == dstd::StringView(n))
                return invalid_argument<
                    "the index column {} is also a column of the frame">(n);
    const dstd::Vector<Series> taken =
        take_all(data, pos.data(), static_cast<std::int64_t>(pos.size()));
    DataFrame out;
    for (std::size_t i = 0; i < data.size(); ++i) {
        auto filled = fill_rows(taken[i].share(), pos, fill, data_names[i]);
        if (!filled) return dstd::Unexpected<dstd::Error>(filled.error());
        out.names.emplace_back(data_names[i]);
        out.columns.push_back(std::move(*filled));
    }
    dstd::Vector<dstd::StringView> views;
    for (std::size_t i = 0; i < names.size(); ++i) {
        out.names.emplace_back(dstd::StringView(names[i]));
        out.columns.push_back(levels[i].share());
        views.emplace_back(names[i]);
    }
    return out.with_index(views);
}

}  // namespace

dstd::Result<Series> Series::reindex(dstd::Span<const Series> keys,
                                     const ReindexOptions& options) const {
    return reindex(keys, options, Series{});
}

dstd::Result<Series> Series::reindex(dstd::Span<const Series> keys,
                                     const ReindexOptions& options,
                                     const Series& fill) const {
    const dstd::Vector<Series> levels = label_levels(*this);
    auto pos =
        reindex_labels(handle_->labels ? &handle_->labels->cache : nullptr,
                       levels, length(), keys, options);
    if (!pos) return dstd::Unexpected<dstd::Error>(pos.error());
    auto filled = fill_rows(dataframe::take(without_index(), *pos), *pos, fill,
                            "reindex");
    if (!filled) return dstd::Unexpected<dstd::Error>(filled.error());
    dstd::Vector<dstd::String> names;
    dstd::Vector<dstd::StringView> views;
    const std::int32_t count = static_cast<std::int32_t>(keys.size());
    for (std::int32_t i = 0; i < count; ++i)
        names.push_back(has_index() ? dstd::String(index_name(i))
                                    : default_level_name(i));
    for (const dstd::String& name : names) views.emplace_back(name);
    Series out = filled->rename(handle_->name).with_index(views, keys);
    if (!out.valid())
        return invalid_argument<"reindex: the keys cannot be row labels">();
    return out;
}

dstd::Result<DataFrame> DataFrame::reindex(dstd::Span<const Series> keys,
                                           const ReindexOptions& options,
                                           const Series& fill) const {
    dstd::Vector<Series> levels;
    for (const dstd::SharedString& n : index_names())
        levels.push_back(column(n));
    auto pos = reindex_labels(row_index ? row_index->cache.get() : nullptr,
                              levels, num_rows(), keys, options);
    if (!pos) return dstd::Unexpected<dstd::Error>(pos.error());
    dstd::Vector<dstd::String> level_names;
    for (std::size_t i = 0; i < keys.size(); ++i)
        level_names.push_back(
            row_index ? dstd::String(dstd::StringView(row_index->names[i]))
                      : default_level_name(static_cast<std::int32_t>(i)));
    return gathered_frame(*this, *pos, level_names, keys, fill);
}

dstd::Result<AlignedSeries> align_labels(const Series& a, const Series& b,
                                         JoinHow how) {
    auto joined = join_labels(levels_of(a), levels_of(b), how);
    if (!joined) return joined.error();
    if (joined->same)
        return a.has_index() ? AlignedSeries{a.share(), b.with_index_of(a)}
                             : AlignedSeries{a.share(), b.without_index()};
    dstd::Vector<dstd::StringView> views;
    for (const dstd::String& name : joined->names) views.emplace_back(name);
    Series left = widen_gained(take(a.without_index(), joined->left), a);
    Series right = widen_gained(take(b.without_index(), joined->right), b);
    left = left.valid()
               ? left.rename(a.handle()->name).with_index(views, joined->levels)
               : Series{};
    right =
        right.valid()
            ? right.rename(b.handle()->name).with_index(views, joined->levels)
            : Series{};
    if (!left.valid() || !right.valid())
        return invalid_argument<"cannot align: the rows cannot be gathered">();
    return AlignedSeries{std::move(left), std::move(right)};
}

dstd::Result<AlignedFrames> align_rows(const DataFrame& a, const DataFrame& b,
                                       JoinHow how, const Series& fill) {
    auto joined = join_labels(frame_levels(a), frame_levels(b), how);
    if (!joined) return joined.error();
    if (joined->same) return AlignedFrames{a.share(), b.share()};
    auto left =
        gathered_frame(a, joined->left, joined->names, joined->levels, fill);
    if (!left) return dstd::Unexpected<dstd::Error>(left.error());
    auto right =
        gathered_frame(b, joined->right, joined->names, joined->levels, fill);
    if (!right) return dstd::Unexpected<dstd::Error>(right.error());
    return AlignedFrames{std::move(*left), std::move(*right)};
}

}  // namespace dftracer::utils::dataframe
