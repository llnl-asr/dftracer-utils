// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/internal/errors.h>
#include <dftracer/utils/dataframe/internal/label_index.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/hash/xxh3.h>
#include <dftracer/utils/dstd/memory.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

// Label lookup: the hash from a label tuple to its rows that Series::locate
// and DataFrame::locate share, and the binary search of a sorted index.
namespace dftracer::utils::dataframe {

namespace {

std::atomic<std::uint64_t> g_builds{0};

constexpr double TWO_63 = 9223372036854775808.0;
constexpr double TWO_64 = 18446744073709551616.0;
constexpr std::uint8_t TAGGED = static_cast<std::uint8_t>(CellKind::Tagged);

bool bit_set(const std::uint8_t* bits, std::int64_t i) noexcept {
    return (bits[i >> 3] >> (i & 7)) & 1;
}

// A float as the cell its value is: a NaN is a null, an integral value in
// range is the integer, so 3.0 and 3 are one label.
void set_float(double d, std::uint8_t& kind, std::uint64_t& bits) noexcept {
    if (std::isnan(d)) {
        kind = static_cast<std::uint8_t>(CellKind::Null);
        bits = 0;
    } else if (d == std::floor(d) && d >= -TWO_63 && d < TWO_63) {
        kind = static_cast<std::uint8_t>(CellKind::Int);
        bits = static_cast<std::uint64_t>(static_cast<std::int64_t>(d));
    } else if (d == std::floor(d) && d >= TWO_63 && d < TWO_64) {
        kind = static_cast<std::uint8_t>(CellKind::Uint);
        bits = static_cast<std::uint64_t>(d);
    } else {
        kind = static_cast<std::uint8_t>(CellKind::Float);
        std::memcpy(&bits, &d, sizeof(bits));
    }
}

void set_signed(std::int64_t v, std::uint8_t& kind,
                std::uint64_t& bits) noexcept {
    kind = static_cast<std::uint8_t>(CellKind::Int);
    bits = static_cast<std::uint64_t>(v);
}

void set_unsigned(std::uint64_t v, std::uint8_t& kind,
                  std::uint64_t& bits) noexcept {
    kind = static_cast<std::uint8_t>(
        v > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
            ? CellKind::Uint
            : CellKind::Int);
    bits = v;
}

int sign(bool less, bool greater) noexcept {
    return less ? -1 : (greater ? 1 : 0);
}

// Int64 `a` against a double `f` that is not NaN.
int compare_int_float(std::int64_t a, double f) noexcept {
    if (f >= TWO_63) return -1;
    if (f < -TWO_63) return 1;
    const double fl = std::floor(f);
    const std::int64_t b = static_cast<std::int64_t>(fl);
    if (a != b) return a < b ? -1 : 1;
    return f > fl ? -1 : 0;
}

// A Uint64 `a` (above the Int64 range) against a double `f` that is not NaN.
int compare_uint_float(std::uint64_t a, double f) noexcept {
    if (f >= TWO_64) return -1;
    if (f < TWO_63) return 1;
    const double fl = std::floor(f);
    const std::uint64_t b = static_cast<std::uint64_t>(fl);
    if (a != b) return a < b ? -1 : 1;
    return f > fl ? -1 : 0;
}

double as_double(std::uint64_t bits) noexcept {
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

std::uint64_t combine(std::uint64_t seed, std::uint64_t cell) noexcept {
    return dstd::hash::splitmix64(seed ^ (cell + 0x9e3779b97f4a7c15ULL));
}

std::size_t pow2_at_least(std::size_t n) noexcept {
    std::size_t p = 16;
    while (p < n) p <<= 1;
    return p;
}

bool same_buffers(const dftu_series& a, const dftu_series& b) noexcept {
    return a.type == b.type && a.encoding == b.encoding &&
           a.length == b.length && a.data.data() == b.data.data() &&
           a.offsets.data() == b.offsets.data() &&
           a.validity.data() == b.validity.data() &&
           a.nested.size() == b.nested.size() &&
           (a.nested.empty() ||
            a.nested.front().series.get() == b.nested.front().series.get());
}

}  // namespace

std::uint64_t label_index_builds() noexcept {
    return g_builds.load(std::memory_order_relaxed);
}

// ---- CellColumn
// ----------------------------------------------------------------

bool CellColumn::is_bytes(CellKind k) const noexcept {
    const std::uint8_t v = static_cast<std::uint8_t>(k);
    return k == CellKind::Text || k == CellKind::Blob ||
           (v >= TAGGED && value_domain(static_cast<TypeId>(v - TAGGED)) ==
                               ValueDomain::Bytes);
}

dstd::StringView CellColumn::bytes(std::int64_t i) const {
    return read_bytes(column_, i);
}

std::uint64_t CellColumn::hash(std::int64_t i) const noexcept {
    const std::uint8_t k = kind_[static_cast<std::size_t>(i)];
    return dstd::hash::splitmix64(bits_[static_cast<std::size_t>(i)] +
                                  (static_cast<std::uint64_t>(k) << 56));
}

dstd::Result<CellColumn> CellColumn::make(const Series& column) {
    const TypeId t = column.type();
    if (value_domain(t) == ValueDomain::None)
        return invalid_argument<"labels of type {} have no hash and equality">(
            type_name(t));
    CellColumn out;
    out.column_ = column.is_flat() ? column.share() : column.materialize();
    if (!out.column_.valid())
        return invalid_argument<"labels of type {} cannot be read">(
            type_name(t));
    const Series& c = out.column_;
    const std::int64_t n = c.length();
    out.kind_.assign(static_cast<std::size_t>(n),
                     static_cast<std::uint8_t>(CellKind::Null));
    out.bits_.assign(static_cast<std::size_t>(n), 0);
    const dftu_series& raw = *c.handle();
    const std::uint8_t* valid =
        raw.validity.block() ? raw.validity.data() : nullptr;
    std::uint8_t* kind = out.kind_.data();
    std::uint64_t* bits = out.bits_.data();

    auto each = [&](auto&& cell) {
        for (std::int64_t i = 0; i < n; ++i)
            if (!valid || bit_set(valid, i)) cell(i, kind[i], bits[i]);
    };
    switch (t) {
        case TypeId::Bool: {
            const std::uint8_t* d = c.data<std::uint8_t>();
            each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                k = static_cast<std::uint8_t>(CellKind::Bool);
                b = bit_set(d, i) ? 1 : 0;
            });
            break;
        }
        case TypeId::Int8: {
            const auto* d = c.data<std::int8_t>();
            each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                set_signed(d[i], k, b);
            });
            break;
        }
        case TypeId::Int16: {
            const auto* d = c.data<std::int16_t>();
            each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                set_signed(d[i], k, b);
            });
            break;
        }
        case TypeId::Int32: {
            const auto* d = c.data<std::int32_t>();
            each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                set_signed(d[i], k, b);
            });
            break;
        }
        case TypeId::Int64: {
            const auto* d = c.data<std::int64_t>();
            each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                set_signed(d[i], k, b);
            });
            break;
        }
        case TypeId::Uint8: {
            const auto* d = c.data<std::uint8_t>();
            each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                set_unsigned(d[i], k, b);
            });
            break;
        }
        case TypeId::Uint16: {
            const auto* d = c.data<std::uint16_t>();
            each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                set_unsigned(d[i], k, b);
            });
            break;
        }
        case TypeId::Uint32: {
            const auto* d = c.data<std::uint32_t>();
            each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                set_unsigned(d[i], k, b);
            });
            break;
        }
        case TypeId::Uint64: {
            const auto* d = c.data<std::uint64_t>();
            each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                set_unsigned(d[i], k, b);
            });
            break;
        }
        case TypeId::Float32: {
            const auto* d = c.data<float>();
            each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                set_float(static_cast<double>(d[i]), k, b);
            });
            break;
        }
        case TypeId::Float64: {
            const auto* d = c.data<double>();
            each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                set_float(d[i], k, b);
            });
            break;
        }
        case TypeId::Float16:
            each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                set_float(read_f64(c, i), k, b);
            });
            break;
        case TypeId::String:
        case TypeId::LargeString:
        case TypeId::Binary:
        case TypeId::LargeBinary: {
            const bool text = t == TypeId::String || t == TypeId::LargeString;
            const auto tag = static_cast<std::uint8_t>(text ? CellKind::Text
                                                            : CellKind::Blob);
            each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                k = tag;
                b = dstd::hash::xxh3_64(c.string_at(i));
            });
            break;
        }
        default: {
            const auto tag = static_cast<std::uint8_t>(
                TAGGED + static_cast<std::uint8_t>(t));
            if (value_domain(t) == ValueDomain::Bytes) {
                each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                    k = tag;
                    b = dstd::hash::xxh3_64(read_bytes(c, i));
                });
            } else {
                each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                    k = tag;
                    b = static_cast<std::uint64_t>(read_i64(c, i));
                });
            }
        }
    }
    return out;
}

