#include <dftracer/utils/query/fields.h>
#include <dftracer/utils/query/pattern.h>

#include <algorithm>
#include <type_traits>
#include <variant>

namespace dftracer::utils::query {

namespace {

// Calls `fn` with the field of every leaf under `node`.
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
            } else {
                fn(n.field);
            }
        },
        node.data);
}

}  // namespace

dftracer::utils::StringViewSet collect_fields(const QueryNode& node) {
    dftracer::utils::StringViewSet fields;
    for_each_field(node, [&](const FieldNode& f) { fields.insert(f.path); });
    return fields;
}

std::vector<std::string> collect_any_paths(const QueryNode& node) {
    std::vector<std::string> paths;
    for_each_field(node, [&](const FieldNode& f) {
        if (f.any) paths.push_back(f.path);
    });
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
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

}  // namespace dftracer::utils::query
