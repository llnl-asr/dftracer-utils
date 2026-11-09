// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/mask.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/pipeline.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>
#include <limits>

using namespace dftracer::utils;

using namespace dftracer::utils::duql;

namespace {

constexpr std::int64_t N = 8;
constexpr std::int64_t I64_MAX = std::numeric_limits<std::int64_t>::max();
constexpr std::uint64_t U64_MAX = std::numeric_limits<std::uint64_t>::max();

using OptI = dstd::Optional<std::int64_t>;
using OptU = dstd::Optional<std::uint64_t>;
using OptD = dstd::Optional<double>;
using OptS = dstd::Optional<dstd::String>;
using OptB = dstd::Optional<bool>;

const dstd::Vector<OptI> I = {1, -7, 0, I64_MAX, {}, 9, -3, 6};
const dstd::Vector<OptI> J = {2, 3, 0, 1, 5, {}, -2, 4};
const dstd::Vector<OptD> F = {0.5, -2.5, 0.0, 1e300, 2.0, {}, 3.0, 6.0};
const dstd::Vector<OptU> U = {1, U64_MAX, 0, 5, 2, 3, {}, 7};
const dstd::Vector<OptS> S = {"a", "B", "", {}, "abc", " x y ", "Ab", "zz"};
const dstd::Vector<OptS> T = {"a", "B", "", "q", "bc", " x", "b", {}};
const dstd::Vector<OptB> B = {true, false, {}, true, false, true, false, true};

template <class T>
dstd::Vector<std::uint8_t> validity(const dstd::Vector<dstd::Optional<T>>& v) {
    dstd::Vector<std::uint8_t> bits((v.size() + 7) / 8, 0);
    for (std::size_t r = 0; r < v.size(); ++r)
        if (v[r]) bits[r / 8] |= static_cast<std::uint8_t>(1U << (r % 8));
    return bits;
}

template <class T>
dataframe::Series numbers(dataframe::TypeId type,
                          const dstd::Vector<dstd::Optional<T>>& v) {
    dstd::Vector<T> data;
    for (const auto& x : v) data.push_back(x.value_or(T{}));
    const auto bits = validity(v);
    return dataframe::Series::flat(type, data.data(), N, bits.data());
}

dataframe::DataFrame frame() {
    dataframe::DataFrame f;
    f.names = {"i", "j", "f", "u", "s", "b", "t"};
    f.columns.push_back(numbers(dataframe::TypeId::Int64, I));
    f.columns.push_back(numbers(dataframe::TypeId::Int64, J));
    f.columns.push_back(numbers(dataframe::TypeId::Float64, F));
    f.columns.push_back(numbers(dataframe::TypeId::Uint64, U));
    dstd::Vector<dstd::StringView> s;
    for (const auto& x : S) s.push_back(x ? dstd::StringView(*x) : "");
    const auto sbits = validity(S);
    f.columns.push_back(dataframe::Series::strings(s, sbits.data()));
    dstd::Vector<std::uint8_t> b;
    for (const auto& x : B) b.push_back(x.value_or(false) ? 1 : 0);
    dstd::Vector<std::uint8_t> packed((N + 7) / 8, 0);
    for (std::size_t r = 0; r < b.size(); ++r)
        if (b[r]) packed[r / 8] |= static_cast<std::uint8_t>(1U << (r % 8));
    const auto bbits = validity(B);
    f.columns.push_back(dataframe::Series::flat(
        dataframe::TypeId::Bool, packed.data(), N, bbits.data()));
    dstd::Vector<dstd::StringView> t;
    for (const auto& x : T) t.push_back(x ? dstd::StringView(*x) : "");
    const auto tbits = validity(T);
    f.columns.push_back(dataframe::Series::strings(t, tbits.data()));
    return f;
}

template <class T>
Cell cell(const dstd::Optional<T>& v) {
    return v ? Cell(*v) : Cell::null();
}

ValueMap row(std::size_t r) {
    ValueMap m;
    m["i"] = cell(I[r]);
    m["j"] = cell(J[r]);
    m["f"] = cell(F[r]);
    m["u"] = cell(U[r]);
    m["s"] = S[r] ? Cell(*S[r]) : Cell::null();
    m["b"] = cell(B[r]);
    m["t"] = T[r] ? Cell(*T[r]) : Cell::null();
    return m;
}

Truth column_truth(const dataframe::Series& mask, std::int64_t r) {
    if (mask.is_null(r)) return Truth::Unknown;
    const auto* bits = mask.data<std::uint8_t>();
    return (bits[r / 8] >> (r % 8)) & 1 ? Truth::Yes : Truth::No;
}

const char* name(Truth t) {
    return t == Truth::Yes ? "YES" : t == Truth::No ? "NO" : "UNKNOWN";
}

}  // namespace

