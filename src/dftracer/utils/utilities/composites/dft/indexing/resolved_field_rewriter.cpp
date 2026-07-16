#include <dftracer/utils/utilities/common/query/ast.h>
#include <dftracer/utils/utilities/common/query/pattern.h>
#include <dftracer/utils/utilities/composites/dft/indexing/resolved_field_rewriter.h>

#include <regex>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing {

namespace {

namespace q = common::query;
using indexer::IndexDatabase;

struct ResolvedField {
    IndexDatabase::HashType hash_type;
    const char* hash_dim;  // concrete event dimension to rewrite to
};

std::optional<ResolvedField> resolve_virtual_field(std::string_view path) {
    std::string_view rest;
    if (path.rfind("resolved.", 0) == 0) {
        rest = path.substr(9);
    } else if (path.rfind("r.", 0) == 0) {
        rest = path.substr(2);
    } else {
        return std::nullopt;
    }
    if (rest == "fpath" || rest == "cwd")
        return ResolvedField{IndexDatabase::HashType::FILE, "fhash"};
    if (rest == "hostname" || rest == "host")
        return ResolvedField{IndexDatabase::HashType::HOST, "hhash"};
    if (rest == "exec" || rest == "cmd")
        return ResolvedField{IndexDatabase::HashType::STRING, "shash"};
    return std::nullopt;
}

std::optional<std::string> literal_string(const q::LiteralNode& lit) {
    if (const auto* s = std::get_if<std::string>(&lit.value)) return *s;
    return std::nullopt;
}

q::QueryNodePtr make_hash_node(const char* dim,
                               const std::vector<std::string>& hashes,
                               bool negated) {
    q::ArrayNode arr;
    arr.elements.reserve(hashes.size());
    for (const auto& h : hashes) arr.elements.push_back(q::LiteralNode{h});
    q::FieldNode field{std::string(dim)};
    if (negated)
        return q::make_node(q::NotInNode{std::move(field), std::move(arr)});
    return q::make_node(q::InNode{std::move(field), std::move(arr)});
}

std::vector<std::string> resolve_names(const IndexDatabase& db,
                                       const ResolvedField& vf,
                                       const std::vector<std::string>& names) {
    std::vector<std::string> hashes;
    for (const auto& name : names) {
        auto h = db.resolve_name_to_hash(vf.hash_type, name);
        if (h) hashes.push_back(std::move(*h));
    }
    return hashes;
}

std::vector<std::string> match_names(const IndexDatabase& db,
                                     const ResolvedField& vf,
                                     const q::CompiledPattern& pattern) {
    std::vector<std::string> hashes;
    auto table = db.query_hash_table(vf.hash_type);
    for (const auto& [hash, name] : table) {
        if (std::regex_search(name.begin(), name.end(), pattern.re))
            hashes.push_back(hash);
    }
    return hashes;
}

q::QueryNodePtr transform(const q::QueryNode& node, const IndexDatabase& db,
                          bool& changed) {
    return std::visit(
        [&](const auto& n) -> q::QueryNodePtr {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, q::CompareNode>) {
                auto vf = resolve_virtual_field(n.field.path);
                if (vf &&
                    (n.op == q::CompareOp::EQ || n.op == q::CompareOp::NE)) {
                    changed = true;
                    std::vector<std::string> names;
                    if (auto s = literal_string(n.value)) names.push_back(*s);
                    return make_hash_node(vf->hash_dim,
                                          resolve_names(db, *vf, names),
                                          n.op == q::CompareOp::NE);
                }
                return q::make_node(T{n});
            } else if constexpr (std::is_same_v<T, q::InNode> ||
                                 std::is_same_v<T, q::NotInNode>) {
                auto vf = resolve_virtual_field(n.field.path);
                if (vf) {
                    changed = true;
                    std::vector<std::string> names;
                    for (const auto& e : n.values.elements)
                        if (auto s = literal_string(e)) names.push_back(*s);
                    constexpr bool negated = std::is_same_v<T, q::NotInNode>;
                    return make_hash_node(
                        vf->hash_dim, resolve_names(db, *vf, names), negated);
                }
                return q::make_node(T{n});
            } else if constexpr (std::is_same_v<T, q::MatchNode>) {
                auto vf = resolve_virtual_field(n.field.path);
                if (vf && n.compiled) {
                    changed = true;
                    return make_hash_node(vf->hash_dim,
                                          match_names(db, *vf, *n.compiled),
                                          n.negated);
                }
                return q::make_node(T{n});
            } else if constexpr (std::is_same_v<T, q::AndNode>) {
                return q::make_node(
                    q::AndNode{transform(*n.left, db, changed),
                               transform(*n.right, db, changed)});
            } else if constexpr (std::is_same_v<T, q::OrNode>) {
                return q::make_node(
                    q::OrNode{transform(*n.left, db, changed),
                              transform(*n.right, db, changed)});
            } else {  // NotNode
                return q::make_node(
                    q::NotNode{transform(*n.operand, db, changed)});
            }
        },
        node.data);
}

bool node_has_resolved(const q::QueryNode& node) {
    return std::visit(
        [](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, q::CompareNode> ||
                          std::is_same_v<T, q::InNode> ||
                          std::is_same_v<T, q::NotInNode> ||
                          std::is_same_v<T, q::MatchNode>) {
                return resolve_virtual_field(n.field.path).has_value();
            } else if constexpr (std::is_same_v<T, q::AndNode> ||
                                 std::is_same_v<T, q::OrNode>) {
                return node_has_resolved(*n.left) ||
                       node_has_resolved(*n.right);
            } else {  // NotNode
                return node_has_resolved(*n.operand);
            }
        },
        node.data);
}

}  // namespace

bool has_resolved_fields(const q::Query& query) {
    return node_has_resolved(query.root());
}

std::optional<q::Query> rewrite_resolved_fields(const q::Query& query,
                                                const IndexDatabase& db) {
    bool changed = false;
    auto new_root = transform(query.root(), db, changed);
    if (!changed) return std::nullopt;
    auto reparsed = q::Query::from_string(q::to_string(*new_root));
    if (!reparsed) return std::nullopt;
    return std::move(*reparsed);
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing
