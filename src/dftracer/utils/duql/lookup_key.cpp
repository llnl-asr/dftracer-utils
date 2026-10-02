#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/duql/lookup.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace dftracer::utils::duql {

namespace {

constexpr char INT_TAG = 'i';
constexpr char DOUBLE_TAG = 'd';
constexpr char STRING_TAG = 's';
constexpr char BOOL_TAG = 'b';

void put_u64(std::string& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i)
        out += static_cast<char>((v >> (i * 8)) & 0xFF);
}

std::uint64_t get_u64(std::string_view s) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) |
            static_cast<unsigned char>(s[static_cast<std::size_t>(i)]);
    return v;
}

}  // namespace

void append_int_key(std::string& out, bool negative, std::uint64_t magnitude) {
    out += INT_TAG;
    out += negative && magnitude != 0 ? '-' : '+';
    put_u64(out, magnitude);
}

bool append_double_key(std::string& out, double v) {
    if (std::isnan(v)) return false;
    if (std::trunc(v) == v && std::abs(v) < 18446744073709551616.0) {
        const bool neg = v < 0;
        append_int_key(out, neg, static_cast<std::uint64_t>(neg ? -v : v));
        return true;
    }
    out += DOUBLE_TAG;
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    put_u64(out, bits);
    return true;
}

void append_string_key(std::string& out, std::string_view v) {
    out += STRING_TAG;
    put_u64(out, v.size());
    out += v;
}

std::optional<std::string_view> string_key_text(std::string_view key) {
    if (key.size() < 9 || key[0] != STRING_TAG) return std::nullopt;
    return key.substr(9);
}

void append_bool_key(std::string& out, bool v) {
    out += BOOL_TAG;
    out += v ? '1' : '0';
}

std::string key_text(std::string_view key) {
    std::string out;
    std::size_t parts = 0;
    while (!key.empty()) {
        if (parts++ > 0) out += ", ";
        const char tag = key[0];
        key.remove_prefix(1);
        if (tag == INT_TAG) {
            if (key[0] == '-') out += '-';
            out += std::to_string(get_u64(key.substr(1)));
            key.remove_prefix(9);
        } else if (tag == DOUBLE_TAG) {
            const std::uint64_t bits = get_u64(key);
            double d = 0;
            std::memcpy(&d, &bits, sizeof d);
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.17g", d);
            out += buf;
            key.remove_prefix(8);
        } else if (tag == STRING_TAG) {
            const auto n = static_cast<std::size_t>(get_u64(key));
            out += '"';
            out += key.substr(8, n);
            out += '"';
            key.remove_prefix(8 + n);
        } else {
            out += key[0] == '1' ? "true" : "false";
            key.remove_prefix(1);
        }
    }
    return parts > 1 ? "(" + out + ")" : out;
}

const std::vector<std::int64_t>* LookupTable::find(std::string_view key) const {
    const auto it = rows.find(key);
    return it == rows.end() ? nullptr : &it->second;
}

void conflict(const LookupTable& table, std::string_view column,
              std::string_view key) {
    throw DFTUtilsException(
        ErrorCode::INVALID_ARGUMENT,
        "View::duql '" + table.name + "' has rows with different values of '" +
            std::string(column) + "' for key " + key_text(key) +
            "; pick one with a narrower let, or read every row with "
            "'lookup ... into'");
}

void scalar_conflict(std::string_view name, std::string_view key,
                     std::size_t rows) {
    throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                            "View::duql the sub-query '" + std::string(name) +
                                "' gives " + std::to_string(rows) +
                                " rows for key " + key_text(key) +
                                "; a sub-query in an expression gives one row");
}

namespace {

int kind_of(const Ordered& v) {
    return v.index() < 3 ? 0 : static_cast<int>(v.index()) - 2;
}

RangeIndex::Node combine(const RangeIndex& ix, RangeRead read,
                         const RangeIndex::Node& a, const RangeIndex::Node& b) {
    RangeIndex::Node out;
    out.isum = a.isum + b.isum;
    out.fsum = a.fsum + b.fsum;
    out.count = a.count + b.count;
    if (a.best < 0 || b.best < 0) {
        out.best = a.best < 0 ? b.best : a.best;
    } else {
        const int c =
            compare_ordered(*ix.args[static_cast<std::size_t>(a.best)],
                            *ix.args[static_cast<std::size_t>(b.best)]);
        out.best = (read == RangeRead::MIN ? c <= 0 : c >= 0) ? a.best : b.best;
    }
    return out;
}

// The first position of [first, last) whose value is not before `v` (or,
// with `after`, is after it) in the order of compare_ordered.
std::int64_t position(const RangeIndex& ix, std::int64_t first,
                      std::int64_t last, const Ordered& v, bool after) {
    const auto begin = ix.values.begin();
    auto it = after ? std::upper_bound(begin + first, begin + last, v,
                                       [](const Ordered& x, const Ordered& y) {
                                           return compare_ordered(x, y) < 0;
                                       })
                    : std::lower_bound(begin + first, begin + last, v,
                                       [](const Ordered& x, const Ordered& y) {
                                           return compare_ordered(x, y) < 0;
                                       });
    return it - begin;
}

// The positions of [first, last) of the kind of `v`.
std::pair<std::int64_t, std::int64_t> kind_range(const RangeIndex& ix,
                                                 std::int64_t first,
                                                 std::int64_t last,
                                                 const Ordered& v) {
    const auto begin = ix.values.begin();
    const int k = kind_of(v);
    const auto r = std::equal_range(
        begin + first, begin + last, k, [](const auto& x, const auto& y) {
            if constexpr (std::is_same_v<std::decay_t<decltype(x)>, int>)
                return x < kind_of(y);
            else
                return kind_of(x) < y;
        });
    return {r.first - begin, r.second - begin};
}

[[noreturn]] void range_fail(const std::string& why) {
    throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT, "View::duql " + why);
}

}  // namespace

