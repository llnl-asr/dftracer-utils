// SPDX-License-Identifier: MIT
// The filter-to-duql converter a plugin plan hook is fed through
// (dataframe/query/filter_duql.h): every expression it says reads back, through
// the duql parser and a lowering written here, as the expression it started
// from, and what duql cannot say is refused.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dataframe/expr_ok.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/expr_handle.h>
#include <dftracer/utils/dataframe/query/filter_duql.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/detail/ast.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/term.h>
#include <dftracer/utils/json/json_value.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <variant>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;
using dataframe::query::expr_to_duql;
using dataframe::query::filters_to_duql;

namespace {

constexpr std::int64_t I64_MAX = std::numeric_limits<std::int64_t>::max();
constexpr std::uint64_t U64_MAX = std::numeric_limits<std::uint64_t>::max();

const dstd::Vector<dstd::String> NAMES = {"id",   "dur", "name", "args.size",
                                          "from", "a b", "and"};

dstd::Span<const dstd::String> names() {
    return dstd::Span<const dstd::String>(NAMES.data(), NAMES.size());
}

Expr cmp(CmpOp op, std::int32_t c, dftu_scalar rhs) {
    return expr_cmp(op, col(c), rhs);
}

dstd::Optional<std::int32_t> column_of(dstd::StringView name) {
    for (std::size_t i = 0; i < NAMES.size(); ++i)
        if (NAMES[i] == name) return static_cast<std::int32_t>(i);
    return dstd::nullopt;
}

const duql::TField* field_of(const duql::Term& t) {
    return std::get_if<duql::TField>(&t.node);
}

// The expression a duql filter parses to, for the subset the converter says.
dstd::Optional<Expr> lower_term(const duql::Term& t) {
    if (const auto* is = std::get_if<duql::TIs>(&t.node)) {
        const auto* f = field_of(*is->subject);
        if (!f || is->missing) return dstd::nullopt;
        auto c = column_of(f->base);
        if (!c) return dstd::nullopt;
        return expr_is_null(col(*c), !is->negated);
    }
    if (const auto* call = std::get_if<duql::TCall>(&t.node)) {
        if (call->args.size() != 2) return dstd::nullopt;
        const auto* f = field_of(*call->args[0]);
        const auto* k = std::get_if<duql::TConst>(&call->args[1]->node);
        if (!f || !k) return dstd::nullopt;
        const auto* text = std::get_if<dstd::String>(&k->value);
        auto c = column_of(f->base);
        if (!text || !c) return dstd::nullopt;
        StrPredOp op{};
        switch (call->fn) {
            case duql::Fn::Contains:
                op = StrPredOp::Contains;
                break;
            case duql::Fn::StartsWith:
                op = StrPredOp::StartsWith;
                break;
            case duql::Fn::EndsWith:
                op = StrPredOp::EndsWith;
                break;
            default:
                return dstd::nullopt;
        }
        return expr_str_pred(op, col(*c), *text);
    }
    return dstd::nullopt;
}

dstd::Optional<Expr> lower(const duql::detail::QueryNode& node);

dstd::Optional<Series> list_of(const duql::detail::ArrayNode& array) {
    dstd::Vector<std::int64_t> ints;
    dstd::Vector<double> doubles;
    dstd::Vector<dstd::String> strings;
    for (const duql::LiteralNode& lit : array.elements) {
        if (const auto* i = std::get_if<std::int64_t>(&lit.value))
            ints.push_back(*i);
        else if (const auto* u = std::get_if<std::uint64_t>(&lit.value))
            ints.push_back(static_cast<std::int64_t>(*u));
        else if (const auto* d = std::get_if<double>(&lit.value))
            doubles.push_back(*d);
        else if (const auto* s = std::get_if<dstd::String>(&lit.value))
            strings.push_back(*s);
        else
            return dstd::nullopt;
    }
    if (!ints.empty() && doubles.empty() && strings.empty())
        return Series::flat_i64(
            ints.data(), static_cast<std::int64_t>(ints.size()), nullptr);
    if (!doubles.empty() && ints.empty() && strings.empty())
        return Series::flat_f64(
            doubles.data(), static_cast<std::int64_t>(doubles.size()), nullptr);
    if (!strings.empty() && ints.empty() && doubles.empty())
        return Series::strings(
            dstd::Span<const dstd::String>(strings.data(), strings.size()));
    return dstd::nullopt;
}

dstd::Optional<Expr> lower(const duql::detail::QueryNode& node) {
    using namespace duql::detail;
    if (const auto* c = std::get_if<CompareNode>(&node.data)) {
        auto column = column_of(c->field.path);
        if (!column) return dstd::nullopt;
        CmpOp op{};
        switch (c->op) {
            case duql::CompareOp::Eq:
                op = CmpOp::Eq;
                break;
            case duql::CompareOp::Ne:
                op = CmpOp::Ne;
                break;
            case duql::CompareOp::Gt:
                op = CmpOp::Gt;
                break;
            case duql::CompareOp::Lt:
                op = CmpOp::Lt;
                break;
            case duql::CompareOp::Ge:
                op = CmpOp::Ge;
                break;
            case duql::CompareOp::Le:
                op = CmpOp::Le;
                break;
        }
        // A string literal borrows into the scalar, and expr_cmp copies it.
        if (const auto* i = std::get_if<std::int64_t>(&c->value.value))
            return cmp(op, *column, i64(*i));
        if (const auto* u = std::get_if<std::uint64_t>(&c->value.value))
            return *u <= static_cast<std::uint64_t>(I64_MAX)
                       ? cmp(op, *column, i64(static_cast<std::int64_t>(*u)))
                       : cmp(op, *column, u64(*u));
        if (const auto* d = std::get_if<double>(&c->value.value))
            return cmp(op, *column, f64(*d));
        if (const auto* s = std::get_if<dstd::String>(&c->value.value))
            return cmp(op, *column, str(*s));
        return dstd::nullopt;
    }
    if (const auto* in = std::get_if<InNode>(&node.data)) {
        auto column = column_of(in->field.path);
        auto values = list_of(in->values);
        if (!column || !values) return dstd::nullopt;
        return expr_is_in(col(*column), values->share());
    }
    if (const auto* in = std::get_if<NotInNode>(&node.data)) {
        auto column = column_of(in->field.path);
        auto values = list_of(in->values);
        if (!column || !values) return dstd::nullopt;
        return expr_not(expr_is_in(col(*column), values->share()));
    }
    if (const auto* m = std::get_if<MatchNode>(&node.data)) {
        auto column = column_of(m->field.path);
        if (!column || m->op != duql::MatchOp::Like || m->escape)
            return dstd::nullopt;
        Expr like = expr_str_pred(StrPredOp::Like, col(*column), m->pattern);
        return m->negated ? expr_not(like) : like;
    }
    if (const auto* a = std::get_if<AndNode>(&node.data)) {
        auto l = lower(*a->left);
        auto r = lower(*a->right);
        if (!l || !r) return dstd::nullopt;
        return expr_logical(LogicalOp::And, *l, *r);
    }
    if (const auto* o = std::get_if<OrNode>(&node.data)) {
        auto l = lower(*o->left);
        auto r = lower(*o->right);
        if (!l || !r) return dstd::nullopt;
        return expr_logical(LogicalOp::Or, *l, *r);
    }
    if (const auto* n = std::get_if<NotNode>(&node.data)) {
        auto inner = lower(*n->operand);
        if (!inner) return dstd::nullopt;
        return expr_not(*inner);
    }
    if (const auto* leaf = std::get_if<ExprLeaf>(&node.data))
        return leaf->term ? lower_term(*leaf->term) : dstd::nullopt;
    return dstd::nullopt;
}

// `e` as duql and back.
dstd::Optional<Expr> round_trip(const Expr& e, dstd::String* text = nullptr) {
    auto said = expr_to_duql(e, names());
    if (!said) return dstd::nullopt;
    if (text) *text = *said;
    auto parsed = duql::parse(*said);
    CAPTURE(*said);
    REQUIRE(parsed.has_value());
    return lower(**parsed);
}

void same(const Expr& e) {
    dstd::String text;
    auto back = round_trip(e, &text);
    CAPTURE(text);
    REQUIRE(back.has_value());
    CHECK(expr_canonical(*back) == expr_canonical(e));
}

}  // namespace

