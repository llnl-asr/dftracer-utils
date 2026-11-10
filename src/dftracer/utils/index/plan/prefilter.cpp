// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/detail/ast.h>
#include <dftracer/utils/duql/literal_set.h>
#include <dftracer/utils/duql/pattern_engine.h>
#include <dftracer/utils/index/plan/condition.h>
#include <dftracer/utils/index/plan/prefilter.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <type_traits>
#include <variant>

namespace dftracer::utils::index::plan {

namespace {

using Clauses = dstd::Vector<dstd::Vector<dstd::String>>;

// Past this many clauses an `or` distributes into, it adds none.
constexpr std::size_t MAX_CLAUSES = 16;
constexpr std::size_t MIN_NEEDLE = 3;
// Letters from rarest to most common in English text, which key names follow;
// the rarest letter of a key is the one memchr stops on least.
constexpr dstd::StringView LETTERS_BY_RARITY = "zqxjkvbpygfwmucldrhsnioate";

std::size_t letter_rank(char c) {
    const char lower =
        (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    const auto i = LETTERS_BY_RARITY.find(lower);
    return i == dstd::StringView::npos ? 0 : i;
}

// A reader repairs a bad byte in a string to '?', so a '?' of the value need
// not be in the text.
bool safe(dstd::StringView v) {
    for (unsigned char c : v)
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\' || c == '/' ||
            c == '?')
            return false;
    return true;
}

dstd::Optional<dstd::String> needle(const duql::LiteralNode& lit) {
    dstd::String out;
    if (const auto* s = std::get_if<dstd::String>(&lit.value)) {
        if (!safe(*s)) return dstd::nullopt;
        out = "\"" + *s + "\"";
    } else if (const auto* i = std::get_if<std::int64_t>(&lit.value)) {
        out = dstd::to_string(*i);
    } else if (const auto* u = std::get_if<std::uint64_t>(&lit.value)) {
        out = dstd::to_string(*u);
    } else {
        return dstd::nullopt;
    }
    if (out.size() < MIN_NEEDLE) return dstd::nullopt;
    return out;
}

// The clauses a line matching `node` must satisfy; empty when nothing can be
// required of its bytes.
Clauses clauses_of(const duql::detail::QueryNode& node) {
    return std::visit(
        [](auto&& n) -> Clauses {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::detail::CompareNode>) {
                if (n.op != duql::CompareOp::Eq) return {};
                auto s = needle(n.value);
                if (!s) return {};
                return {{std::move(*s)}};
            } else if constexpr (std::is_same_v<T, duql::detail::InNode>) {
                if (n.values.elements.size() > SEMI_JOIN_CAP) return {};
                dstd::Vector<dstd::String> any;
                for (const auto& e : n.values.elements) {
                    auto s = needle(e);
                    if (!s) return {};
                    any.push_back(std::move(*s));
                }
                if (any.empty()) return {};
                return {std::move(any)};
            } else if constexpr (std::is_same_v<T, duql::detail::MatchNode>) {
                // A case-sensitive pattern's required literals sit in the
                // value's bytes unless JSON escaping hides them.
                if (n.negated || !n.compiled ||
                    (n.op != duql::MatchOp::Like &&
                     n.op != duql::MatchOp::Regex))
                    return {};
                Clauses out;
                for (const auto& lit : duql::required_literals(*n.compiled))
                    if (lit.size() >= MIN_NEEDLE && safe(lit))
                        out.push_back({dstd::String(lit)});
                return out;
            } else if constexpr (std::is_same_v<T, duql::detail::AndNode>) {
                Clauses out = clauses_of(*n.left);
                Clauses right = clauses_of(*n.right);
                out.insert(out.end(), std::make_move_iterator(right.begin()),
                           std::make_move_iterator(right.end()));
                return out;
            } else if constexpr (std::is_same_v<T, duql::detail::OrNode>) {
                // (a1 and a2) or (b1 and b2) holds each ai-or-bj clause.
                const Clauses l = clauses_of(*n.left);
                const Clauses r = clauses_of(*n.right);
                if (l.empty() || r.empty() || l.size() * r.size() > MAX_CLAUSES)
                    return {};
                Clauses out;
                for (const auto& a : l)
                    for (const auto& b : r) {
                        auto& c = out.emplace_back(a);
                        c.insert(c.end(), b.begin(), b.end());
                    }
                return out;
            } else {
                return {};
            }
        },
        node.data);
}

// A segment a key can end in: not an array index, no byte a JSON key would
// escape.
bool plain_key(dstd::StringView k) {
    bool digits_only = true;
    for (unsigned char c : k) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return false;
        digits_only = digits_only && c >= '0' && c <= '9';
    }
    return !k.empty() && !digits_only;
}

