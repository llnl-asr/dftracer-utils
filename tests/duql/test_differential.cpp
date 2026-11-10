// SPDX-License-Identifier: MIT
// Every literal leaf of the golden filters, evaluated as that leaf and as the
// same condition in expression form, gives the same result on each record.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/json/canonical.h>
#include <dftracer/utils/json/json_escape.h>
#include <doctest/doctest.h>
#include <simdjson.h>
#include <testing_utilities.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <variant>

using namespace dftracer::utils;

namespace {

void leaves(const duql::detail::QueryNode& node,
            dstd::Vector<const duql::detail::QueryNode*>& out) {
    std::visit(
        [&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::detail::AndNode> ||
                          std::is_same_v<T, duql::detail::OrNode>) {
                leaves(*n.left, out);
                leaves(*n.right, out);
            } else if constexpr (std::is_same_v<T, duql::detail::NotNode>) {
                leaves(*n.operand, out);
            } else if constexpr (std::is_same_v<T, duql::detail::ExprLeaf>) {
            } else if constexpr (std::is_same_v<T, duql::detail::MatchNode>) {
                if (!n.field.any && n.op != duql::MatchOp::Icontains)
                    out.push_back(&node);
            } else {
                if (!n.field.any) out.push_back(&node);
            }
        },
        node.data);
}

const duql::detail::FieldNode& field_of(const duql::detail::QueryNode& leaf) {
    return std::visit(
        [](const auto& n) -> const duql::detail::FieldNode& {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::detail::CompareNode> ||
                          std::is_same_v<T, duql::detail::InNode> ||
                          std::is_same_v<T, duql::detail::NotInNode> ||
                          std::is_same_v<T, duql::detail::MatchNode>)
                return n.field;
            else
                throw std::logic_error("not a literal leaf");
        },
        leaf.data);
}

// The leaf's text with its field read through if(), which forces the
// expression form without changing the value.
dstd::String expression_form(const duql::detail::QueryNode& leaf) {
    const dstd::String text = duql::detail::to_string(leaf);
    const dstd::String& path = field_of(leaf).path;
    const dstd::String wrapped = dstd::fmt<"if(true, {}, null)">(path);
    const auto at = text.find(path);
    REQUIRE(at == 0);
    return wrapped + text.substr(path.size());
}

dstd::String json_literal(const duql::LiteralValue& v) {
    return std::visit(
        [](const auto& x) -> dstd::String {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, dstd::String>) {
                dstd::String out = "\"";
                dftracer::utils::json::append_json_escaped(out, x);
                return dstd::fmt<"{}\"">(out);
            } else if constexpr (std::is_same_v<T, bool>) {
                return x ? "true" : "false";
            } else if constexpr (std::is_same_v<T, double>) {
                return dstd::fixed_text(x);
            } else {
                return dstd::int_text(x);
            }
        },
        v);
}

// Values to place at the leaf's field: its literals and their neighbours,
// every JSON type, and nothing at all.
dstd::Vector<dstd::String> values_for(const duql::detail::QueryNode& leaf) {
    dstd::Vector<dstd::String> out = {"null", "true",      "false", "0",
                                      "1.5",  "\"\"",      "\"a\"", "[1]",
                                      "[]",   "{\"k\":1}", "-7"};
    auto add = [&](const duql::LiteralValue& v) {
        out.push_back(json_literal(v));
        if (const auto* i = std::get_if<std::int64_t>(&v)) {
            out.push_back(dstd::int_text(*i + 1));
            out.push_back(dstd::fmt<"{}.5">(*i));
        } else if (const auto* u = std::get_if<std::uint64_t>(&v)) {
            out.push_back(dstd::int_text(*u + 1));
            out.push_back(dstd::fmt<"{}.0">(*u));
        } else if (const auto* s = std::get_if<dstd::String>(&v)) {
            out.push_back(json_literal(dstd::fmt<"{}x">(*s)));
        }
    };
    std::visit(
        [&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::detail::CompareNode>) {
                add(n.value.value);
            } else if constexpr (std::is_same_v<T, duql::detail::InNode> ||
                                 std::is_same_v<T, duql::detail::NotInNode>) {
                for (const auto& e : n.values.elements) add(e.value);
            } else if constexpr (std::is_same_v<T, duql::detail::MatchNode>) {
                out.push_back(json_literal(n.pattern));
            }
        },
        leaf.data);
    return out;
}