TEST_CASE("vectorized expressions agree with the row evaluator") {
    const char* const FORMS[] = {
        "i + j > 0",
        "i - j < 0",
        "i * j >= 0",
        "i * 2 > 0",
        "i / j > 1",
        "i // j == 0",
        "i % j == 1",
        "i // j < 0",
        "i % j < 0",
        "f / j > 0",
        "f // j >= 0",
        "f % j > 0",
        "-i < 0",
        "u + i > 0",
        "u - 1 > 0",
        "u == i",
        "i == f",
        "i < f",
        "u > f",
        "s == \"a\"",
        "s < \"b\"",
        "s == i",
        "i + s > 0",
        "b",
        "i",
        "not b",
        "b and i > 0",
        "b or i > 0",
        "i between j and 10",
        "i not between 0 and j",
        "i in [1, 0, 9]",
        "i in [j, 1]",
        "i in [\"a\", 1]",
        "s in [\"a\", \"zz\"]",
        "i not in [1, j]",
        "i is null",
        "s is not null",
        "i is missing",
        "(i ?? j) > 0",
        "coalesce(i, j, 0) > 0",
        "if(b, i, j) > 0",
        "case(i > 5, 1, i > 0, 2, 3) == 2",
        "abs(i) > 5",
        "abs(j) == 2",
        "floor(f) == 0",
        "ceil(f) == 1",
        "round(f) == 1",
        "round(f, 1) == 0.5",
        "min(i, j) < 2",
        "max(i, j, 3) == 3",
        "max(f, i) > 1",
        "log(f) < 0",
        "exp(f) > 1",
        "pow(f, 2) > 1",
        "len(s) == 1",
        "concat(s, i) == \"a1\"",
        "concat(s, b) == \"atrue\"",
        "lower(s) == \"ab\"",
        "upper(s) == \"AB\"",
        "trim(s) == \"x y\"",
        "starts_with(s, \"a\")",
        "ends_with(s, \"b\")",
        "contains(s, \"b\")",
        "substr(s, 1) == \"bc\"",
        "substr(s, 0, 1) == \"a\"",
        "replace(s, \"a\", \"q\") == \"qbc\"",
        "starts_with(s, t)",
        "ends_with(s, t)",
        "contains(s, t)",
        "starts_with(s, \"a\") and contains(s, t)",
        "replace(s, t, \"q\") == \"qbc\"",
        "replace(s, \"a\", t) == \"bbc\"",
        "replace(s, t, t) == s",
        "substr(s, j) == \"c\"",
        "substr(s, i) == \"\"",
        "substr(s, i, j) == \"b\"",
        "substr(s, 1, j) == \"bc\"",
        "substr(s, i, 1) == \"a\"",
        "round(f, j) == 0.5",
        "round(i, j) == 9",
        "round(u, j) == 1",
        "round(1.23456, j) == 1.2",
        "extract(s, \"(b)\", j) == \"b\"",
        "extract(s, \"(a)(b)?\", i) == \"a\"",
        "json(s) == '\"a\"'",
        "json(i) == \"1\"",
        "json(f) == \"0.5\"",
        "type(i) == \"number\"",
        "type(s) == \"string\"",
        "int(f) == 0",
        "float(i) > 1",
        "string(i) == \"9\"",
        "string(f) == \"0.5\"",
        "string(b) == \"true\"",
        "s like \"a%\"",
        "s ilike \"a%\"",
        "s ~ \"^a\"",
        "s ~* \"^a\"",
        "s !~ \"b\"",
        "extract(s, \"(b)\") == \"b\"",
        "regex_replace(s, \"(?<x>[a-z])\", \"<${x}$1>\") == \"<aa>\"",
        "regex_replace(s, \"x*\", \"-\") == \"-a-\"",
        "regex_replace(s, \"(q)|b\", \"[$1]\") == \"[]\"",
        "regex_replace(s, \"b\", \"$$\") == \"a$\"",
        "exists(i)",
        "i + 1 > 0 and f > 0",
    };
    const dataframe::DataFrame f = frame();
    for (const char* text : FORMS) {
        INFO("query: ", dstd::String(text));
        auto q = parse(text);
        REQUIRE_MESSAGE(q.has_value(), (q ? "" : q.error().format()));
        const dataframe::Series mask =
            dftu_utils_test::ok(dataframe::evaluate_mask(**q, f));
        for (std::int64_t r = 0; r < N; ++r) {
            const Truth want =
                evaluate_truth(**q, row(static_cast<std::size_t>(r)));
            const Truth got = column_truth(mask, r);
            CHECK_MESSAGE(got == want, "row ", r, ": columns ",
                          dstd::String(name(got)), ", rows ",
                          dstd::String(name(want)));
        }
    }
}

