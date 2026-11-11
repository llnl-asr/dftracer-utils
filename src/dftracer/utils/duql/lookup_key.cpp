// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/lookup.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace dftracer::utils::duql {

namespace {

constexpr char INT_TAG = 'i';
constexpr char DOUBLE_TAG = 'd';
constexpr char STRING_TAG = 's';
constexpr char BOOL_TAG = 'b';

std::uint64_t get_u64(dstd::StringView s) {
    return dstd::read_u64_be(reinterpret_cast<const std::uint8_t*>(s.data()));
}

}  // namespace

void append_int_key(dstd::String& out, bool negative, std::uint64_t magnitude) {
    out += INT_TAG;
    out += negative && magnitude != 0 ? '-' : '+';
    dstd::append_u64_be(out, magnitude);
}

bool append_double_key(dstd::String& out, double v) {
    if (std::isnan(v)) return false;
    if (std::trunc(v) == v && std::abs(v) < 18446744073709551616.0) {
        const bool neg = v < 0;
        append_int_key(out, neg, static_cast<std::uint64_t>(neg ? -v : v));
        return true;
    }
    out += DOUBLE_TAG;
    dstd::append_f64_be(out, v);
    return true;
}

void append_string_key(dstd::String& out, dstd::StringView v) {
    out += STRING_TAG;
    dstd::append_u64_be(out, v.size());
    out += v;
}

dstd::Optional<dstd::StringView> string_key_text(dstd::StringView key) {
    if (key.size() < 9 || key[0] != STRING_TAG) return dstd::nullopt;
    return key.substr(9);
}

void append_bool_key(dstd::String& out, bool v) {
    out += BOOL_TAG;
    out += v ? '1' : '0';
}