bool CellColumn::equal(const CellColumn& a, std::int64_t i, const CellColumn& b,
                       std::int64_t j) noexcept {
    const CellKind ka = a.kind(i);
    if (ka != b.kind(j) || a.bits(i) != b.bits(j)) return false;
    return !a.is_bytes(ka) || a.bytes(i) == b.bytes(j);
}

int CellColumn::compare(const CellColumn& a, std::int64_t i,
                        const CellColumn& b, std::int64_t j,
                        bool& comparable) noexcept {
    comparable = true;
    const CellKind ka = a.kind(i);
    const CellKind kb = b.kind(j);
    const std::uint64_t x = a.bits(i);
    const std::uint64_t y = b.bits(j);
    auto number = [](CellKind k) {
        return k == CellKind::Int || k == CellKind::Uint ||
               k == CellKind::Float;
    };
    if (number(ka) && number(kb)) {
        if (ka == kb) {
            switch (ka) {
                case CellKind::Int:
                    return sign(static_cast<std::int64_t>(x) <
                                    static_cast<std::int64_t>(y),
                                static_cast<std::int64_t>(x) >
                                    static_cast<std::int64_t>(y));
                case CellKind::Uint:
                    return sign(x<y, x> y);
                default:
                    return sign(as_double(x) < as_double(y),
                                as_double(x) > as_double(y));
            }
        }
        if (ka == CellKind::Int && kb == CellKind::Uint) return -1;
        if (ka == CellKind::Uint && kb == CellKind::Int) return 1;
        if (ka == CellKind::Int)
            return compare_int_float(static_cast<std::int64_t>(x),
                                     as_double(y));
        if (kb == CellKind::Int)
            return -compare_int_float(static_cast<std::int64_t>(y),
                                      as_double(x));
        if (ka == CellKind::Uint) return compare_uint_float(x, as_double(y));
        return -compare_uint_float(y, as_double(x));
    }
    if (ka != kb || ka == CellKind::Null) {
        comparable = false;
        return 0;
    }
    if (a.is_bytes(ka)) {
        const dstd::StringView p = a.bytes(i);
        const dstd::StringView q = b.bytes(j);
        const int c = p.compare(q);
        return sign(c < 0, c > 0);
    }
    return sign(static_cast<std::int64_t>(x) < static_cast<std::int64_t>(y),
                static_cast<std::int64_t>(x) > static_cast<std::int64_t>(y));
}

