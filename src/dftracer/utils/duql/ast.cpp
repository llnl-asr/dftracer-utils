#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/pattern.h>
#include <dftracer/utils/duql/term.h>

#include <algorithm>
#include <string_view>
#include <type_traits>
#include <variant>

namespace dftracer::utils::duql {

namespace {

// Calls `fn(path, any)` for the field of every leaf under `node`, and for
// every field an expression leaf reads.
template <class Fn>
void for_each_field(const QueryNode& node, Fn&& fn) {
    std::visit(
        [&fn](auto&& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, AndNode> ||
                          std::is_same_v<T, OrNode>) {
                for_each_field(*n.left, fn);
                for_each_field(*n.right, fn);
            } else if constexpr (std::is_same_v<T, NotNode>) {
                for_each_field(*n.operand, fn);
            } else if constexpr (std::is_same_v<T, ExprLeaf>) {
                for_each_term_field(*n.term, [&fn](const TField& f) {
                    fn(std::string_view(f.base), false);
                });
            } else {
                fn(std::string_view(n.field.path), n.field.any);
            }
        },
        node.data);
}

}  // namespace

dftracer::utils::StringViewSet collect_fields(const QueryNode& node) {
    dftracer::utils::StringViewSet fields;
    for_each_field(node,
                   [&](std::string_view path, bool) { fields.insert(path); });
    return fields;
}

std::vector<std::string> collect_any_paths(const QueryNode& node) {
    std::vector<std::string> paths;
    for_each_field(node, [&](std::string_view path, bool any) {
        if (any) paths.emplace_back(path);
    });
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

bool has_expression_leaf(const QueryNode& node) {
    return std::visit(
        [](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, AndNode> ||
                          std::is_same_v<T, OrNode>)
                return has_expression_leaf(*n.left) ||
                       has_expression_leaf(*n.right);
            else if constexpr (std::is_same_v<T, NotNode>)
                return has_expression_leaf(*n.operand);
            else
                return std::is_same_v<T, ExprLeaf>;
        },
        node.data);
}

QueryNodePtr clone(const QueryNode& node) {
    return std::visit(
        [](const auto& n) -> QueryNodePtr {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, AndNode> ||
                          std::is_same_v<T, OrNode>)
                return make_node(T{clone(*n.left), clone(*n.right)});
            else if constexpr (std::is_same_v<T, NotNode>)
                return make_node(NotNode{clone(*n.operand)});
            else
                return make_node(T(n));
        },
        node.data);
}

std::shared_ptr<const InSet> make_in_set(const ArrayNode& values) {
    if (values.elements.size() < IN_SET_MIN) return nullptr;
    auto set = std::make_shared<InSet>();
    set->values.reserve(values.elements.size());
    for (const auto& e : values.elements) {
        const auto* s = std::get_if<std::string>(&e.value);
        if (!s) return nullptr;
        set->values.insert(*s);
    }
    return set;
}

}  // namespace dftracer::utils::duql