// The ranges a line matching `node` must satisfy: numeric order leaves
// joined by `and`.
void ranges_of(const duql::detail::QueryNode& node,
               dstd::Vector<Prefilter::Range>& out) {
    if (const auto* a = std::get_if<duql::detail::AndNode>(&node.data)) {
        ranges_of(*a->left, out);
        ranges_of(*a->right, out);
        return;
    }
    const auto* n = std::get_if<duql::detail::CompareNode>(&node.data);
    // An any() key is followed by an array, not the number a range reads.
    if (!n || n->field.any || n->op == duql::CompareOp::Ne) return;
    const dstd::String path(n->field.path);
    const dstd::StringView key =
        dstd::StringView(path).substr(path.rfind('.') + 1);
    if (!plain_key(key)) return;
    Prefilter::Bound bound{n->op};
    if (const auto* i = std::get_if<std::int64_t>(&n->value.value)) {
        bound.ibound = *i;
    } else if (const auto* u = std::get_if<std::uint64_t>(&n->value.value)) {
        if (*u > static_cast<std::uint64_t>(INT64_MAX)) return;
        bound.ibound = static_cast<std::int64_t>(*u);
    } else if (const auto* d = std::get_if<double>(&n->value.value)) {
        bound.integral = false;
        bound.dbound = *d;
    } else {
        return;
    }
    // A number equals the bound when it is at both ends of [bound, bound].
    dstd::Vector<Prefilter::Bound> bounds{bound};
    if (n->op == duql::CompareOp::Eq) {
        bounds[0].op = duql::CompareOp::Ge;
        bounds.push_back(bounds[0]);
        bounds[1].op = duql::CompareOp::Le;
    }
    for (auto& r : out)
        if (r.path == path) {
            r.bounds.insert(r.bounds.end(), bounds.begin(), bounds.end());
            return;
        }
    std::size_t pivot = 0;
    for (std::size_t i = 1; i < key.size(); ++i)
        if (letter_rank(key[i]) < letter_rank(key[pivot])) pivot = i;
    out.push_back({path, dstd::String(key) + "\"", std::move(bounds), pivot});
}

bool holds(duql::CompareOp op, int cmp) {
    switch (op) {
        case duql::CompareOp::Gt:
            return cmp > 0;
        case duql::CompareOp::Ge:
            return cmp >= 0;
        case duql::CompareOp::Lt:
            return cmp < 0;
        case duql::CompareOp::Le:
            return cmp <= 0;
        default:
            return true;
    }
}

// Doubles past this may round across the bound; such a value keeps the line.
constexpr double EXACT_DOUBLE = 9007199254740992.0;

// Whether the number from `p` (just after a key's colon) satisfies every bound
// of `r`; true when it cannot be read exactly, since only a certain failure
// may drop the line.
bool number_holds(const char* p, const char* end, const Prefilter::Range& r) {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    const char* tok = p;
    bool integer = true;
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' ||
                       *p == '.' || *p == 'e' || *p == 'E')) {
        if (*p == '.' || *p == 'e' || *p == 'E') integer = false;
        ++p;
    }
    if (p == tok) return false;
    std::int64_t iv = 0;
    double dv = 0;
    if (integer) {
        auto [q, ec] = dstd::from_chars(tok, p, iv);
        if (ec != std::errc() || q != p) return true;
    } else {
        auto [q, ec] = dstd::from_chars_double(tok, p, dv);
        if (ec != std::errc() || q != p || std::fabs(dv) >= EXACT_DOUBLE)
            return true;
    }
    for (const auto& b : r.bounds) {
        int cmp = 0;
        if (integer && b.integral) {
            cmp = iv < b.ibound ? -1 : (iv > b.ibound ? 1 : 0);
        } else {
            const double v = integer ? static_cast<double>(iv) : dv;
            const double x =
                b.integral ? static_cast<double>(b.ibound) : b.dbound;
            if (std::fabs(v) >= EXACT_DOUBLE || std::fabs(x) >= EXACT_DOUBLE)
                continue;
            cmp = v < x ? -1 : (v > x ? 1 : 0);
        }
        if (!holds(b.op, cmp)) return false;
    }
    return true;
}

}  // namespace