// ---- LabelIndex
// ----------------------------------------------------------------

dstd::Result<dstd::SharedPtr<const LabelIndex>> LabelIndex::make(
    dstd::Span<const Series> levels) {
    if (levels.empty())
        return invalid_argument<"a label index needs at least one level">();
    const std::int64_t rows = levels.front().length();
    if (rows >=
        static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))
        return invalid_argument<"{} rows are too many for a label index">(rows);
    auto out = dstd::make_shared<LabelIndex>();
    out->rows_ = rows;
    out->levels_.reserve(levels.size());
    out->sources_.reserve(levels.size());
    for (const Series& level : levels) {
        if (level.length() != rows)
            return invalid_argument<"label levels differ in length">();
        auto cells = CellColumn::make(level);
        if (!cells) return dstd::Unexpected<dstd::Error>(cells.error());
        out->levels_.push_back(std::move(*cells));
        out->sources_.push_back(level.share());
    }
    out->tables_.reserve(levels.size());
    for (std::size_t i = 0; i < levels.size(); ++i)
        out->tables_.push_back(dstd::make_unique<Slot>());
    g_builds.fetch_add(1, std::memory_order_relaxed);
    return dstd::SharedPtr<const LabelIndex>(std::move(out));
}

bool LabelIndex::matches(dstd::Span<const Series> levels) const noexcept {
    if (levels.size() != sources_.size()) return false;
    for (std::size_t i = 0; i < levels.size(); ++i)
        if (!same_buffers(*levels[i].handle(), *sources_[i].handle()))
            return false;
    return true;
}

