// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/duql/subsumption.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace dftracer::utils::duql {

namespace {

dstd::Optional<double> as_number(const LiteralValue& v) {
    if (auto p = std::get_if<std::int64_t>(&v)) return static_cast<double>(*p);
    if (auto p = std::get_if<std::uint64_t>(&v)) return static_cast<double>(*p);
    if (auto p = std::get_if<double>(&v)) return *p;
    return dstd::nullopt;
}

// Canonical token for one literal, used as a set-membership key.
dstd::String literal_key(const LiteralValue& v) {
    if (auto p = std::get_if<dstd::String>(&v)) return "s:" + *p;
    if (auto p = std::get_if<bool>(&v)) return *p ? "b:1" : "b:0";
    if (auto p = std::get_if<std::int64_t>(&v))
        return "n:" + dstd::fixed_text(static_cast<double>(*p));
    if (auto p = std::get_if<std::uint64_t>(&v))
        return "n:" + dstd::fixed_text(static_cast<double>(*p));
    if (auto p = std::get_if<double>(&v)) return "n:" + dstd::fixed_text(*p);
    return "?";
}

// One side of a numeric range. `strict` distinguishes > / < from >= / <=.
struct Bound {
    double val;
    bool strict;
};

struct Interval {
    dstd::Optional<Bound> lo;  // lower bound (> or >=), empty = -inf
    dstd::Optional<Bound> hi;  // upper bound (< or <=), empty = +inf
};

// Intersect a conjunction's per-field constraints: keep the tighter bound.
void tighten_lo(dstd::Optional<Bound>& cur, const Bound& cand) {
    if (!cur || cand.val > cur->val ||
        (cand.val == cur->val && cand.strict && !cur->strict))
        cur = cand;
}
void tighten_hi(dstd::Optional<Bound>& cur, const Bound& cand) {
    if (!cur || cand.val < cur->val ||
        (cand.val == cur->val && cand.strict && !cur->strict))
        cur = cand;
}

// True if query's bound is at least as restrictive as mv's (so query's
// admitted values are a subset of mv's on that side).
bool lo_within(const dstd::Optional<Bound>& q,
               const dstd::Optional<Bound>& mv) {
    if (!mv) return true;
    if (!q) return false;
    if (q->val != mv->val) return q->val > mv->val;
    return (q->strict ? 1 : 0) >= (mv->strict ? 1 : 0);
}
bool hi_within(const dstd::Optional<Bound>& q,
               const dstd::Optional<Bound>& mv) {
    if (!mv) return true;
    if (!q) return false;
    if (q->val != mv->val) return q->val < mv->val;
    return (mv->strict ? 0 : 1) >= (q->strict ? 0 : 1);
}

bool point_in(double v, const Interval& iv) {
    if (iv.lo && (v < iv.lo->val || (v == iv.lo->val && iv.lo->strict)))
        return false;
    if (iv.hi && (v > iv.hi->val || (v == iv.hi->val && iv.hi->strict)))
        return false;
    return true;
}
dstd::Optional<double> as_point(const Interval& iv) {
    if (iv.lo && iv.hi && !iv.lo->strict && !iv.hi->strict &&
        iv.lo->val == iv.hi->val)
        return iv.lo->val;
    return dstd::nullopt;
}

// A conjunction split into numeric intervals, value sets, and opaque
// residuals (or/not/like/!=/not in) keyed by canonical source.
struct Buckets {
    dstd::Map<dstd::String, Interval> intervals;
    dstd::Map<dstd::String, dstd::Map<dstd::String, dstd::Optional<double>>>
        sets;
    dstd::Set<dstd::String> residual;
};

void add_set_value(Buckets& b, const dstd::String& field,
                   const LiteralValue& v) {
    b.sets[field].emplace(literal_key(v), as_number(v));
}

// An `or` made only of equality/IN tests on ONE field is a value set
// (`name == a or name == b` is `name in [a,b]`). Collects the field + values,
// or returns false if the disjunction mixes fields or carries any other op.
bool or_to_set(const detail::QueryNode& node, dstd::String& field,
               dstd::Map<dstd::String, dstd::Optional<double>>& vals) {
    return std::visit(
        [&](auto&& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, detail::OrNode>) {
                return or_to_set(*n.left, field, vals) &&
                       or_to_set(*n.right, field, vals);
            } else if constexpr (std::is_same_v<T, detail::CompareNode>) {
                if (n.op != CompareOp::Eq || n.field.any) return false;
                if (field.empty())
                    field = n.field.path;
                else if (field != n.field.path)
                    return false;
                vals.emplace(literal_key(n.value.value),
                             as_number(n.value.value));
                return true;
            } else if constexpr (std::is_same_v<T, detail::InNode>) {
                if (n.field.any) return false;
                if (field.empty())
                    field = n.field.path;
                else if (field != n.field.path)
                    return false;
                for (const auto& e : n.values.elements)
                    vals.emplace(literal_key(e.value), as_number(e.value));
                return true;
            } else {
                return false;
            }
        },
        node.data);
}