// A key ending in the segment of `r` sits at `start`, preceded by its opening
// quote or a dot, followed by the closing quote, spaces and a colon.
bool Prefilter::range_may_hold(dstd::StringView line, const Range& r) {
    const dstd::String& seg = r.segment;
    const std::size_t n = seg.size() - 1;
    const char* begin = line.data();
    const char* end = begin + line.size();
    const std::size_t p = r.pivot;
    for (const char* cur = begin + 1 + p; cur + (seg.size() - p) <= end;) {
        const char* hit = static_cast<const char*>(std::memchr(
            cur, seg[p], static_cast<std::size_t>(end - cur) - (n - p)));
        if (!hit) return false;
        const char* start = hit - p;
        cur = hit + 1;
        if ((start[-1] != '"' && start[-1] != '.') ||
            std::memcmp(start, seg.data(), seg.size()) != 0)
            continue;
        const char* q = start + seg.size();
        while (q < end && (*q == ' ' || *q == '\t')) ++q;
        if (q < end && *q == ':' && number_holds(q + 1, end, r)) return true;
    }
    return false;
}

namespace {

// JSON lines are mostly structure, digits and lowercase keys; a needle's
// byte outside those is rare, so memchr on it stops seldom.
int rarity(unsigned char c) {
    if (c == '"' || c == ':' || c == ',' || c == '{' || c == '}' || c == ' ')
        return 0;
    if (c >= '0' && c <= '9') return 1;
    if (c >= 'a' && c <= 'z') return 2;
    return 3;
}

std::size_t pivot_of(const dstd::String& n) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < n.size(); ++i)
        if (rarity(static_cast<unsigned char>(n[i])) >
            rarity(static_cast<unsigned char>(n[best])))
            best = i;
    return best;
}

// memchr on the pivot byte, then compare: several times faster than memmem
// or string_view::find, which cost more per line than parsing it.
bool contains(dstd::StringView line, const dstd::String& n, std::size_t p) {
    if (line.size() < n.size()) return false;
    const char* begin = line.data();
    const char* last = begin + (line.size() - n.size()) + p;
    for (const char* cur = begin + p; cur <= last;) {
        const char* hit = static_cast<const char*>(
            std::memchr(cur, n[p], static_cast<std::size_t>(last - cur) + 1));
        if (!hit) return false;
        if (std::memcmp(hit - p, n.data(), n.size()) == 0) return true;
        cur = hit + 1;
    }
    return false;
}

}  // namespace

Prefilter::Prefilter(const duql::Query& q)
    : clauses_(clauses_of(duql::detail::QueryAccess::root(q))) {
    ranges_of(duql::detail::QueryAccess::root(q), ranges_);
    pivots_.reserve(clauses_.size());
    for (const auto& clause : clauses_) {
        auto& p = pivots_.emplace_back();
        for (const auto& n : clause) p.push_back(pivot_of(n));
    }
    std::size_t widest = 0;
    for (const auto& clause : clauses_)
        widest = std::max(widest, clause.size());
    if (!duql::LiteralSet::AVAILABLE || widest < HS_MIN_ALTERNATIVES) return;
    dstd::HashMap<dstd::String, unsigned> seen;
    dstd::Vector<dstd::StringView> literals;
    ids_.reserve(clauses_.size());
    for (const auto& clause : clauses_) {
        auto& ids = ids_.emplace_back();
        for (const auto& n : clause) {
            auto [it, fresh] =
                seen.try_emplace(n, static_cast<unsigned>(literals.size()));
            if (fresh) literals.push_back(n);
            ids.push_back(it->second);
        }
    }
    literals_ = duql::LiteralSet::make(literals);
}

bool Prefilter::may_match_per_needle(dstd::StringView line) const {
    for (std::size_t c = 0; c < clauses_.size(); ++c) {
        bool any = false;
        for (std::size_t k = 0; k < clauses_[c].size(); ++k)
            if (contains(line, clauses_[c][k], pivots_[c][k])) {
                any = true;
                break;
            }
        if (!any) return false;
    }
    for (const auto& r : ranges_)
        if (!range_may_hold(line, r)) return false;
    return true;
}

bool Prefilter::may_match(dstd::StringView line) const {
    if (!literals_) return may_match_per_needle(line);
    const std::uint64_t* const bits = literals_.scan(line);
    if (!bits) return may_match_per_needle(line);
    for (const auto& ids : ids_) {
        bool any = false;
        for (const unsigned id : ids)
            if ((bits[id >> 6] >> (id & 63)) & 1) {
                any = true;
                break;
            }
        if (!any) return false;
    }
    for (const auto& r : ranges_)
        if (!range_may_hold(line, r)) return false;
    return true;
}

bool Prefilter::Gate::may_match(dstd::StringView line) {
    if (!on_) return true;
    const bool pass = p_->may_match(line);
    passed_ += pass ? 1 : 0;
    if (++checked_ == CHECK_WINDOW &&
        static_cast<double>(passed_) >
            MAX_PASS_RATE * static_cast<double>(checked_))
        on_ = false;
    return pass;
}

}  // namespace dftracer::utils::index::plan