const LabelIndex::Table& LabelIndex::table(std::size_t prefix) const {
    Slot& slot = *tables_[prefix - 1];
    std::call_once(slot.once, [&] {
        const std::size_t n = static_cast<std::size_t>(rows_);
        dstd::Vector<std::uint64_t> row_hash(n, 0);
        for (std::size_t l = 0; l < prefix; ++l)
            for (std::size_t i = 0; i < n; ++i)
                row_hash[i] = combine(
                    row_hash[i], levels_[l].hash(static_cast<std::int64_t>(i)));
        Table& t = slot.table;
        t.slots.assign(pow2_at_least(2 * n), 0);
        const std::size_t mask = t.slots.size() - 1;
        dstd::Vector<std::uint32_t> group_of(n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t h = row_hash[i];
            std::size_t at = static_cast<std::size_t>(h) & mask;
            for (;;) {
                const std::uint32_t s = t.slots[at];
                if (s == 0) {
                    t.slots[at] =
                        static_cast<std::uint32_t>(t.first.size()) + 1;
                    group_of[i] = static_cast<std::uint32_t>(t.first.size());
                    t.first.push_back(static_cast<std::int64_t>(i));
                    t.hashes.push_back(h);
                    break;
                }
                const std::uint32_t g = s - 1;
                bool same = t.hashes[g] == h;
                for (std::size_t l = 0; same && l < prefix; ++l)
                    same = CellColumn::equal(levels_[l], t.first[g], levels_[l],
                                             static_cast<std::int64_t>(i));
                if (same) {
                    group_of[i] = g;
                    break;
                }
                at = (at + 1) & mask;
            }
        }
        const std::size_t groups = t.first.size();
        t.start.assign(groups + 1, 0);
        for (std::size_t i = 0; i < n; ++i) ++t.start[group_of[i] + 1];
        for (std::size_t g = 0; g < groups; ++g) t.start[g + 1] += t.start[g];
        t.rows.resize(n);
        dstd::Vector<std::int64_t> fill(t.start.begin(), t.start.end() - 1);
        for (std::size_t i = 0; i < n; ++i)
            t.rows[static_cast<std::size_t>(fill[group_of[i]]++)] =
                static_cast<std::int64_t>(i);
    });
    return slot.table;
}

std::int64_t LabelIndex::find(const Table& t, std::uint64_t hash,
                              dstd::Span<const CellColumn> key,
                              std::int64_t key_row,
                              std::size_t prefix) const noexcept {
    const std::size_t mask = t.slots.size() - 1;
    std::size_t at = static_cast<std::size_t>(hash) & mask;
    for (;;) {
        const std::uint32_t s = t.slots[at];
        if (s == 0) return -1;
        const std::uint32_t g = s - 1;
        if (t.hashes[g] == hash) {
            bool same = true;
            for (std::size_t l = 0; same && l < prefix; ++l)
                same =
                    CellColumn::equal(levels_[l], t.first[g], key[l], key_row);
            if (same) return g;
        }
        at = (at + 1) & mask;
    }
}

namespace {

dstd::Result<dstd::Vector<CellColumn>> key_cells(dstd::Span<const Series> keys,
                                                 std::size_t levels,
                                                 std::int64_t& count) {
    if (keys.empty())
        return invalid_argument<"a label lookup needs at least one key">();
    if (keys.size() > levels)
        return invalid_argument<
            "a label lookup has {} key levels, the labels have {}">(keys.size(),
                                                                    levels);
    count = keys.front().length();
    dstd::Vector<CellColumn> out;
    out.reserve(keys.size());
    for (const Series& k : keys) {
        if (k.length() != count)
            return invalid_argument<"label keys differ in length">();
        auto cells = CellColumn::make(k);
        if (!cells) return dstd::Unexpected<dstd::Error>(cells.error());
        out.push_back(std::move(*cells));
    }
    return out;
}

}  // namespace

dstd::Result<Located> LabelIndex::locate(dstd::Span<const Series> keys) const {
    std::int64_t count = 0;
    auto cells = key_cells(keys, levels_.size(), count);
    if (!cells) return dstd::Unexpected<dstd::Error>(cells.error());
    const std::size_t prefix = cells->size();
    const Table& t = table(prefix);
    Located out;
    for (std::int64_t r = 0; r < count; ++r) {
        std::uint64_t h = 0;
        for (std::size_t l = 0; l < prefix; ++l)
            h = combine(h, (*cells)[l].hash(r));
        const std::int64_t g = find(t, h, *cells, r, prefix);
        if (g < 0) {
            out.missing.push_back(r);
            continue;
        }
        const auto from = t.rows.begin() + t.start[static_cast<std::size_t>(g)];
        const auto to =
            t.rows.begin() + t.start[static_cast<std::size_t>(g) + 1];
        out.positions.insert(out.positions.end(), from, to);
    }
    return out;
}