int compare_ordered(const Ordered& a, const Ordered& b) {
    const int ka = kind_of(a);
    const int kb = kind_of(b);
    if (ka != kb) return ka < kb ? -1 : 1;
    return std::visit(
        [](const auto& x, const auto& y) -> int {
            using A = std::decay_t<decltype(x)>;
            using B = std::decay_t<decltype(y)>;
            if constexpr (std::is_same_v<A, std::string_view> ||
                          std::is_same_v<B, std::string_view> ||
                          std::is_same_v<A, bool> || std::is_same_v<B, bool>) {
                if constexpr (std::is_same_v<A, B>)
                    return x < y ? -1 : (y < x ? 1 : 0);
                else
                    return 0;
            } else {
                return compare_numbers(x, y).value_or(0);
            }
        },
        a, b);
}

void build_range_tree(RangeIndex& ix, RangeRead read,
                      const std::vector<RangeIndex::Node>& leaves) {
    ix.tree.assign(2 * leaves.size(), {});
    for (const auto& [key, span] : ix.keys) {
        const auto [b, e] = span;
        const std::int64_t m = e - b;
        RangeIndex::Node* t = ix.tree.data() + 2 * b;
        for (std::int64_t i = 0; i < m; ++i)
            t[m + i] = leaves[static_cast<std::size_t>(b + i)];
        for (std::int64_t i = m - 1; i > 0; --i)
            t[i] = combine(ix, read, t[2 * i], t[2 * i + 1]);
    }
}

RangeAnswer range_answer(const TLookup& l, const LookupTable& table,
                         std::string_view key,
                         const std::optional<Ordered>& low,
                         const std::optional<Ordered>& high) {
    RangeAnswer out;
    const RangeRead read = l.range;
    if (read == RangeRead::COUNT || read == RangeRead::COUNT_VALUES ||
        read == RangeRead::COUNT_IF)
        out.number = Number{std::int64_t{0}};
    if ((l.low && !low) || (l.high && !high)) return out;
    if (low && high && kind_of(*low) != kind_of(*high)) return out;
    const RangeIndex& ix = *table.range;
    const auto it = ix.keys.find(key);
    if (it == ix.keys.end()) return out;
    const auto [first, last] = it->second;
    auto [lo, hi] = kind_range(ix, first, last, low ? *low : *high);
    if (low) lo = position(ix, lo, hi, *low, *l.low == TermOp::GT);
    if (high) hi = position(ix, lo, hi, *high, *l.high == TermOp::LE);
    if (lo >= hi) return out;
    out.rows = hi - lo;
    if (read == RangeRead::ROWS) {
        if (l.kind == LookupKind::SCALAR && out.rows > 1)
            range_fail(
                "the sub-query '" + table.name + "' gives " +
                std::to_string(out.rows) + " rows in the range of " +
                (key.empty() ? std::string("a row") : "key " + key_text(key)) +
                "; a sub-query in an expression gives one row");
        out.row = ix.rows[static_cast<std::size_t>(lo)];
        return out;
    }
    if (read == RangeRead::COUNT) {
        out.number = Number{out.rows};
        return out;
    }
    const std::int64_t m = last - first;
    const RangeIndex::Node* t = ix.tree.data() + 2 * first;
    RangeIndex::Node acc;
    for (std::int64_t a = lo - first + m, b = hi - first + m; a < b;
         a >>= 1, b >>= 1) {
        if (a & 1) acc = combine(ix, read, acc, t[a++]);
        if (b & 1) acc = combine(ix, read, acc, t[--b]);
    }
    switch (read) {
        case RangeRead::COUNT_VALUES:
        case RangeRead::COUNT_IF:
            out.number = Number{acc.count};
            break;
        case RangeRead::SUM:
            if (acc.count == 0) break;
            if (!ix.integral) {
                out.number = Number{acc.fsum};
            } else if (acc.isum < std::numeric_limits<std::int64_t>::min() ||
                       acc.isum > std::numeric_limits<std::int64_t>::max()) {
                range_fail("'sum' of the sub-query '" + table.name +
                           "' overflows int64");
            } else {
                out.number = Number{static_cast<std::int64_t>(acc.isum)};
            }
            break;
        case RangeRead::MEAN:
            if (acc.count == 0) break;
            out.number = Number{
                (ix.integral ? static_cast<double>(acc.isum) : acc.fsum) /
                static_cast<double>(acc.count)};
            break;
        case RangeRead::MIN:
        case RangeRead::MAX:
            if (acc.best >= 0)
                out.row = ix.rows[static_cast<std::size_t>(acc.best)];
            break;
        case RangeRead::NONE:
        case RangeRead::ROWS:
        case RangeRead::COUNT:
            break;
    }
    return out;
}

}  // namespace dftracer::utils::duql