TEST_CASE("a comparison with every operator and literal reads back") {
    for (CmpOp op :
         {CmpOp::Gt, CmpOp::Ge, CmpOp::Lt, CmpOp::Le, CmpOp::Eq, CmpOp::Ne}) {
        same(cmp(op, 0, i64(5)));
        same(cmp(op, 1, i64(-7)));
        same(cmp(op, 1, i64(0)));
        same(cmp(op, 1, i64(I64_MAX)));
        same(cmp(op, 1, i64(std::numeric_limits<std::int64_t>::min() + 1)));
        same(cmp(op, 1, f64(2.5)));
        same(cmp(op, 1, f64(-0.125)));
        same(cmp(op, 1, f64(3.0)));
        same(cmp(op, 1, f64(1e20)));
        same(cmp(op, 1, f64(2.5e-7)));
        same(cmp(op, 2, str("POSIX")));
        same(cmp(op, 2, str("")));
        same(cmp(op, 2, str("a\"b\\c\n\t\x01 \xc3\xa9")));
    }
}

TEST_CASE("a name that is not a plain word is quoted") {
    CHECK(*expr_to_duql(cmp(CmpOp::Gt, 3, i64(3)), names()) ==
          "`args.size` > 3");
    CHECK(*expr_to_duql(cmp(CmpOp::Eq, 4, i64(1)), names()) == "`from` == 1");
    CHECK(*expr_to_duql(cmp(CmpOp::Eq, 6, i64(1)), names()) == "`and` == 1");
    CHECK(*expr_to_duql(cmp(CmpOp::Eq, 5, i64(1)), names()) == "`a b` == 1");
    CHECK(*expr_to_duql(cmp(CmpOp::Lt, 0, i64(1)), names()) == "id < 1");
    for (std::int32_t c = 0; c < 7; ++c) same(cmp(CmpOp::Eq, c, i64(1)));
}

