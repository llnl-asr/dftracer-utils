// SPDX-License-Identifier: MIT
// Every literal leaf of the golden filters, evaluated as that leaf and as the
// same condition in expression form, gives the same result on each record.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/json/canonical.h>
#include <dftracer/utils/json/json_escape.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

namespace duql = dftracer::utils::duql;
using dftracer::utils::json::JsonValue;

namespace {

void leaves(const duql::QueryNode& node,
            dftracer::utils::dstd::Vector<const duql::QueryNode*>& out) {
    std::visit(
        [&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::AndNode> ||
                          std::is_same_v<T, duql::OrNode>) {
                leaves(*n.left, out);
                leaves(*n.right, out);
            } else if constexpr (std::is_same_v<T, duql::NotNode>) {
                leaves(*n.operand, out);
            } else if constexpr (std::is_same_v<T, duql::ExprLeaf>) {
            } else if constexpr (std::is_same_v<T, duql::MatchNode>) {
                if (!n.field.any && n.op != duql::MatchOp::Icontains)
                    out.push_back(&node);
            } else {
                if (!n.field.any) out.push_back(&node);
            }
        },
        node.data);
}

const duql::FieldNode& field_of(const duql::QueryNode& leaf) {
    return std::visit(
        [](const auto& n) -> const duql::FieldNode& {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::CompareNode> ||
                          std::is_same_v<T, duql::InNode> ||
                          std::is_same_v<T, duql::NotInNode> ||
                          std::is_same_v<T, duql::MatchNode>)
                return n.field;
            else
                throw std::logic_error("not a literal leaf");
        },
        leaf.data);
}

// The leaf's text with its field read through if(), which forces the
// expression form without changing the value.
dftracer::utils::dstd::String expression_form(const duql::QueryNode& leaf) {
    const dftracer::utils::dstd::String text = duql::to_string(leaf);
    const dftracer::utils::dstd::String& path = field_of(leaf).path;
    const dftracer::utils::dstd::String wrapped =
        "if(true, " + path + ", null)";
    const auto at = text.find(path);
    REQUIRE(at == 0);
    return wrapped + text.substr(path.size());
}

dftracer::utils::dstd::String json_literal(const duql::LiteralValue& v) {
    return std::visit(
        [](const auto& x) -> dftracer::utils::dstd::String {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, dftracer::utils::dstd::String>) {
                dftracer::utils::dstd::String out = "\"";
                dftracer::utils::json::append_json_escaped(out, x);
                return out + "\"";
            } else if constexpr (std::is_same_v<T, bool>) {
                return x ? "true" : "false";
            } else if constexpr (std::is_same_v<T, double>) {
                return dftracer::utils::dstd::fixed_text(x);
            } else {
                return dftracer::utils::dstd::int_text(x);
            }
        },
        v);
}

// Values to place at the leaf's field: its literals and their neighbours,
// every JSON type, and nothing at all.
dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> values_for(
    const duql::QueryNode& leaf) {
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> out = {
        "null",  "true", "false", "0",         "1.5", "\"\"",
        "\"a\"", "[1]",  "[]",    "{\"k\":1}", "-7"};
    auto add = [&](const duql::LiteralValue& v) {
        out.push_back(json_literal(v));
        if (const auto* i = std::get_if<std::int64_t>(&v)) {
            out.push_back(dftracer::utils::dstd::int_text(*i + 1));
            out.push_back(dftracer::utils::dstd::int_text(*i) + ".5");
        } else if (const auto* u = std::get_if<std::uint64_t>(&v)) {
            out.push_back(dftracer::utils::dstd::int_text(*u + 1));
            out.push_back(dftracer::utils::dstd::int_text(*u) + ".0");
        } else if (const auto* s =
                       std::get_if<dftracer::utils::dstd::String>(&v)) {
            out.push_back(json_literal(*s + "x"));
        }
    };
    std::visit(
        [&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::CompareNode>) {
                add(n.value.value);
            } else if constexpr (std::is_same_v<T, duql::InNode> ||
                                 std::is_same_v<T, duql::NotInNode>) {
                for (const auto& e : n.values.elements) add(e.value);
            } else if constexpr (std::is_same_v<T, duql::MatchNode>) {
                out.push_back(json_literal(n.pattern));
            }
        },
        leaf.data);
    return out;
}