void LabelIndex::order() const {
    std::call_once(order_once_, [&] {
        bool inc = true;
        bool dec = true;
        for (const CellColumn& level : levels_)
            for (std::int64_t i = 0; i < rows_; ++i)
                if (level.kind(i) == CellKind::Null) inc = dec = false;
        for (std::int64_t i = 0; (inc || dec) && i + 1 < rows_; ++i) {
            // The first level in which the two rows differ decides.
            int c = 0;
            for (std::size_t l = 0; l < levels_.size() && c == 0; ++l) {
                bool ok = true;
                c = CellColumn::compare(levels_[l], i, levels_[l], i + 1, ok);
                if (!ok) inc = dec = false;
            }
            if (c > 0) inc = false;
            if (c < 0) dec = false;
        }
        increasing_ = inc;
        decreasing_ = dec && !inc;
    });
}

dstd::Result<std::int64_t> LabelIndex::bound(dstd::Span<const Series> key,
                                             dstd::StringView which) const {
    std::int64_t count = 0;
    auto cells = key_cells(key, levels_.size(), count);
    if (!cells) return dstd::Unexpected<dstd::Error>(cells.error());
    if (count != 1)
        return invalid_argument<"a label slice bound is one label">();
    const std::size_t prefix = cells->size();
    const Table& t = table(prefix);
    std::uint64_t h = 0;
    for (std::size_t l = 0; l < prefix; ++l)
        h = combine(h, (*cells)[l].hash(0));
    const std::int64_t g = find(t, h, *cells, 0, prefix);
    if (g < 0)
        return not_found<"the {} bound of a label slice is not in the index">(
            which);
    const std::size_t gi = static_cast<std::size_t>(g);
    if (t.start[gi + 1] - t.start[gi] != 1)
        return not_found<
            "the {} bound of a label slice is a label that rows repeat">(which);
    return t.rows[static_cast<std::size_t>(t.start[gi])];
}

