// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/internal/errors.h>
#include <dftracer/utils/dataframe/internal/label_index.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dstd/hash/constants.h>
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
    return dstd::hash::splitmix64(seed ^ (cell + dstd::hash::GOLDEN_RATIO));
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

bool CellColumn::is_number(std::int64_t i) const noexcept {
    const CellKind k = kind(i);
    return k == CellKind::Int || k == CellKind::Uint || k == CellKind::Float ||
           (static_cast<std::uint8_t>(k) >= TAGGED && !is_bytes(k));
}

double CellColumn::number(std::int64_t i) const noexcept {
    const std::uint64_t x = bits(i);
    switch (kind(i)) {
        case CellKind::Uint:
            return static_cast<double>(x);
        case CellKind::Float:
            return as_double(x);
        default:
            return static_cast<double>(static_cast<std::int64_t>(x));
    }
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
    out.type_ = t;
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
                if (t == TypeId::Timestamp || t == TypeId::Duration ||
                    t == TypeId::Time32 || t == TypeId::Time64) {
                    out.has_unit_ = true;
                    out.unit_ = raw.time_unit();
                }
                each([&](std::int64_t i, std::uint8_t& k, std::uint64_t& b) {
                    k = tag;
                    b = static_cast<std::uint64_t>(read_i64(c, i));
                });
            }
        }
    }
    return out;
}

CellColumn CellColumn::copy() const {
    CellColumn out;
    out.column_ = column_.share();
    out.bits_ = bits_;
    out.kind_ = kind_;
    out.type_ = type_;
    out.has_unit_ = has_unit_;
    out.unit_ = unit_;
    return out;
}