TEST_CASE("an unsigned literal that fits int64 reads back as the signed one") {
    auto back = round_trip(cmp(CmpOp::Gt, 0, u64(5)));
    REQUIRE(back.has_value());
    CHECK(expr_canonical(*back) == expr_canonical(cmp(CmpOp::Gt, 0, i64(5))));
}

TEST_CASE("and, or and not read back") {
    const Expr a = cmp(CmpOp::Gt, 0, i64(1));
    const Expr b = cmp(CmpOp::Eq, 2, str("x"));
    const Expr c = cmp(CmpOp::Le, 1, f64(0.5));
    same(expr_logical(LogicalOp::And, a, b));
    same(expr_logical(LogicalOp::Or, a, b));
    same(expr_not(a));
    same(expr_logical(LogicalOp::Or, expr_logical(LogicalOp::And, a, b),
                      expr_not(c)));
    same(expr_logical(LogicalOp::And, expr_logical(LogicalOp::Or, a, b),
                      expr_logical(LogicalOp::Or, b, c)));
    same(expr_not(expr_not(expr_logical(LogicalOp::And, a, c))));
    // The tree is kept, not rebalanced.
    same(expr_logical(LogicalOp::And, a, expr_logical(LogicalOp::And, b, c)));
}

TEST_CASE("in and not in read back") {
    const std::int64_t ints[] = {1, -2, 30};
    const double doubles[] = {0.5, 3.0, -1e10};
    const dstd::String strings[] = {"a", "b\"c", "d e"};
    same(expr_is_in(col(0), Series::flat_i64(ints, 3, nullptr)));
    same(expr_is_in(col(1), Series::flat_f64(doubles, 3, nullptr)));
    same(expr_is_in(
        col(2), Series::strings(dstd::Span<const dstd::String>(strings, 3))));
}

TEST_CASE("is null and is not null read back") {
    same(expr_is_null(col(0), true));
    same(expr_is_null(col(3), false));
}