void flatten(const detail::QueryNode& node, Buckets& b) {
    std::visit(
        [&](auto&& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, detail::AndNode>) {
                flatten(*n.left, b);
                flatten(*n.right, b);
            } else if constexpr (std::is_same_v<T, detail::CompareNode>) {
                // Two any() bounds may hold on different elements, so they
                // do not narrow one interval.
                if (n.field.any) {
                    b.residual.insert(to_string(node));
                    return;
                }
                const auto num = as_number(n.value.value);
                if (n.op == CompareOp::Eq) {
                    if (num) {
                        auto& iv = b.intervals[n.field.path];
                        tighten_lo(iv.lo, Bound{*num, false});
                        tighten_hi(iv.hi, Bound{*num, false});
                    } else {
                        add_set_value(b, n.field.path, n.value.value);
                    }
                } else if (num &&
                           (n.op == CompareOp::Gt || n.op == CompareOp::Ge)) {
                    tighten_lo(b.intervals[n.field.path].lo,
                               Bound{*num, n.op == CompareOp::Gt});
                } else if (num &&
                           (n.op == CompareOp::Lt || n.op == CompareOp::Le)) {
                    tighten_hi(b.intervals[n.field.path].hi,
                               Bound{*num, n.op == CompareOp::Lt});
                } else {
                    b.residual.insert(to_string(node));  // NE, or non-numeric
                }
            } else if constexpr (std::is_same_v<T, detail::InNode>) {
                if (n.field.any) {
                    b.residual.insert(to_string(node));
                    return;
                }
                for (const auto& e : n.values.elements)
                    add_set_value(b, n.field.path, e.value);
            } else if constexpr (std::is_same_v<T, detail::OrNode>) {
                // An all-equality/IN-on-one-field `or` is a value set; any
                // other disjunction stays an opaque residual.
                dstd::String field;
                dstd::Map<dstd::String, dstd::Optional<double>> vals;
                if (or_to_set(node, field, vals))
                    for (const auto& [k, v] : vals) b.sets[field].emplace(k, v);
                else
                    b.residual.insert(to_string(node));
            } else {
                // NotInNode, MatchNode, NotNode, ExprLeaf: opaque residual.
                b.residual.insert(to_string(node));
            }
        },
        node.data);
}

// Every value listed for `field` in `q` sets is numeric and inside `iv`.
bool set_within_interval(
    const dstd::Map<dstd::String, dstd::Optional<double>>& vals,
    const Interval& iv) {
    for (const auto& entry : vals) {
        const auto& num = entry.second;
        if (!num || !point_in(*num, iv)) return false;
    }
    return true;
}

}  // namespace

// Conjunctive containment: both predicates flattened to buckets, no top-level
// disjunction (or is one opaque residual atom here).
static bool conj_subsumes(const detail::QueryNode& mv,
                          const detail::QueryNode& query) {
    Buckets M, Q;
    flatten(mv, M);
    flatten(query, Q);

    // Every interval mv imposes, the query must impose at least as tightly
    // (directly, or via an all-numeric value set contained in the interval).
    for (const auto& [field, m_iv] : M.intervals) {
        auto qi = Q.intervals.find(field);
        if (qi != Q.intervals.end() && lo_within(qi->second.lo, m_iv.lo) &&
            hi_within(qi->second.hi, m_iv.hi))
            continue;
        auto qs = Q.sets.find(field);
        if (qs != Q.sets.end() && set_within_interval(qs->second, m_iv))
            continue;
        return false;
    }

    // Every value set mv imposes, the query must land within (subset of the
    // set, or a single numeric point equal to a member).
    for (const auto& [field, m_set] : M.sets) {
        auto qs = Q.sets.find(field);
        if (qs != Q.sets.end()) {
            bool subset = true;
            for (const auto& entry : qs->second) {
                const auto& key = entry.first;
                if (!m_set.count(key)) {
                    subset = false;
                    break;
                }
            }
            if (subset) continue;
        }
        auto qi = Q.intervals.find(field);
        if (qi != Q.intervals.end()) {
            if (auto p = as_point(qi->second)) {
                bool member = false;
                for (const auto& entry : m_set) {
                    const auto& num = entry.second;
                    if (num && *num == *p) {
                        member = true;
                        break;
                    }
                }
                if (member) continue;
            }
        }
        return false;
    }

    // Every opaque residual mv imposes must appear verbatim in the query.
    for (const auto& r : M.residual)
        if (!Q.residual.count(r)) return false;

    return true;
}

bool query_subsumes(const detail::QueryNode& mv,
                    const detail::QueryNode& query) {
    // A top-level `or` on the query is a union: mv must subsume every branch.
    // Exact (rows(A or B) subset X iff rows(A) subset X and rows(B) subset X).
    if (auto* o = std::get_if<detail::OrNode>(&query.data))
        return query_subsumes(mv, *o->left) && query_subsumes(mv, *o->right);
    // Conjunctive match first - it also matches a whole `or` as an identical
    // residual (e.g. the query re-uses the mv's disjunction verbatim).
    if (conj_subsumes(mv, query)) return true;
    // Otherwise a top-level `or` on the mv widens it: the query fits if it fits
    // some branch. Sound but incomplete - a query spanning branches is
    // declined.
    if (auto* o = std::get_if<detail::OrNode>(&mv.data))
        return query_subsumes(*o->left, query) ||
               query_subsumes(*o->right, query);
    return false;
}

}  // namespace dftracer::utils::duql
