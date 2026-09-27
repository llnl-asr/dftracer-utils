#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/duql/ast.h>
#include <dftracer/utils/duql/pattern_engine.h>
#include <dftracer/utils/index/plan/condition.h>
#include <dftracer/utils/index/plan/prefilter.h>

#include <charconv>
#include <cmath>
#include <cstring>
#include <optional>
#include <type_traits>
#include <variant>

namespace dftracer::utils::index::plan {

namespace {

using Clauses = std::vector<std::vector<std::string>>;

// Past this many clauses an `or` distributes into, it adds none.
constexpr std::size_t MAX_CLAUSES = 16;
constexpr std::size_t MIN_NEEDLE = 3;
// Letters from rarest to most common in English text, which key names follow;
// the rarest letter of a key is the one memchr stops on least.
constexpr std::string_view LETTERS_BY_RARITY = "zqxjkvbpygfwmucldrhsnioate";

std::size_t letter_rank(char c) {
    const char lower =
        (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    const auto i = LETTERS_BY_RARITY.find(lower);
    return i == std::string_view::npos ? 0 : i;
}

bool safe(std::string_view v) {
    for (unsigned char c : v)
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\' || c == '/')
            return false;
    return true;
}

std::optional<std::string> needle(const duql::LiteralNode& lit) {
    std::string out;
    if (const auto* s = std::get_if<std::string>(&lit.value)) {
        if (!safe(*s)) return std::nullopt;
        out = "\"" + *s + "\"";
    } else if (const auto* i = std::get_if<std::int64_t>(&lit.value)) {
        out = std::to_string(*i);
    } else if (const auto* u = std::get_if<std::uint64_t>(&lit.value)) {
        out = std::to_string(*u);
    } else {
        return std::nullopt;
    }
    if (out.size() < MIN_NEEDLE) return std::nullopt;
    return out;
}

// The clauses a line matching `node` must satisfy; empty when nothing can be
// required of its bytes.
Clauses clauses_of(const duql::QueryNode& node) {
    return std::visit(
        [](auto&& n) -> Clauses {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::CompareNode>) {
                if (n.op != duql::CompareOp::EQ) return {};
                auto s = needle(n.value);
                if (!s) return {};
                return {{std::move(*s)}};
            } else if constexpr (std::is_same_v<T, duql::InNode>) {
                if (n.values.elements.size() > SEMI_JOIN_CAP) return {};
                std::vector<std::string> any;
                for (const auto& e : n.values.elements) {
                    auto s = needle(e);
                    if (!s) return {};
                    any.push_back(std::move(*s));
                }
                if (any.empty()) return {};
                return {std::move(any)};
            } else if constexpr (std::is_same_v<T, duql::MatchNode>) {
                // A case-sensitive pattern's required literals sit in the
                // value's bytes unless JSON escaping hides them.
                if (n.negated || !n.compiled ||
                    (n.op != duql::MatchOp::LIKE &&
                     n.op != duql::MatchOp::REGEX))
                    return {};
                Clauses out;
                for (const auto& lit : duql::required_literals(*n.compiled))
                    if (lit.size() >= MIN_NEEDLE && safe(lit))
                        out.push_back({lit});
                return out;
            } else if constexpr (std::is_same_v<T, duql::AndNode>) {
                Clauses out = clauses_of(*n.left);
                Clauses right = clauses_of(*n.right);
                out.insert(out.end(), std::make_move_iterator(right.begin()),
                           std::make_move_iterator(right.end()));
                return out;
            } else if constexpr (std::is_same_v<T, duql::OrNode>) {
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
bool plain_key(std::string_view k) {
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
void ranges_of(const duql::QueryNode& node,
               std::vector<Prefilter::Range>& out) {
    if (const auto* a = std::get_if<duql::AndNode>(&node.data)) {
        ranges_of(*a->left, out);
        ranges_of(*a->right, out);
        return;
    }
    const auto* n = std::get_if<duql::CompareNode>(&node.data);
    // An any() key is followed by an array, not the number a range reads.
    if (!n || n->field.any || n->op == duql::CompareOp::NE) return;
    const std::string& path = n->field.path;
    const std::string_view key =
        std::string_view(path).substr(path.rfind('.') + 1);
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
    std::vector<Prefilter::Bound> bounds{bound};
    if (n->op == duql::CompareOp::EQ) {
        bounds[0].op = duql::CompareOp::GE;
        bounds.push_back(bounds[0]);
        bounds[1].op = duql::CompareOp::LE;
    }
    for (auto& r : out)
        if (r.path == path) {
            r.bounds.insert(r.bounds.end(), bounds.begin(), bounds.end());
            return;
        }
    std::size_t pivot = 0;
    for (std::size_t i = 1; i < key.size(); ++i)
        if (letter_rank(key[i]) < letter_rank(key[pivot])) pivot = i;
    out.push_back({path, std::string(key) + "\"", std::move(bounds), pivot});
}

bool holds(duql::CompareOp op, int cmp) {
    switch (op) {
        case duql::CompareOp::GT:
            return cmp > 0;
        case duql::CompareOp::GE:
            return cmp >= 0;
        case duql::CompareOp::LT:
            return cmp < 0;
        case duql::CompareOp::LE:
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
        auto [q, ec] = std::from_chars(tok, p, iv);
        if (ec != std::errc() || q != p) return true;
    } else {
        auto [q, ec] = from_chars_double(tok, p, dv);
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
bool Prefilter::range_may_hold(std::string_view line, const Range& r) {
    const std::string& seg = r.segment;
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

std::size_t pivot_of(const std::string& n) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < n.size(); ++i)
        if (rarity(static_cast<unsigned char>(n[i])) >
            rarity(static_cast<unsigned char>(n[best])))
            best = i;
    return best;
}

// memchr on the pivot byte, then compare: several times faster than memmem
// or string_view::find, which cost more per line than parsing it.
bool contains(std::string_view line, const std::string& n, std::size_t p) {
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

Prefilter::Prefilter(const duql::Query& q) : clauses_(clauses_of(q.root())) {
    ranges_of(q.root(), ranges_);
    pivots_.reserve(clauses_.size());
    for (const auto& clause : clauses_) {
        auto& p = pivots_.emplace_back();
        for (const auto& n : clause) p.push_back(pivot_of(n));
    }
}

bool Prefilter::may_match(std::string_view line) const {
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

bool Prefilter::Gate::may_match(std::string_view line) {
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