TEST_CASE("the string predicates duql says read back") {
    same(expr_str_pred(StrPredOp::Contains, col(2), "Send"));
    same(expr_str_pred(StrPredOp::StartsWith, col(2), "H5"));
    same(expr_str_pred(StrPredOp::EndsWith, col(2), "\"q\""));
    same(expr_str_pred(StrPredOp::Like, col(2), "%Send_x%"));
    same(expr_logical(
        LogicalOp::And, expr_str_pred(StrPredOp::StartsWith, col(2), "a"),
        expr_not(expr_str_pred(StrPredOp::Contains, col(2), "b"))));
}

TEST_CASE("what duql cannot say is refused") {
    CHECK_FALSE(expr_to_duql(expr_str_pred(StrPredOp::Matches, col(2), "a.*"),
                             names()));
    CHECK_FALSE(
        expr_to_duql(expr_str_pred(StrPredOp::Search, col(2), "a"), names()));
    CHECK_FALSE(expr_to_duql(cmp(CmpOp::Gt, 0, f64(std::nan(""))), names()));
    CHECK_FALSE(expr_to_duql(
        cmp(CmpOp::Gt, 0, f64(std::numeric_limits<double>::infinity())),
        names()));
    CHECK_FALSE(expr_to_duql(cmp(CmpOp::Gt, 0, u64(U64_MAX)), names()));
    CHECK_FALSE(expr_to_duql(cmp(CmpOp::Gt, 99, i64(1)), names()));
    CHECK_FALSE(expr_to_duql(cmp(CmpOp::Gt, -1, i64(1)), names()));
    CHECK_FALSE(
        expr_to_duql(expr_binary(BinaryOp::Add, col(0), col(1)), names()));
    CHECK_FALSE(expr_to_duql(col(0), names()));
    CHECK_FALSE(expr_to_duql(Expr{}, names()));
    // One refused side refuses the whole.
    CHECK_FALSE(expr_to_duql(
        expr_logical(LogicalOp::And, cmp(CmpOp::Gt, 0, i64(1)),
                     expr_str_pred(StrPredOp::Search, col(2), "a")),
        names()));
    const dstd::Vector<dstd::String> odd = {"has`tick", ""};
    CHECK_FALSE(expr_to_duql(cmp(CmpOp::Eq, 0, i64(1)),
                             dstd::Span<const dstd::String>(odd.data(), 2)));
    CHECK_FALSE(expr_to_duql(cmp(CmpOp::Eq, 1, i64(1)),
                             dstd::Span<const dstd::String>(odd.data(), 2)));
}

TEST_CASE(
    "filters_to_duql joins the filters it can say and keeps their places") {
    const dstd::Vector<Expr> filters = {
        cmp(CmpOp::Gt, 0, i64(2)),
        expr_str_pred(StrPredOp::Search, col(2), "x"),
        expr_logical(LogicalOp::Or, cmp(CmpOp::Lt, 1, i64(5)),
                     expr_is_null(col(1), true)),
        cmp(CmpOp::Eq, 2, str("a"))};
    const auto said = filters_to_duql(
        dstd::Span<const Expr>(filters.data(), filters.size()), names());
    CHECK(said.text == "id > 2 and (dur < 5 or dur is null) and name == \"a\"");
    CHECK(said.index == dstd::Vector<std::int32_t>{0, 2, 3});

    const auto none =
        filters_to_duql(dstd::Span<const Expr>(filters.data() + 1, 1), names());
    CHECK(none.text.empty());
    CHECK(none.index.empty());
    CHECK(filters_to_duql({}, names()).text.empty());
}

