#include <dftracer/utils/index/plan/resolved_field_rewriter.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/query/ast.h>

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dftracer::utils::index::plan {

namespace {

namespace q = query;
using index::store::IndexDatabase;

bool is_resolved(const std::string& path) {
    return std::string_view(path).starts_with(RESOLVED_PREFIX);
}

// A literal as a dictionary stores the value: strings as written, numbers
// as their JSON text; nullopt for a boolean.
std::optional<std::string> literal_text(const q::LiteralNode& lit) {
    if (const auto* s = std::get_if<std::string>(&lit.value)) return *s;
    if (const auto* i = std::get_if<std::int64_t>(&lit.value))
        return std::to_string(*i);
    if (const auto* u = std::get_if<std::uint64_t>(&lit.value))
        return std::to_string(*u);
    if (const auto* d = std::get_if<double>(&lit.value)) {
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof(buf), *d);
        return std::string(buf, r.ptr);
    }
    return std::nullopt;
}

// A stored value as the evaluator compares it: a number when the whole text
// is one, the text otherwise.
q::LiteralValue stored_value(std::string text) {
    const char* end = text.data() + text.size();
    std::int64_t i = 0;
    if (auto r = std::from_chars(text.data(), end, i);
        r.ec == std::errc{} && r.ptr == end && !text.empty())
        return i;
    double d = 0;
    if (auto r = std::from_chars(text.data(), end, d);
        r.ec == std::errc{} && r.ptr == end && !text.empty())
        return d;
    return text;
}

// Keys of the rows whose field satisfies `leaf`: exact values through the
// reverse entries, any other leaf evaluated on every row.
std::vector<std::string> matching_keys(const q::QueryNode& leaf,
                                       const std::string& path,
                                       const IndexDatabase& db,
                                       const ResolvedColumn& rc) {
    const std::string& dict = rc.dictionary->name;
    std::vector<std::string> keys;
    auto add_value = [&](const q::LiteralNode& lit) {
        if (const auto text = literal_text(lit))
            for (auto& k : db.dict_keys(dict, rc.field, *text))
                keys.push_back(std::move(k));
    };
    if (const auto* n = std::get_if<q::CompareNode>(&leaf.data);
        n && n->op == q::CompareOp::EQ) {
        add_value(n->value);
        return keys;
    }
    if (const auto* n = std::get_if<q::InNode>(&leaf.data)) {
        for (const auto& e : n->values.elements) add_value(e);
        return keys;
    }
    q::ValueMap row;
    auto& value = row[path];
    for (auto& [key, v] : db.dict_field(dict, rc.field)) {
        value = stored_value(std::move(v));
        if (q::evaluate(leaf, row)) keys.push_back(key);
    }
    return keys;
}

q::QueryNodePtr transform(const q::QueryNode& node, const IndexDatabase& db,
                          const RecordSchema& schema, bool& changed) {
    return std::visit(
        [&](const auto& n) -> q::QueryNodePtr {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, q::AndNode> ||
                          std::is_same_v<T, q::OrNode>) {
                return q::make_node(
                    T{transform(*n.left, db, schema, changed),
                      transform(*n.right, db, schema, changed)});
            } else if constexpr (std::is_same_v<T, q::NotNode>) {
                return q::make_node(
                    q::NotNode{transform(*n.operand, db, schema, changed)});
            } else {
                if (!is_resolved(n.field.path)) return q::make_node(T{n});
                changed = true;
                const ResolvedColumn rc = schema.resolved_column(n.field.path);
                q::ArrayNode keys;
                for (auto& k : matching_keys(node, n.field.path, db, rc))
                    keys.elements.push_back(q::LiteralNode{std::move(k)});
                return q::make_node(
                    q::InNode{q::FieldNode{rc.key_field}, std::move(keys)});
            }
        },
        node.data);
}

}  // namespace

bool has_resolved_fields(const q::Query& query) {
    for (const auto& f : query.fields())
        if (f.starts_with(RESOLVED_PREFIX)) return true;
    return false;
}

std::optional<q::Query> rewrite_resolved_fields(const q::Query& query,
                                                const IndexDatabase& db,
                                                const RecordSchema& schema) {
    bool changed = false;
    auto root = transform(query.root(), db, schema, changed);
    if (!changed) return std::nullopt;
    return q::parse_or_throw(q::to_string(*root));
}

std::optional<q::Query> rewrite_resolved_fields(const q::Query& query,
                                                const std::string& index_path,
                                                const std::string& trace) {
    load_index_schemas(index_path);
    IndexDatabase db(index_path, index::store::IndexOpenMode::ReadOnly);
    const RecordSchema* p = recorded_schema(db, trace);
    return rewrite_resolved_fields(query, db,
                                   p ? *p : detect_file_schema(trace));
}

const RecordSchema* recorded_schema(const IndexDatabase& db,
                                    const std::string& trace) {
    const int fid =
        db.get_file_info_id(index::store::internal::get_logical_path(trace));
    if (fid < 0) return nullptr;
    const auto id = db.file_schema(fid);
    return id ? &get_schema(*id) : nullptr;
}

}  // namespace dftracer::utils::index::plan