dstd::Result<CellColumn> CellColumn::in_unit(TimeUnit to) const {
    CellColumn out = copy();
    if (!has_unit_ || to == unit_) return out;
    std::int64_t ratio = 1;
    for (int u = static_cast<int>(unit_); u < static_cast<int>(to); ++u)
        ratio *= 1000;
    const std::int64_t hi = std::numeric_limits<std::int64_t>::max() / ratio;
    const std::int64_t lo = std::numeric_limits<std::int64_t>::min() / ratio;
    for (std::size_t i = 0; i < bits_.size(); ++i) {
        if (kind_[i] == static_cast<std::uint8_t>(CellKind::Null)) continue;
        const auto v = static_cast<std::int64_t>(bits_[i]);
        if (v > hi || v < lo)
            return invalid_argument<
                "a time value does not fit 64 bits in the finer unit of the "
                "other side">();
        out.bits_[i] = static_cast<std::uint64_t>(v * ratio);
    }
    out.unit_ = to;
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

const LabelIndex::IntTable& LabelIndex::int_table() const {
    std::call_once(int_once_, [&] {
        if (levels_.size() != 1 || rows_ == 0) return;
        const CellColumn& level = levels_.front();
        const std::uint8_t kind = static_cast<std::uint8_t>(level.kind(0));
        const bool signed_ints =
            kind == static_cast<std::uint8_t>(CellKind::Int);
        const bool tagged = kind >= TAGGED && level.is_number(0);
        if (!signed_ints && !tagged) return;
        for (std::int64_t i = 1; i < rows_; ++i)
            if (static_cast<std::uint8_t>(level.kind(i)) != kind) return;
        IntTable& t = int_;
        t.kind = kind;
        t.slots.assign(pow2_at_least(2 * static_cast<std::size_t>(rows_)),
                       IntSlot{});
        const std::size_t mask = t.slots.size() - 1;
        for (std::int64_t i = 0; i < rows_; ++i) {
            const auto label = static_cast<std::int64_t>(level.bits(i));
            std::size_t at = static_cast<std::size_t>(
                                 dstd::hash::splitmix64(level.bits(i))) &
                             mask;
            while (t.slots[at].row_plus_one != 0) {
                if (t.slots[at].label == label) {
                    t.state = IntState::Duplicate;
                    return;
                }
                at = (at + 1) & mask;
            }
            t.slots[at] = IntSlot{label, static_cast<std::uint32_t>(i) + 1};
        }
        t.state = IntState::Usable;
    });
    return int_;
}

const LabelIndex::IntTable* LabelIndex::int_table_for(
    dstd::Span<const CellColumn> keys) const {
    if (levels_.size() != 1 || keys.size() != 1) return nullptr;
    const IntTable& t = int_table();
    if (t.state == IntState::Unusable) return nullptr;
    const CellColumn& key = keys.front();
    for (std::int64_t i = 0; i < key.size(); ++i)
        if (static_cast<std::uint8_t>(key.kind(i)) != t.kind) return nullptr;
    return &t;
}

Located LabelIndex::locate_cells(dstd::Span<const CellColumn> keys,
                                 std::int64_t count) const {
    Located out;
    if (const IntTable* it = int_table_for(keys);
        it != nullptr && it->state == IntState::Usable) {
        const dstd::Vector<std::int64_t> found =
            int_rows(*it, keys.front(), count);
        out.positions.reserve(found.size());
        for (std::int64_t r = 0; r < count; ++r) {
            const std::int64_t row = found[static_cast<std::size_t>(r)];
            if (row < 0)
                out.missing.push_back(r);
            else
                out.positions.push_back(row);
        }
        return out;
    }
    const std::size_t prefix = keys.size();
    const Table& t = table(prefix);
    for (std::int64_t r = 0; r < count; ++r) {
        std::uint64_t h = 0;
        for (std::size_t l = 0; l < prefix; ++l)
            h = combine(h, keys[l].hash(r));
        const std::int64_t g = find(t, h, keys, r, prefix);
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

// Brings the keys and the labels of each temporal level to one unit: a key in
// a coarser unit is multiplied up, and a key in a finer one gives a copy of
// the labels counted in that unit, so ticks are compared exactly and a key
// that is not a whole tick of the labels matches none of them. Null when the
// labels need no copy.
dstd::Result<dstd::SharedPtr<const LabelIndex>> LabelIndex::in_units(
    dstd::Span<dstd::Vector<CellColumn>* const> sets) const {
    dstd::Vector<TimeUnit> finest;
    finest.reserve(levels_.size());
    for (const CellColumn& level : levels_) finest.push_back(level.unit());
    bool rescale = false;
    for (const dstd::Vector<CellColumn>* set : sets)
        for (std::size_t l = 0; l < set->size() && l < levels_.size(); ++l) {
            const CellColumn& key = (*set)[l];
            if (!key.has_unit() || !levels_[l].has_unit() ||
                key.type() != levels_[l].type())
                continue;
            if (static_cast<int>(key.unit()) > static_cast<int>(finest[l])) {
                finest[l] = key.unit();
                rescale = true;
            }
        }
    for (dstd::Vector<CellColumn>* set : sets)
        for (std::size_t l = 0; l < set->size() && l < levels_.size(); ++l) {
            CellColumn& key = (*set)[l];
            if (!key.has_unit() || !levels_[l].has_unit() ||
                key.type() != levels_[l].type() || key.unit() == finest[l])
                continue;
            auto up = key.in_unit(finest[l]);
            if (!up) return dstd::Unexpected<dstd::Error>(up.error());
            key = std::move(*up);
        }
    if (!rescale) return dstd::SharedPtr<const LabelIndex>();
    auto out = dstd::make_shared<LabelIndex>();
    out->rows_ = rows_;
    for (std::size_t l = 0; l < levels_.size(); ++l) {
        auto up = levels_[l].in_unit(finest[l]);
        if (!up) return dstd::Unexpected<dstd::Error>(up.error());
        out->levels_.push_back(std::move(*up));
        out->sources_.push_back(sources_[l].share());
        out->tables_.push_back(dstd::make_unique<Slot>());
    }
    return dstd::SharedPtr<const LabelIndex>(std::move(out));
}

dstd::Result<Located> LabelIndex::locate(dstd::Span<const Series> keys) const {
    std::int64_t count = 0;
    auto cells = key_cells(keys, levels_.size(), count);
    if (!cells) return dstd::Unexpected<dstd::Error>(cells.error());
    dstd::Vector<CellColumn>* sets[] = {&*cells};
    auto alt = in_units(sets);
    if (!alt) return dstd::Unexpected<dstd::Error>(alt.error());
    if (*alt) return (*alt)->locate_cells(*cells, count);
    return locate_cells(*cells, count);
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

dstd::Result<std::int64_t> LabelIndex::bound_cells(
    dstd::Span<const CellColumn> keys, std::int64_t count,
    dstd::StringView which) const {
    if (count != 1)
        return invalid_argument<"a label slice bound is one label">();
    const std::size_t prefix = keys.size();
    const Table& t = table(prefix);
    std::uint64_t h = 0;
    for (std::size_t l = 0; l < prefix; ++l) h = combine(h, keys[l].hash(0));
    const std::int64_t g = find(t, h, keys, 0, prefix);
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
    std::int64_t s_count = 0;
    std::int64_t e_count = 0;
    dstd::Vector<CellColumn> s_cells;
    dstd::Vector<CellColumn> e_cells;
    if (!start.empty()) {
        auto c = key_cells(start, levels_.size(), s_count);
        if (!c) return dstd::Unexpected<dstd::Error>(c.error());
        s_cells = std::move(*c);
    }
    if (!stop.empty()) {
        auto c = key_cells(stop, levels_.size(), e_count);
        if (!c) return dstd::Unexpected<dstd::Error>(c.error());
        e_cells = std::move(*c);
    }
    dstd::Vector<CellColumn>* sets[] = {&s_cells, &e_cells};
    auto alt = in_units(sets);
    if (!alt) return dstd::Unexpected<dstd::Error>(alt.error());
    if (*alt)
        return (*alt)->locate_range_cells(s_cells, s_count, e_cells, e_count);
    return locate_range_cells(s_cells, s_count, e_cells, e_count);
}

dstd::Result<LabelSpan> LabelIndex::locate_range_cells(
    dstd::Span<const CellColumn> s_cells, std::int64_t s_count,
    dstd::Span<const CellColumn> e_cells, std::int64_t e_count) const {
    order();
    if (!increasing_ && !decreasing_) {
        if (levels_.size() > 1)
            return not_found<
                "a label slice needs a MultiIndex sorted by its levels">();
        std::int64_t from = 0;
        std::int64_t to = rows_ - 1;
        if (!s_cells.empty()) {
            auto r = bound_cells(s_cells, s_count, "start");
            if (!r) return dstd::Unexpected<dstd::Error>(r.error());
            from = *r;
        }
        if (!e_cells.empty()) {
            auto r = bound_cells(e_cells, e_count, "stop");
            if (!r) return dstd::Unexpected<dstd::Error>(r.error());
            to = *r;
        }
        return LabelSpan{from, to >= from ? to - from + 1 : 0};
    }
    if ((!s_cells.empty() && s_count != 1) ||
        (!e_cells.empty() && e_count != 1))
        return invalid_argument<"a label slice bound is one label">();
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

// The row each key row fills from, on labels that are monotonic. `before[r]`
// is the row of the nearest label before key `r` in label order (the key's own
// row when it is a label), `after[r]` the nearest after, -1 where there is
// none.
dstd::Result<dstd::Vector<std::int64_t>> LabelIndex::reindex_fill(
    dstd::Span<const CellColumn> keys, std::int64_t count,
    const ReindexOptions& options) const {
    order();
    if (!increasing_ && !decreasing_)
        return invalid_argument<
            "index must be monotonic increasing or decreasing">();
    const bool nearest = options.method == FillMethod::Nearest;
    const bool finite =
        std::isfinite(options.tolerance) || options.tolerance_is_nanos;
    if ((nearest || finite) && levels_.size() > 1)
        return invalid_argument<
            "the nearest method and a tolerance need a single-level index">();
    if (options.tolerance < 0 ||
        (options.tolerance_is_nanos && options.tolerance_nanos < 0))
        return invalid_argument<"tolerance must be non-negative">();
    if (options.tolerance_is_nanos && !levels_[0].has_unit())
        return invalid_argument<
            "a tolerance in nanoseconds needs timestamp, duration or time "
            "labels">();
    const int dir = increasing_ ? 1 : -1;
    const std::size_t depth = levels_.size();
    const std::size_t n = static_cast<std::size_t>(count);
    dstd::Vector<std::int64_t> before(n, -1);
    dstd::Vector<std::int64_t> after(n, -1);
    dstd::Vector<std::uint8_t> exact(n, 0);

    bool walk = count > 1 && count * 16 >= rows_;
    for (std::int64_t r = 1; walk && r < count; ++r) {
        for (std::size_t l = 0; l < depth; ++l) {
            bool ok = true;
            const int c = CellColumn::compare(keys[l], r - 1, keys[l], r, ok);
            if (!ok || c * dir > 0) {
                walk = false;
                break;
            }
            if (c != 0) break;
        }
    }
    // The first row not before key `k` in label order at or after row `from`,
    // -2 when the key does not compare with the labels (a null, a text against
    // numbers).
    auto lower = [&](std::int64_t k, std::int64_t from,
                     bool& is_exact) -> std::int64_t {
        is_exact = false;
        std::int64_t lo = from;
        std::int64_t hi = rows_;
        if (walk) {
            for (; lo < rows_; ++lo) {
                int c = 0;
                for (std::size_t l = 0; l < depth && c == 0; ++l) {
                    bool ok = true;
                    c = CellColumn::compare(levels_[l], lo, keys[l], k, ok);
                    if (!ok) return -2;
                }
                if (c * dir >= 0) break;
            }
            hi = lo;
        }
        while (lo < hi) {
            const std::int64_t mid = lo + (hi - lo) / 2;
            int c = 0;
            for (std::size_t l = 0; l < depth && c == 0; ++l) {
                bool ok = true;
                c = CellColumn::compare(levels_[l], mid, keys[l], k, ok);
                if (!ok) return -2;
            }
            if (c * dir < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < rows_) {
            is_exact = true;
            for (std::size_t l = 0; is_exact && l < depth; ++l)
                is_exact = CellColumn::equal(levels_[l], lo, keys[l], k);
        }
        return lo;
    };
    // Keys sorted the way the labels are: each key's row is at or after the
    // previous one, so one forward walk replaces a search per key. Checked in
    // one pass; worth it unless the keys are few against the labels.
    std::int64_t cursor = 0;
    for (std::int64_t r = 0; r < count; ++r) {
        bool is_exact = false;
        const std::int64_t at = lower(r, walk ? cursor : 0, is_exact);
        if (at == -2) {
            bool null_key = false;
            for (std::size_t l = 0; l < depth; ++l)
                null_key = null_key || keys[l].kind(r) == CellKind::Null;
            if (null_key) {
                walk = false;
                cursor = 0;
                continue;
            }
            return invalid_argument<
                "'<' not supported between the keys and the labels">();
        }
        if (walk) cursor = at;
        const std::size_t i = static_cast<std::size_t>(r);
        exact[i] = is_exact ? 1 : 0;
        before[i] = is_exact ? at : at - 1;
        after[i] = at < rows_ ? at : -1;
    }

    if (options.limit >= 0 && count > 0) {
        const char* which =
            options.method == FillMethod::Backfill ? "backfill" : "pad";
        bool monotonic = dir > 0;
        for (std::int64_t r = 1; monotonic && r < count; ++r) {
            for (std::size_t l = 0; l < depth; ++l) {
                bool ok = true;
                const int c =
                    CellColumn::compare(keys[l], r - 1, keys[l], r, ok);
                if (!ok || c > 0) monotonic = false;
                if (!ok || c != 0) break;
            }
        }
        if (count > 0 && depth > 0 && keys[0].kind(0) == CellKind::Null)
            monotonic = false;
        if (!monotonic)
            return invalid_argument<
                "limit argument for '{}' method only well-defined if index "
                "and target are monotonic">(which);
        // A row fills at most `limit` consecutive targets; an exact match
        // fills itself and counts for nothing.
        auto cap = [&](dstd::Vector<std::int64_t>& from, bool forward) {
            std::int64_t last = -1;
            std::int64_t run = 0;
            for (std::int64_t step = 0; step < count; ++step) {
                const std::size_t i =
                    static_cast<std::size_t>(forward ? step : count - 1 - step);
                if (from[i] < 0 || exact[i]) continue;
                if (from[i] == last) {
                    ++run;
                } else {
                    last = from[i];
                    run = 1;
                }
                if (run > options.limit) from[i] = -1;
            }
        };
        if (options.method != FillMethod::Backfill) cap(before, true);
        if (options.method != FillMethod::Pad) cap(after, false);
    }

    dstd::Vector<std::int64_t> out(n, -1);
    // |key - label| of key `r` and label row `row`: an exact unsigned count
    // for integer and temporal cells, a double only when a float is involved.
    struct Dist {
        bool exact;
        std::uint64_t whole;
        double inexact;
    };
    auto distance = [&](std::int64_t r, std::int64_t row) -> Dist {
        const CellKind a = levels_[0].kind(row);
        const CellKind b = keys[0].kind(r);
        auto integral = [](CellKind k) {
            return k == CellKind::Int || k == CellKind::Tagged ||
                   static_cast<std::uint8_t>(k) >= TAGGED;
        };
        if (integral(a) && integral(b)) {
            const std::uint64_t x = levels_[0].bits(row);
            const std::uint64_t y = keys[0].bits(r);
            const bool x_below =
                static_cast<std::int64_t>(x) < static_cast<std::int64_t>(y);
            return {true, x_below ? y - x : x - y, 0.0};
        }
        if (a == CellKind::Uint && b == CellKind::Uint) {
            const std::uint64_t x = levels_[0].bits(row);
            const std::uint64_t y = keys[0].bits(r);
            return {true, x < y ? y - x : x - y, 0.0};
        }
        return {false, 0,
                std::fabs(levels_[0].number(row) - keys[0].number(r))};
    };
    auto as_double = [](const Dist& d) {
        return d.exact ? static_cast<double>(d.whole) : d.inexact;
    };
    auto closer = [&](const Dist& x, const Dist& y, bool or_equal) {
        if (x.exact && y.exact)
            return or_equal ? x.whole <= y.whole : x.whole < y.whole;
        return or_equal ? as_double(x) <= as_double(y)
                        : as_double(x) < as_double(y);
    };
    // Whole counts compare to the tolerance as integers, so a nanosecond
    // timestamp or an integer above 2**53 is exact.
    std::uint64_t tolerance_whole = 0;
    if (options.tolerance_is_nanos) {
        std::uint64_t per_tick = 1;
        for (int u = static_cast<int>(levels_[0].unit()); u < 3; ++u)
            per_tick *= 1000;
        tolerance_whole =
            static_cast<std::uint64_t>(options.tolerance_nanos) / per_tick;
    } else if (finite) {
        tolerance_whole = options.tolerance >= 18446744073709551616.0
                              ? std::numeric_limits<std::uint64_t>::max()
                              : static_cast<std::uint64_t>(options.tolerance);
    }
    auto within = [&](const Dist& d) {
        if (d.exact || options.tolerance_is_nanos)
            return d.exact && d.whole <= tolerance_whole;
        return d.inexact <= options.tolerance;
    };
    if (nearest || finite) {
        for (std::int64_t r = 0; r < count; ++r) {
            const std::size_t i = static_cast<std::size_t>(r);
            if (before[i] < 0 && after[i] < 0) continue;
            if (!keys[0].is_number(r) ||
                (before[i] >= 0 && !levels_[0].is_number(before[i])) ||
                (after[i] >= 0 && !levels_[0].is_number(after[i])))
                return invalid_argument<
                    "the nearest method and a tolerance need labels that "
                    "subtract">();
        }
    }
    for (std::int64_t r = 0; r < count; ++r) {
        const std::size_t i = static_cast<std::size_t>(r);
        std::int64_t pick = -1;
        switch (options.method) {
            case FillMethod::Pad:
                pick = before[i];
                break;
            case FillMethod::Backfill:
                pick = after[i];
                break;
            default:
                if (after[i] < 0) {
                    pick = before[i];
                } else if (before[i] < 0) {
                    pick = after[i];
                } else {
                    const Dist left = distance(r, before[i]);
                    const Dist right = distance(r, after[i]);
                    pick = closer(left, right, dir < 0) ? before[i] : after[i];
                }
        }
        if (pick >= 0 && finite && !within(distance(r, pick))) pick = -1;
        out[i] = pick;
    }
    return out;
}

dstd::Result<dstd::Vector<std::int64_t>> LabelIndex::reindex(
    dstd::Span<const Series> keys, const ReindexOptions& options) const {
    std::int64_t count = 0;
    auto cells = key_cells(keys, levels_.size(), count);
    if (!cells) return dstd::Unexpected<dstd::Error>(cells.error());
    dstd::Vector<CellColumn>* sets[] = {&*cells};
    auto alt = in_units(sets);
    if (!alt) return dstd::Unexpected<dstd::Error>(alt.error());
    if (*alt) return (*alt)->reindex_cells(*cells, count, options);
    return reindex_cells(*cells, count, options);
}

// The row of each of the `count` keys in the table, -1 where a key is not a
// label. Lookups do not depend on each other, so they run in parallel.
dstd::Vector<std::int64_t> LabelIndex::int_rows(const IntTable& t,
                                                const CellColumn& key,
                                                std::int64_t count) const {
    dstd::Vector<std::int64_t> out(static_cast<std::size_t>(count), -1);
    const std::size_t mask = t.slots.size() - 1;
    parallel_for(
        count, std::int64_t{1} << 14, [&](std::int64_t from, std::int64_t to) {
            for (std::int64_t r = from; r < to; ++r) {
                const auto label = static_cast<std::int64_t>(key.bits(r));
                std::size_t at = static_cast<std::size_t>(
                                     dstd::hash::splitmix64(key.bits(r))) &
                                 mask;
                for (;;) {
                    const IntSlot& slot = t.slots[at];
                    if (slot.row_plus_one == 0) break;
                    if (slot.label == label) {
                        out[static_cast<std::size_t>(r)] =
                            slot.row_plus_one - 1;
                        break;
                    }
                    at = (at + 1) & mask;
                }
            }
        });
    return out;
}

// The row each key fills from on one level of whole-number labels that are
// monotonic (see reindex_fill): the label itself or the nearest before it for
// Pad, the nearest after it for Backfill. A key at or after the previous one
// (in label order) searches on from the previous answer.
dstd::Vector<std::int64_t> LabelIndex::int_fill_rows(const CellColumn& key,
                                                     std::int64_t count,
                                                     FillMethod method) const {
    dstd::Vector<std::int64_t> out(static_cast<std::size_t>(count), -1);
    const CellColumn& level = levels_.front();
    const bool inc = increasing_;
    const bool pad = method == FillMethod::Pad;
    const std::int64_t rows = rows_;
    parallel_for(
        count, std::int64_t{1} << 14, [&](std::int64_t from, std::int64_t to) {
            std::int64_t cursor = 0;
            std::int64_t previous = 0;
            for (std::int64_t r = from; r < to; ++r) {
                const auto k = static_cast<std::int64_t>(key.bits(r));
                auto before = [&](std::int64_t row) {
                    const auto label =
                        static_cast<std::int64_t>(level.bits(row));
                    return inc ? label < k : label > k;
                };
                const bool onward =
                    r > from && (inc ? k >= previous : k <= previous);
                std::int64_t lo = onward ? cursor : 0;
                std::int64_t hi = lo;
                std::int64_t step = 1;
                while (hi < rows && before(hi)) {
                    lo = hi + 1;
                    hi += step;
                    step <<= 1;
                }
                if (hi > rows) hi = rows;
                while (lo < hi) {
                    const std::int64_t mid = lo + (hi - lo) / 2;
                    if (before(mid))
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                cursor = lo;
                previous = k;
                const bool exact =
                    lo < rows && static_cast<std::int64_t>(level.bits(lo)) == k;
                out[static_cast<std::size_t>(r)] =
                    pad ? (exact ? lo : lo - 1) : (lo < rows ? lo : -1);
            }
        });
    return out;
}

dstd::Result<dstd::Vector<std::int64_t>> LabelIndex::reindex_cells(
    dstd::Span<const CellColumn> keys, std::int64_t count,
    const ReindexOptions& options) const {
    if (keys.size() != levels_.size())
        return invalid_argument<
            "reindex keys have {} levels, the index has {}">(keys.size(),
                                                             levels_.size());
    const std::size_t n = static_cast<std::size_t>(count);
    if (count == rows_) {
        bool same = true;
        for (std::int64_t r = 0; same && r < count; ++r)
            for (std::size_t l = 0; same && l < levels_.size(); ++l)
                same = CellColumn::equal(levels_[l], r, keys[l], r);
        if (same) {
            dstd::Vector<std::int64_t> out(n);
            for (std::size_t r = 0; r < n; ++r)
                out[r] = static_cast<std::int64_t>(r);
            return out;
        }
    }
    const bool filling = options.method != FillMethod::None;
    if (!filling && options.limit >= 0)
        return invalid_argument<
            "limit argument only valid if doing pad, backfill or nearest "
            "reindexing">();
    if (!filling &&
        (options.tolerance_is_nanos ||
         !(options.tolerance == std::numeric_limits<double>::infinity())))
        return invalid_argument<
            "tolerance argument only valid if doing pad, backfill or nearest "
            "reindexing">();
    const IntTable* typed = int_table_for(keys);
    if (typed != nullptr && typed->state == IntState::Duplicate)
        return invalid_argument<
            "cannot reindex on an axis with duplicate labels">();
    const bool use_typed = typed != nullptr && typed->state == IntState::Usable;
    const std::size_t prefix = levels_.size();
    const Table* t = nullptr;
    if (!use_typed) {
        t = &table(prefix);
        if (t->first.size() != static_cast<std::size_t>(rows_))
            return invalid_argument<
                "cannot reindex on an axis with duplicate labels">();
    }
    if (filling) {
        if (use_typed && options.method != FillMethod::Nearest &&
            options.limit < 0 && !std::isfinite(options.tolerance) &&
            !options.tolerance_is_nanos && options.tolerance >= 0) {
            order();
            if (increasing_ || decreasing_)
                return int_fill_rows(keys.front(), count, options.method);
        }
        return reindex_fill(keys, count, options);
    }
    dstd::Vector<std::int64_t> out(n, -1);
    if (use_typed) return int_rows(*typed, keys.front(), count);
    for (std::int64_t r = 0; r < count; ++r) {
        std::uint64_t h = 0;
        for (std::size_t l = 0; l < prefix; ++l)
            h = combine(h, keys[l].hash(r));
        const std::int64_t g = find(*t, h, keys, r, prefix);
        if (g >= 0)
            out[static_cast<std::size_t>(r)] =
                t->first[static_cast<std::size_t>(g)];
    }
    return out;
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

dstd::Result<dstd::Vector<std::int64_t>> reindex_labels(
    LabelCache* cache, dstd::Span<const Series> levels, std::int64_t rows,
    dstd::Span<const Series> keys, const ReindexOptions& options) {
    if (levels.empty()) {
        if (options.method == FillMethod::None && options.limit < 0 &&
            !std::isfinite(options.tolerance) && !options.tolerance_is_nanos) {
            // The labels 0 .. rows-1: a key is its own row, no hash.
            std::int64_t count = 0;
            auto cells = key_cells(keys, 1, count);
            if (!cells) return dstd::Unexpected<dstd::Error>(cells.error());
            dstd::Vector<std::int64_t> out(static_cast<std::size_t>(count), -1);
            const CellColumn& key = cells->front();
            for (std::int64_t r = 0; r < count; ++r) {
                const auto v = static_cast<std::int64_t>(key.bits(r));
                if (key.kind(r) == CellKind::Int && v >= 0 && v < rows)
                    out[static_cast<std::size_t>(r)] = v;
            }
            return out;
        }
        dstd::Vector<std::int64_t> v(static_cast<std::size_t>(rows));
        for (std::int64_t i = 0; i < rows; ++i)
            v[static_cast<std::size_t>(i)] = i;
        const Series positions[] = {Series::flat_i64(v.data(), rows)};
        auto index = LabelIndex::make(positions);
        if (!index) return dstd::Unexpected<dstd::Error>(index.error());
        return (*index)->reindex(keys, options);
    }
    auto index = index_of(cache, levels);
    if (!index) return dstd::Unexpected<dstd::Error>(index.error());
    return (*index)->reindex(keys, options);
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