// `value` placed at `path` (dotted, object keys only), as a record.
dftracer::utils::dstd::String record_at(
    const dftracer::utils::dstd::String& path,
    const dftracer::utils::dstd::String& value) {
    dftracer::utils::dstd::String out;
    std::size_t start = 0, depth = 0;
    for (;;) {
        const auto dot = path.find('.', start);
        dftracer::utils::dstd::String key = "\"";
        dftracer::utils::json::append_json_escaped(
            key, path.substr(start, dot - start));
        out += "{" + key + "\":";
        ++depth;
        if (dot == dftracer::utils::dstd::String::npos) break;
        start = dot + 1;
    }
    out += value;
    out.append(depth, '}');
    return out;
}

// The field map a reader builds for `path` from `record`.
duql::ValueMap map_of(const dftracer::utils::dstd::String& path,
                      simdjson::dom::element record) {
    duql::ValueMap m;
    JsonValue v = JsonValue(record).at(path);
    if (!v.exists() && !path.starts_with("args."))
        v = JsonValue(record).at("args." + path);
    if (!v.exists()) return m;
    const auto el = v.element();
    using T = simdjson::dom::element_type;
    switch (el.type()) {
        case T::NULL_VALUE:
            m[path] = duql::Cell::null();
            break;
        case T::ARRAY:
        case T::OBJECT: {
            dftracer::utils::dstd::String text;
            dftracer::utils::json::append_canonical_json(text, el);
            m[path] = duql::Cell::json(text, el.is_array());
            break;
        }
        case T::STRING:
            m[path] = dftracer::utils::dstd::String(el.get_string().value());
            break;
        case T::INT64:
            m[path] = el.get_int64().value();
            break;
        case T::UINT64:
            m[path] = el.get_uint64().value();
            break;
        case T::DOUBLE:
            m[path] = el.get_double().value();
            break;
        case T::BOOL:
            m[path] = el.get_bool().value();
            break;
        default:
            break;
    }
    return m;
}

}  // namespace

TEST_CASE("literal leaves and their expression form agree") {
    std::ifstream in(DUQL_GOLDEN_PATH);
    REQUIRE(in);
    simdjson::dom::parser json;
    simdjson::dom::parser records;
    std::size_t compared = 0;
    for (dftracer::utils::dstd::String line; std::getline(in, line);) {
        if (line.empty()) continue;
        simdjson::dom::element row;
        const simdjson::padded_string padded(line);
        REQUIRE(json.parse(padded).get(row) == simdjson::SUCCESS);
        if (!row["ok"].get_bool().value()) continue;
        const auto tree = duql::parse(
            dftracer::utils::dstd::String(row["q"].get_string().value()));
        REQUIRE(tree);
        dftracer::utils::dstd::Vector<const duql::QueryNode*> found;
        leaves(**tree, found);
        for (const duql::QueryNode* leaf : found) {
            const dftracer::utils::dstd::String& path = field_of(*leaf).path;
            if (path.find_first_of("[`") != dftracer::utils::dstd::String::npos)
                continue;
            const dftracer::utils::dstd::String form = expression_form(*leaf);
            CAPTURE(form);
            const auto expr = duql::parse(form);
            REQUIRE_MESSAGE(expr.has_value(),
                            (expr ? dftracer::utils::dstd::String()
                                  : expr.error().message));
            REQUIRE(std::holds_alternative<duql::ExprLeaf>((*expr)->data));
            dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>
                placed = {path};
            if (path.find('.') == dftracer::utils::dstd::String::npos)
                placed.push_back("args." + path);
            for (const auto& at : placed)
                for (const auto& value : values_for(*leaf)) {
                    const dftracer::utils::dstd::String text =
                        record_at(at, value);
                    CAPTURE(text);
                    simdjson::dom::element record;
                    REQUIRE(
                        records.parse(text.data(), text.size()).get(record) ==
                        simdjson::SUCCESS);
                    const JsonValue jv(record);
                    CHECK(duql::evaluate_truth(*leaf, jv) ==
                          duql::evaluate_truth(**expr, jv));
                    const auto map = map_of(path, record);
                    CHECK(duql::evaluate_truth(*leaf, map) ==
                          duql::evaluate_truth(**expr, map));
                    ++compared;
                }
            simdjson::dom::element empty;
            REQUIRE(records.parse(std::string("{}")).get(empty) ==
                    simdjson::SUCCESS);
            CHECK(duql::evaluate_truth(*leaf, JsonValue(empty)) ==
                  duql::evaluate_truth(**expr, JsonValue(empty)));
        }
    }
    CHECK(compared > 1000);
}