dstd::Result<LabelSpan> LabelIndex::locate_range(
    dstd::Span<const Series> start, dstd::Span<const Series> stop) const {
    order();
    if (!increasing_ && !decreasing_) {
        if (levels_.size() > 1)
            return not_found<
                "a label slice needs a MultiIndex sorted by its levels">();
        std::int64_t from = 0;
        std::int64_t to = rows_ - 1;
        if (!start.empty()) {
            auto r = bound(start, "start");
            if (!r) return dstd::Unexpected<dstd::Error>(r.error());
            from = *r;
        }
        if (!stop.empty()) {
            auto r = bound(stop, "stop");
            if (!r) return dstd::Unexpected<dstd::Error>(r.error());
            to = *r;
        }
        return LabelSpan{from, to >= from ? to - from + 1 : 0};
    }
    std::int64_t s_count = 0;
    std::int64_t e_count = 0;
    dstd::Vector<CellColumn> s_cells;
    dstd::Vector<CellColumn> e_cells;
    if (!start.empty()) {
        auto c = key_cells(start, levels_.size(), s_count);
        if (!c) return dstd::Unexpected<dstd::Error>(c.error());
        if (s_count != 1)
            return invalid_argument<"a label slice bound is one label">();
        s_cells = std::move(*c);
    }
    if (!stop.empty()) {
        auto c = key_cells(stop, levels_.size(), e_count);
        if (!c) return dstd::Unexpected<dstd::Error>(c.error());
        if (e_count != 1)
            return invalid_argument<"a label slice bound is one label">();
        e_cells = std::move(*c);
    }
    bool incomparable = false;
    // Row against the bound over the bound's leading levels: -1, 0 or 1.
    auto against = [&](std::int64_t row,
                       dstd::Span<const CellColumn> bound_cells) {
        for (std::size_t l = 0; l < bound_cells.size(); ++l) {
            bool ok = true;
            const int c =
                CellColumn::compare(levels_[l], row, bound_cells[l], 0, ok);
            if (!ok) {
                incomparable = true;
                return 0;
            }
            if (c != 0) return c;
        }
        return 0;
    };
    // The first row for which `before(row)` is false (`before` holds for a
    // prefix of the rows).
    auto first_not = [&](auto&& before) {
        std::int64_t lo = 0;
        std::int64_t hi = rows_;
        while (lo < hi && !incomparable) {
            const std::int64_t mid = lo + (hi - lo) / 2;
            if (before(mid))
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    };
    const int dir = increasing_ ? 1 : -1;
    std::int64_t from = 0;
    std::int64_t to = rows_;
    if (!s_cells.empty())
        from = first_not(
            [&](std::int64_t r) { return against(r, s_cells) * dir < 0; });
    if (!e_cells.empty())
        to = first_not(
            [&](std::int64_t r) { return against(r, e_cells) * dir <= 0; });
    if (incomparable)
        return invalid_argument<
            "the label slice bounds do not compare with the labels">();
    return LabelSpan{from, to > from ? to - from : 0};
}

// ---- the lookups
// ---------------------------------------------------------------

namespace {

dstd::Result<dstd::SharedPtr<const LabelIndex>> index_of(
    LabelCache* cache, dstd::Span<const Series> levels) {
    if (cache) {
        std::call_once(cache->once, [&] {
            auto built = LabelIndex::make(levels);
            if (built) cache->index = std::move(*built);
        });
        if (cache->index && cache->index->matches(levels)) return cache->index;
    }
    return LabelIndex::make(levels);
}

// A positional index: the one level 0 .. rows-1 needs no hash.
dstd::Result<Located> locate_positions(std::int64_t rows,
                                       dstd::Span<const Series> keys) {
    std::int64_t count = 0;
    auto cells = key_cells(keys, 1, count);
    if (!cells) return dstd::Unexpected<dstd::Error>(cells.error());
    const CellColumn& key = cells->front();
    Located out;
    for (std::int64_t r = 0; r < count; ++r) {
        const bool whole = key.kind(r) == CellKind::Int;
        const std::int64_t v = static_cast<std::int64_t>(key.bits(r));
        if (whole && v >= 0 && v < rows)
            out.positions.push_back(v);
        else
            out.missing.push_back(r);
    }
    return out;
}

}  // namespace

dstd::Result<Located> locate_labels(LabelCache* cache,
                                    dstd::Span<const Series> levels,
                                    std::int64_t rows,
                                    dstd::Span<const Series> keys) {
    if (levels.empty()) return locate_positions(rows, keys);
    auto index = index_of(cache, levels);
    if (!index) return dstd::Unexpected<dstd::Error>(index.error());
    return (*index)->locate(keys);
}

dstd::Result<LabelSpan> locate_label_range(LabelCache* cache,
                                           dstd::Span<const Series> levels,
                                           std::int64_t rows,
                                           dstd::Span<const Series> start,
                                           dstd::Span<const Series> stop) {
    if (levels.empty()) {
        // 0 .. rows-1 is increasing and holds every integer in range: the
        // run starts at the first row not below the start and ends after the
        // last row not above the stop.
        const double n = static_cast<double>(rows);
        auto bound = [&](dstd::Span<const Series> key, bool upper,
                         std::int64_t& at) -> dstd::Result<bool> {
            std::int64_t count = 0;
            auto cells = key_cells(key, 1, count);
            if (!cells) return dstd::Unexpected<dstd::Error>(cells.error());
            if (count != 1)
                return invalid_argument<"a label slice bound is one label">();
            const CellColumn& c = cells->front();
            double d;
            switch (c.kind(0)) {
                case CellKind::Int:
                    d = static_cast<double>(
                        static_cast<std::int64_t>(c.bits(0)));
                    break;
                case CellKind::Uint:
                    d = static_cast<double>(c.bits(0));
                    break;
                case CellKind::Float:
                    d = as_double(c.bits(0));
                    break;
                default:
                    return invalid_argument<
                        "the label slice bounds do not compare with the "
                        "labels">();
            }
            const double edge = upper ? std::floor(d) + 1 : std::ceil(d);
            at = static_cast<std::int64_t>(std::fmin(std::fmax(edge, 0.0), n));
            return true;
        };
        std::int64_t from = 0;
        std::int64_t to = rows;
        if (!start.empty()) {
            auto r = bound(start, false, from);
            if (!r.has_value()) return dstd::Unexpected<dstd::Error>(r.error());
        }
        if (!stop.empty()) {
            auto r = bound(stop, true, to);
            if (!r.has_value()) return dstd::Unexpected<dstd::Error>(r.error());
        }
        return LabelSpan{from, to > from ? to - from : 0};
    }
    auto index = index_of(cache, levels);
    if (!index) return dstd::Unexpected<dstd::Error>(index.error());
    return (*index)->locate_range(start, stop);
}

}  // namespace dftracer::utils::dataframe