namespace {

constexpr std::int64_t I64_MIN = std::numeric_limits<std::int64_t>::min();

// Eight rows with nulls in every column: id (int64), x (float64), s (string).
// Column order is the first three of NAMES.
const char* const ROW_JSON[] = {
    R"({"id":1,"x":1.0,"s":"a%b"})",    R"({"id":2,"x":2.5,"s":"a\\xb"})",
    R"({"id":null,"x":3.0,"s":"axb"})", R"({"id":4,"x":null,"s":null})",
    R"({"id":-5,"x":-5.0,"s":""})",     R"({"id":7,"x":7.0,"s":"A"})",
    R"({"id":7,"x":null,"s":"a%b"})",   R"({"id":null,"x":0.5,"s":null})"};
constexpr std::int64_t ROWS = 8;

const dstd::Vector<dstd::String> ROW_NAMES = {"id", "x", "s"};

dstd::Span<const dstd::String> row_names() {
    return dstd::Span<const dstd::String>(ROW_NAMES.data(), ROW_NAMES.size());
}

struct Rows {
    dstd::Vector<std::int64_t> ids{1, 2, 0, 4, -5, 7, 7, 0};
    std::uint8_t id_valid[1] = {0x7B};
    dstd::Vector<double> xs{1.0, 2.5, 3.0, 0, -5.0, 7.0, 0, 0.5};
    std::uint8_t x_valid[1] = {0xB7};
    dstd::Vector<dstd::StringView> ss{"a%b", "a\\xb", "axb", "",
                                      "",    "A",     "a%b", ""};
    std::uint8_t s_valid[1] = {0x77};
    Series id = Series::flat_i64(ids.data(), ROWS, id_valid);
    Series x = Series::flat_f64(xs.data(), ROWS, x_valid);
    Series s = Series::strings(
        dstd::Span<const dstd::StringView>(ss.data(), ss.size()), s_valid);
};

dstd::Vector<bool> kept_by_expr(const Expr& e, const Rows& rows) {
    const Series* inputs[] = {&rows.id, &rows.x, &rows.s};
    const Series mask =
        dftu_utils_test::eval_ok(e, dstd::Span<const Series* const>(inputs, 3));
    const Series m = mask.materialize();
    dstd::Vector<bool> kept;
    for (std::int64_t i = 0; i < m.length(); ++i) {
        const std::uint8_t* b = m.data<std::uint8_t>();
        kept.push_back(!m.is_null(i) && ((b[i >> 3] >> (i & 7)) & 1) != 0);
    }
    return kept;
}

dstd::Vector<bool> kept_by_duql(dstd::StringView text) {
    auto parsed = duql::parse(text);
    CAPTURE(text);
    REQUIRE(parsed.has_value());
    dstd::Vector<bool> kept;
    for (const char* line : ROW_JSON) {
        simdjson::dom::parser parser;
        auto doc = parser.parse(line, std::strlen(line));
        REQUIRE_FALSE(doc.error());
        kept.push_back(duql::evaluate(
            **parsed, json::JsonValue(doc.value_unsafe()), false));
    }
    return kept;
}

dstd::String bits(dstd::Vector<bool> kept) {
    dstd::String out;
    for (const bool k : kept) out += k ? '1' : '0';
    return out;
}

// `e` keeps the same rows as its duql text, or the converter refuses it.
void agrees(const Expr& e) {
    const Rows rows;
    auto text = expr_to_duql(e, row_names());
    if (!text) return;
    CAPTURE(*text);
    const dstd::String by_expr_text = bits(kept_by_expr(e, rows));
    const char* by_expr = by_expr_text.c_str();
    CAPTURE(by_expr);
    const dstd::String by_duql_text = bits(kept_by_duql(*text));
    const char* by_duql = by_duql_text.c_str();
    CAPTURE(by_duql);
    CHECK(kept_by_expr(e, rows) == kept_by_duql(*text));
}

}  // namespace

TEST_CASE("like with a backslash keeps the rows the expression keeps") {
    const Expr escaped = expr_str_pred(StrPredOp::Like, col(2), "a\\%b");
    const Rows rows;
    // The engine reads `\%` as a literal percent: only "a%b" matches.
    CHECK(kept_by_expr(escaped, rows) == dstd::Vector<bool>{true, false, false,
                                                            false, false, false,
                                                            true, false});
    auto text = expr_to_duql(escaped, row_names());
    REQUIRE(text.has_value());
    CHECK(kept_by_duql(*text) == kept_by_expr(escaped, rows));
    agrees(expr_str_pred(StrPredOp::Like, col(2), "a\\_b"));
    agrees(expr_str_pred(StrPredOp::Like, col(2), "a_b"));
    agrees(expr_str_pred(StrPredOp::Like, col(2), "a%b"));
}

TEST_CASE("the smallest int64 is refused") {
    CHECK_FALSE(expr_to_duql(cmp(CmpOp::Gt, 0, i64(I64_MIN)), names()));
    const std::int64_t ints[] = {I64_MIN, 1};
    CHECK_FALSE(expr_to_duql(
        expr_is_in(col(0), Series::flat_i64(ints, 2, nullptr)), names()));
    // The next one up reads back.
    same(cmp(CmpOp::Gt, 0, i64(I64_MIN + 1)));
}