TEST_CASE("calendar time agrees between the column and row evaluators") {
    const dstd::Vector<OptI> TS = {1700000000123456,
                                   -1,
                                   0,
                                   {},
                                   -86400000000,
                                   951782400000000,
                                   -62135596800000000,
                                   253402300799999999};
    const dstd::Vector<OptD> SECS = {1700000000.9, -0.5,  0.0,   2.5,
                                     {},           -1e30, 1e300, 86399.0};
    dataframe::DataFrame f;
    f.names = {"ts", "secs", "s"};
    f.columns.push_back(numbers(dataframe::TypeId::Int64, TS));
    f.columns.push_back(numbers(dataframe::TypeId::Float64, SECS));
    f.columns.push_back(
        dataframe::Series::strings({"a", "b", "c", "d", "e", "f", "g", "h"}));
    Roles roles;
    roles.fields.emplace("ts", 1000);
    roles.fields.emplace("secs", 1000000000);
    dstd::Vector<dstd::String> forms;
    for (const char* col : {"ts", "secs"}) {
        for (const char* part :
             {"year", "month", "day", "hour", "minute", "second", "millisecond",
              "microsecond", "nanosecond", "day_of_week", "day_of_year",
              "quarter", "iso_week", "iso_year"})
            for (const char* k : {"-1", "0", "1", "3", "6", "12", "23", "59",
                                  "364", "999", "1969", "2022"})
                forms.push_back(dstd::String("date_part(") + col + ", \"" +
                                part + "\") > " + k);
        for (const char* fmt : {"%F %T.%f", "%Y-%m-%d %H:%M:%S %j",
                                "%A %B %p %I %y", "%s %z %Z %%"})
            for (const char* k : {"\"0\"", "\"1969\"", "\"2000\"",
                                  "\"2023-11-14\"", "\"A\"", "\"Thu\""})
                forms.push_back(dstd::String("format_time(") + col + ", \"" +
                                fmt + "\") < " + k);
    }
    forms.push_back("date_part(as_time(s, \"us\"), \"hour\") > 0");
    for (const dstd::String& form : forms) {
        const dstd::String text = "where " + form;
        INFO("query: ", text);
        auto p = compile_program(text, {}, &roles);
        REQUIRE_MESSAGE(p.has_value(), (p ? "" : p.error().format()));
        REQUIRE(p->main.filter);
        const dataframe::Series mask =
            dftu_utils_test::ok(dataframe::evaluate_mask(*p->main.filter, f));
        for (std::int64_t r = 0; r < N; ++r) {
            ValueMap m;
            m["ts"] = cell(TS[static_cast<std::size_t>(r)]);
            m["secs"] = cell(SECS[static_cast<std::size_t>(r)]);
            m["s"] = Cell(dstd::String(1, static_cast<char>('a' + r)));
            const Truth want = evaluate_truth(*p->main.filter, m);
            const Truth got = column_truth(mask, r);
            CHECK_MESSAGE(got == want, "row ", r, ": columns ",
                          dstd::String(name(got)), ", rows ",
                          dstd::String(name(want)));
        }
    }
}

TEST_CASE("quantifiers over a list column agree with the row evaluator") {
    // l: [1, 5], [], null, [2, null], [7]; s: a, b, c, d, e
    const dstd::Vector<std::int32_t> offs = {0, 2, 2, 2, 4, 5};
    const dstd::Vector<std::int64_t> vals = {1, 5, 2, 0, 7};
    const dstd::Vector<std::uint8_t> valid = {0x17};  // element 3 is null
    dataframe::DataFrame f;
    f.names = {"l", "s"};
    f.columns.push_back(
        dataframe::Series::list(
            offs, dataframe::Series::flat_i64(vals.data(), 5, valid.data()))
            .take(dstd::Vector<std::int64_t>{0, 1, -1, 3, 4}));
    f.columns.push_back(dataframe::Series::strings({"a", "b", "c", "d", "e"}));
    const char* const JSON[] = {"[1,5]", "[]", nullptr, "[2,null]", "[7]"};
    const char* const S5[] = {"a", "b", "c", "d", "e"};
    const char* const FORMS[] = {
        "any(l, . > 3)",
        "all(l, . > 0)",
        "not any(l, . > 3)",
        "not all(l, . < 7)",
        "any(l, . == 2) or s == \"c\"",
        "any(l, . > 1 and ^.s != \"e\")",
        "all(l) > 0",
    };
    for (const char* text : FORMS) {
        INFO("query: ", dstd::String(text));
        auto q = parse(text);
        REQUIRE_MESSAGE(q.has_value(), (q ? "" : q.error().format()));
        const dataframe::Series mask =
            dftu_utils_test::ok(dataframe::evaluate_mask(**q, f));
        for (std::int64_t r = 0; r < 5; ++r) {
            ValueMap m;
            if (JSON[r]) m["l"] = Cell::json(JSON[r], true);
            m["s"] = Cell(dstd::String(S5[r]));
            const Truth want = evaluate_truth(**q, m);
            const Truth got = column_truth(mask, r);
            CHECK_MESSAGE(got == want, "row ", r, ": columns ",
                          dstd::String(name(got)), ", rows ",
                          dstd::String(name(want)));
        }
    }
}