// `value` placed at `path` (dotted, object keys only), as a record.
dstd::String record_at(const dstd::String& path, const dstd::String& value) {
    dstd::String out;
    std::size_t start = 0, depth = 0;
    for (;;) {
        const auto dot = path.find('.', start);
        dstd::String key = "\"";
        dftracer::utils::json::append_json_escaped(
            key, path.substr(start, dot - start));
        out += dstd::fmt<"{{}\":">(key);
        ++depth;
        if (dot == dstd::String::npos) break;
        start = dot + 1;
    }
    out += value;
    out.append(depth, '}');
    return out;
}

// The field map a reader builds for `path` from `record`.
duql::ValueMap map_of(const dstd::String& path, simdjson::dom::element record) {
    duql::ValueMap m;
    json::JsonValue v = json::JsonValue(record).at(path);
    if (!v.exists() && !path.starts_with("args."))
        v = json::JsonValue(record).at(dstd::fmt<"args.{}">(path));
    if (!v.exists()) return m;
    const auto el = v.element();
    using T = simdjson::dom::element_type;
    switch (el.type()) {
        case T::NULL_VALUE:
            m[path] = duql::Cell::null();
            break;
        case T::ARRAY:
        case T::OBJECT: {
            dstd::String text;
            dftracer::utils::json::append_canonical_json(text, el);
            m[path] = duql::Cell::json(text, el.is_array());
            break;
        }
        case T::STRING:
            m[path] = dstd::String(el.get_string().value());
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
    const auto golden = dstd::fs::read_file(DUQL_GOLDEN_PATH);
    REQUIRE(golden.has_value());
    dftu_utils_test::LineStream in(*golden);
    simdjson::dom::parser json;
    simdjson::dom::parser records;
    std::size_t compared = 0;
    for (dstd::String line; in.next(line);) {
        if (line.empty()) continue;
        simdjson::dom::element row;
        const simdjson::padded_string padded(line);
        REQUIRE(json.parse(padded).get(row) == simdjson::SUCCESS);
        if (!row["ok"].get_bool().value()) continue;
        const auto tree =
            duql::parse(dstd::String(row["q"].get_string().value()));
        REQUIRE(tree);
        dstd::Vector<const duql::detail::QueryNode*> found;
        leaves(**tree, found);
        for (const duql::detail::QueryNode* leaf : found) {
            const dstd::String& path = field_of(*leaf).path;
            if (path.find_first_of("[`") != dstd::String::npos) continue;
            const dstd::String form = expression_form(*leaf);
            CAPTURE(form);
            const auto expr = duql::parse(form);
            REQUIRE_MESSAGE(expr.has_value(),
                            (expr ? dstd::String() : expr.error().message));
            REQUIRE(
                std::holds_alternative<duql::detail::ExprLeaf>((*expr)->data));
            dstd::Vector<dstd::String> placed = {path};
            if (path.find('.') == dstd::String::npos)
                placed.push_back(dstd::fmt<"args.{}">(path));
            for (const auto& at : placed)
                for (const auto& value : values_for(*leaf)) {
                    const dstd::String text = record_at(at, value);
                    CAPTURE(text);
                    simdjson::dom::element record;
                    REQUIRE(
                        records.parse(text.data(), text.size()).get(record) ==
                        simdjson::SUCCESS);
                    const json::JsonValue jv(record);
                    CHECK(duql::evaluate_truth(*leaf, jv) ==
                          duql::evaluate_truth(**expr, jv));
                    const auto map = map_of(path, record);
                    CHECK(duql::evaluate_truth(*leaf, map) ==
                          duql::evaluate_truth(**expr, map));
                    ++compared;
                }
            simdjson::dom::element empty;
            REQUIRE(records.parse(simdjson::padded_string(dstd::String("{}")))
                        .get(empty) == simdjson::SUCCESS);
            CHECK(duql::evaluate_truth(*leaf, json::JsonValue(empty)) ==
                  duql::evaluate_truth(**expr, json::JsonValue(empty)));
        }
    }
    CHECK(compared > 1000);
}