TEST_CASE("an in under a not is refused: a null cell is kept by not in") {
    const std::int64_t ints[] = {1, 7};
    const Expr in = expr_is_in(col(0), Series::flat_i64(ints, 2, nullptr));
    CHECK(expr_to_duql(in, names()).has_value());
    CHECK_FALSE(expr_to_duql(expr_not(in), names()));
    CHECK_FALSE(expr_to_duql(
        expr_not(expr_logical(LogicalOp::And, in, cmp(CmpOp::Gt, 0, i64(0)))),
        names()));
    CHECK(expr_to_duql(expr_not(expr_not(in)), names()).has_value());
    CHECK(expr_to_duql(expr_not(cmp(CmpOp::Eq, 0, i64(7))), names()));
}

TEST_CASE("every operator keeps the rows its duql text keeps, nulls included") {
    const std::int64_t ints[] = {1, 7};
    const Series int_set = Series::flat_i64(ints, 2, nullptr);
    const double doubles[] = {2.5, 7.0};
    const dstd::String strs[] = {"axb", "A"};
    const Series str_set =
        Series::strings(dstd::Span<const dstd::String>(strs, 2));
    const Expr filters[] = {
        cmp(CmpOp::Ne, 0, i64(2)),
        cmp(CmpOp::Eq, 0, i64(7)),
        cmp(CmpOp::Lt, 0, i64(4)),
        cmp(CmpOp::Ge, 0, i64(4)),
        expr_is_in(col(0), int_set.share()),
        expr_not(expr_is_in(col(0), int_set.share())),
        expr_not(cmp(CmpOp::Eq, 0, i64(7))),
        expr_not(cmp(CmpOp::Lt, 0, i64(4))),
        expr_is_null(col(0), true),
        expr_is_null(col(0), false),
        expr_is_null(col(2), true),
        cmp(CmpOp::Eq, 2, str("axb")),
        cmp(CmpOp::Ne, 2, str("axb")),
        cmp(CmpOp::Lt, 2, str("b")),
        cmp(CmpOp::Eq, 2, str("")),
        expr_is_in(col(2), str_set.share()),
        expr_not(expr_is_in(col(2), str_set.share())),
        expr_str_pred(StrPredOp::Contains, col(2), "%"),
        expr_str_pred(StrPredOp::StartsWith, col(2), "a"),
        expr_str_pred(StrPredOp::EndsWith, col(2), "b"),
        expr_str_pred(StrPredOp::Like, col(2), "a%b"),
        cmp(CmpOp::Gt, 1, i64(2)),
        cmp(CmpOp::Eq, 1, i64(7)),
        cmp(CmpOp::Ne, 1, i64(3)),
        cmp(CmpOp::Lt, 1, f64(3.0)),
        cmp(CmpOp::Gt, 0, f64(2.5)),
        cmp(CmpOp::Eq, 0, f64(7.0)),
        cmp(CmpOp::Ne, 0, f64(7.0)),
        expr_is_in(col(1), Series::flat_f64(doubles, 2, nullptr)),
        expr_not(expr_is_in(col(1), Series::flat_f64(doubles, 2, nullptr))),
        expr_not(cmp(CmpOp::Lt, 1, f64(3.0))),
        expr_is_null(col(1), true),
        expr_logical(LogicalOp::Or, cmp(CmpOp::Gt, 0, i64(3)),
                     cmp(CmpOp::Lt, 1, f64(1.0))),
        expr_logical(LogicalOp::And, cmp(CmpOp::Ne, 0, i64(2)),
                     cmp(CmpOp::Ne, 2, str("A"))),
        expr_not(expr_logical(LogicalOp::Or, cmp(CmpOp::Eq, 0, i64(7)),
                              cmp(CmpOp::Eq, 2, str("axb")))),
        expr_logical(LogicalOp::Or, expr_is_null(col(0), true),
                     cmp(CmpOp::Lt, 0, i64(2)))};
    for (const Expr& e : filters) agrees(e);
}
