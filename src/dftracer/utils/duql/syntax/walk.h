#ifndef DFTRACER_UTILS_DUQL_SYNTAX_WALK_H
#define DFTRACER_UTILS_DUQL_SYNTAX_WALK_H

#include <dftracer/utils/duql/syntax/tree.h>

#include <type_traits>
#include <variant>

namespace dftracer::utils::duql::syntax {

// Calls `fn` on every expression slot of the stages of `p`, unions included.
template <class F>
inline void each_slot(Pipeline& p, F&& fn) {
    auto items = [&](std::vector<Item>& v) {
        for (auto& it : v) fn(it.value);
    };
    auto assigns = [&](std::vector<Assign>& v) {
        for (auto& a : v) fn(a.value);
    };
    auto sort_keys = [&](std::vector<SortKey>& v) {
        for (auto& k : v) fn(k.value);
    };
    auto exprs = [&](std::vector<ExprPtr>& v) {
        for (auto& x : v) fn(x);
    };
    for (auto& s : p.stages)
        std::visit(
            [&](auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, Where>) {
                    fn(n.condition);
                } else if constexpr (std::is_same_v<T, Derive>) {
                    assigns(n.fields);
                } else if constexpr (std::is_same_v<T, Select>) {
                    items(n.items);
                } else if constexpr (std::is_same_v<T, Distinct>) {
                    exprs(n.keys);
                } else if constexpr (std::is_same_v<T, Group>) {
                    items(n.keys);
                    assigns(n.aggregates);
                } else if constexpr (std::is_same_v<T, Agg>) {
                    assigns(n.aggregates);
                } else if constexpr (std::is_same_v<T, Window>) {
                    items(n.partition);
                    sort_keys(n.order);
                    assigns(n.fields);
                } else if constexpr (std::is_same_v<T, Pivot>) {
                    fn(n.key);
                    exprs(n.values);
                    assigns(n.aggregates);
                } else if constexpr (std::is_same_v<T, Sort>) {
                    sort_keys(n.keys);
                } else if constexpr (std::is_same_v<T, Take>) {
                    exprs(n.by);
                    sort_keys(n.order);
                } else if constexpr (std::is_same_v<T, Lookup>) {
                    for (auto& [k, c] : n.keys) {
                        fn(k);
                        fn(c);
                    }
                } else if constexpr (std::is_same_v<T, OverlapLookup>) {
                    for (auto& [k, c] : n.keys) {
                        fn(k);
                        fn(c);
                    }
                } else if constexpr (std::is_same_v<T, AsofLookup>) {
                    for (auto& [k, c] : n.keys) {
                        fn(k);
                        fn(c);
                    }
                    fn(n.time.first);
                    fn(n.time.second);
                } else if constexpr (std::is_same_v<T, Union>) {
                    each_slot(*n.other, fn);
                } else if constexpr (std::is_same_v<T, CallStage>) {
                    for (auto& a : n.call.args) fn(a.value);
                } else if constexpr (std::is_same_v<T, TimeRange>) {
                    fn(n.low);
                    fn(n.high);
                } else if constexpr (std::is_same_v<T, Bucket>) {
                    fn(n.width);
                }
            },
            s.node);
}

// Calls `fn` on every expression slot directly under `e`; a sub-query
// gives the slots of its stages.
template <class F>
inline void children(Expr& e, F&& fn) {
    std::visit(
        [&](auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Unary>) {
                fn(n.operand);
            } else if constexpr (std::is_same_v<T, Binary>) {
                fn(n.left);
                fn(n.right);
            } else if constexpr (std::is_same_v<T, In>) {
                fn(n.subject);
                for (auto& x : n.list) fn(x);
                if (n.subquery) each_slot(*n.subquery, fn);
            } else if constexpr (std::is_same_v<T, Between>) {
                fn(n.subject);
                fn(n.low);
                fn(n.high);
            } else if constexpr (std::is_same_v<T, Like> ||
                                 std::is_same_v<T, Is>) {
                fn(n.subject);
            } else if constexpr (std::is_same_v<T, Arrow>) {
                fn(n.key);
            } else if constexpr (std::is_same_v<T, Call>) {
                for (auto& a : n.args) fn(a.value);
            } else if constexpr (std::is_same_v<T, List> ||
                                 std::is_same_v<T, Tuple>) {
                for (auto& x : n.items) fn(x);
            } else if constexpr (std::is_same_v<T, Subquery>) {
                each_slot(*n.pipeline, fn);
            }
        },
        e.node);
}

}  // namespace dftracer::utils::duql::syntax

#endif  // DFTRACER_UTILS_DUQL_SYNTAX_WALK_H