dstd::String key_text(dstd::StringView key) {
    dstd::String out;
    std::size_t parts = 0;
    while (!key.empty()) {
        if (parts++ > 0) out += ", ";
        const char tag = key[0];
        key.remove_prefix(1);
        if (tag == INT_TAG) {
            if (key[0] == '-') out += '-';
            out += dstd::int_text(get_u64(key.substr(1)));
            key.remove_prefix(9);
        } else if (tag == DOUBLE_TAG) {
            const double d = dstd::bits_double(get_u64(key));
            char buf[32];
            out.append(buf,
                       dstd::to_chars_general(buf, buf + sizeof(buf), d, 17));
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
    return parts > 1 ? dstd::fmt<"({})">(out) : out;
}

const dstd::Vector<std::int64_t>* LookupTable::find(
    dstd::StringView key) const {
    const auto it = rows.find(key);
    return it == rows.end() ? nullptr : &it->second;
}

void LookupFailure::set(DuqlError error) {
    if (failed_.load(std::memory_order_acquire)) return;
    const std::lock_guard<std::mutex> lock(mutex_);
    if (error_) return;
    error_ = std::move(error);
    failed_.store(true, std::memory_order_release);
}

dstd::Optional<DuqlError> LookupFailure::get() const {
    if (!failed_.load(std::memory_order_acquire)) return dstd::nullopt;
    const std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

DuqlError lookup_error(dstd::String message) {
    DuqlError out;
    out.message = std::move(message);
    return out;
}

DuqlError conflict_error(const LookupTable& table, dstd::StringView column,
                         dstd::StringView key) {
    return lookup_error(
        dstd::fmt<"View::duql '{}' has rows with different values of "
                  "'{}' for key {}; pick one with a narrower let, or "
                  "read every row with 'lookup ... into'">(table.name, column,
                                                           key_text(key)));
}

DuqlError scalar_conflict_error(dstd::StringView name, dstd::StringView key,
                                std::size_t rows) {
    return lookup_error(
        dstd::fmt<"View::duql the sub-query '{}' gives {} rows for "
                  "key {}; a sub-query in an expression gives one row">(
            name, rows, key_text(key)));
}

dstd::Optional<DuqlError> lookup_failure(const detail::QueryNode& node) {
    return std::visit(
        [](const auto& n) -> dstd::Optional<DuqlError> {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, detail::AndNode> ||
                          std::is_same_v<T, detail::OrNode>) {
                if (auto left = lookup_failure(*n.left)) return left;
                return lookup_failure(*n.right);
            } else if constexpr (std::is_same_v<T, detail::NotNode>) {
                return lookup_failure(*n.operand);
            } else if constexpr (std::is_same_v<T, detail::ExprLeaf>) {
                dstd::Optional<DuqlError> out;
                for_each_term(*n.term, [&out](const Term& t) {
                    if (out) return;
                    if (const auto* l = std::get_if<TLookup>(&t.node);
                        l && l->slot)
                        out = l->slot->failure.get();
                });
                return out;
            } else {
                return dstd::nullopt;
            }
        },
        node.data);
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
        out.best = (read == RangeRead::Min ? c <= 0 : c >= 0) ? a.best : b.best;
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
dstd::Pair<std::int64_t, std::int64_t> kind_range(const RangeIndex& ix,
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

void range_fail(const TLookup& l, const dstd::String& why) {
    l.slot->failure.set(lookup_error(dstd::fmt<"View::duql {}">(why)));
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
            if constexpr (std::is_same_v<A, dstd::StringView> ||
                          std::is_same_v<B, dstd::StringView> ||
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
                      const dstd::Vector<RangeIndex::Node>& leaves) {
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
                         dstd::StringView key,
                         const dstd::Optional<Ordered>& low,
                         const dstd::Optional<Ordered>& high) {
    RangeAnswer out;
    const RangeRead read = l.range;
    if (read == RangeRead::Count || read == RangeRead::CountValues ||
        read == RangeRead::CountIf)
        out.number = Number{std::int64_t{0}};
    if ((l.low && !low) || (l.high && !high)) return out;
    if (low && high && kind_of(*low) != kind_of(*high)) return out;
    const RangeIndex& ix = *table.range;
    const auto it = ix.keys.find(key);
    if (it == ix.keys.end()) return out;
    const auto [first, last] = it->second;
    auto [lo, hi] = kind_range(ix, first, last, low ? *low : *high);
    if (low) lo = position(ix, lo, hi, *low, *l.low == TermOp::Gt);
    if (high) hi = position(ix, lo, hi, *high, *l.high == TermOp::Le);
    if (lo >= hi) return out;
    out.rows = hi - lo;
    if (read == RangeRead::Rows) {
        if (l.kind == LookupKind::Scalar && out.rows > 1)
            range_fail(
                l, dstd::fmt<"the sub-query '{}' gives {} rows in the range of "
                             "{}; a sub-query in an expression gives one row">(
                       table.name, out.rows,
                       (key.empty() ? dstd::String("a row")
                                    : dstd::fmt<"key {}">(key_text(key)))));
        out.row = ix.rows[static_cast<std::size_t>(lo)];
        return out;
    }
    if (read == RangeRead::Count) {
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
        case RangeRead::CountValues:
        case RangeRead::CountIf:
            out.number = Number{acc.count};
            break;
        case RangeRead::Sum:
            if (acc.count == 0) break;
            if (!ix.integral) {
                out.number = Number{acc.fsum};
            } else if (acc.isum < std::numeric_limits<std::int64_t>::min() ||
                       acc.isum > std::numeric_limits<std::int64_t>::max()) {
                range_fail(
                    l, dstd::fmt<"'sum' of the sub-query '{}' overflows int64">(
                           table.name));
            } else {
                out.number = Number{static_cast<std::int64_t>(acc.isum)};
            }
            break;
        case RangeRead::Mean:
            if (acc.count == 0) break;
            out.number = Number{
                (ix.integral ? static_cast<double>(acc.isum) : acc.fsum) /
                static_cast<double>(acc.count)};
            break;
        case RangeRead::Min:
        case RangeRead::Max:
            if (acc.best >= 0)
                out.row = ix.rows[static_cast<std::size_t>(acc.best)];
            break;
        case RangeRead::None:
        case RangeRead::Rows:
        case RangeRead::Count:
            break;
    }
    return out;
}

}  // namespace dftracer::utils::duql
