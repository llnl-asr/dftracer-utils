// SPDX-License-Identifier: MIT
// The on-demand evaluator gives the same Truth as the DOM evaluator for every
// filter on every event, on the first read and again after a rewind.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_arena.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/detail/query_access.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>
#include <doctest/doctest.h>
#include <duql/filter_corpus.h>
#include <simdjson.h>
#include <testing_utilities.h>

#include <cstddef>
#include <random>

using namespace dftracer::utils;

namespace {

constexpr const char* MORE_EVENTS[] = {
    R"({"dur":5,"args":{"dur":9999,"size":1}})",
    R"({"dur":null,"args":{"dur":5000}})",
    R"({"name":null,"args":{"name":"read"}})",
    R"({"args":{"tags":[]}})",
    R"({"args":{"tags":["a",null,1,2.5,true,{"x":1},[2],"b"]}})",
    R"({"args":{"tags":[null,{"x":1}]}})",
    R"({"args":{"tags":"notarray"}})",
    R"({"tags":["a",{"k":3}],"args":{"tags":["z"]}})",
    R"({"nested":{"a":{"b":{"c":7}}},"args":{"nested":{"a":{"b":{"c":8}}}}})",
    R"({"dur":9223372036854775807,"ts":-9223372036854775808})",
    R"({"dur":9223372036854775808,"ts":18446744073709551615})",
    R"({"dur":1e2,"pid":0.1,"tid":-0.0,"ts":1.5e3})",
    R"({"dur":100,"pid":100.0,"tid":100.5})",
    "{\"name\":\"a\\\"b\",\"cat\":\"line\\nbreak\\u00e9\",\"fname\":\"C:"
    "\\\\dir\\\\f.h5\"}",
    "{\"na\\u006de\":\"read\",\"c\\u0061t\":\"POSIX\",\"dur\":3000}",
    "{\"args\\u002fx\":1,\"ar\\u0067s\":{\"size\":4096,\"s\\u0069ze2\":7}}",
    R"({"args.size":5000,"name":"flat"})",
    R"({"args":{"x.y":1,"x":{"y":2}},"a.b":3,"a":{"b":4}})",
    R"({"x":true,"y":false,"args":{"x":false}})",
    R"({"flag":true,"dur":"2000","size":"4096"})",
    R"({"name":"read","name":"write","dur":1,"dur":2})",
    "{\"na\\u006de\":\"read\",\"name\":\"write\",\"c\\u0061t\":\"POSIX\","
    "\"cat\":\"other\"}",
    "{\"name\":\"write\",\"na\\u006de\":\"read\",\"cat\":\"other\","
    "\"c\\u0061t\":\"POSIX\"}",
    "{\"args\":{\"s\\u0069ze\":1,\"size\":2},\"ar\\u0067s\":{\"size\":3}}",
    R"({"name":"read" , "cat" : "POSIX" ,  "dur" :  2000 , "args" : { "size" : 5 } })",
    R"({"a":{"b":[{"c":1},{"c":2}]},"args":{"a":[1,2,3]}})",
    R"({"": 1, "args": {"": 2}})",
    R"({"dur":2000,"args":{"size":4096,"fd":3,"path":"/data/x","retry":0}})",
    R"({"ph":"M","name":"process_name","args":{"name":"proc"}})",
    R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":2,"ts":10,"dur":20})",
};

constexpr const char* MORE_FILTERS[] = {
    R"(tags.0 == "a")",
    R"(tags.1.k == 3)",
    R"(tags[1].k == 3)",
    R"(args.tags[0] == "z")",
    R"(any(tags) == "a")",
    R"(any(args.tags) == 1)",
    R"(any(args.tags) == 2.5)",
    R"(any(args.tags) == true)",
    R"(any(tags) in ["a", "q"])",
    R"(nested.a.b.c == 7)",
    R"(nested.a.b.c == 8)",
    R"(a.b == 3)",
    R"(a.b == 4)",
    R"(args.x.y == 2)",
    R"(x == true)",
    R"(y == false)",
    R"(dur >= 9223372036854775807)",
    R"(dur > 9223372036854775807)",
    R"(ts < 0)",
    R"(ts == 18446744073709551615)",
    R"(dur == 100)",
    R"(pid == 100)",
    R"(tid == 100.5)",
    R"(dur in [100, 1, 2000])",
    R"(name in ["read", "write", "a\"b"])",
    R"(cat like "line%")",
    R"(name == "a\"b")",
    R"(fname ~ "dir")",
    R"(size == 4096)",
    R"(args.size2 == 7)",
    R"(ph != "M" and dur > 10)",
    R"(ph == "M")",
    R"((dur ?? 0) + (args.size ?? 0) > 5000)",
    R"(exists(args.tags))",
    R"(exists(nested.a))",
    R"(len(args.tags) > 3)",
    R"(args.tags[0] == "a")",
    R"(keys(nested) != null)",
    R"(dur / 2 > 500)",
    R"(upper(name) == "READ")",
    R"(a == null)",
    R"(dur == null)",
    R"(not (name == "read"))",
};

simdjson::dom::element dom_of(simdjson::dom::parser& parser,
                              const simdjson::padded_string& text) {
    simdjson::dom::element el;
    REQUIRE(parser.parse(text).get(el) == simdjson::SUCCESS);
    return el;
}

struct Tally {
    std::size_t pairs = 0;
    std::size_t yes = 0;
    std::size_t no = 0;
    std::size_t unknown = 0;
};

void count(Tally& t, duql::Truth truth) {
    ++t.pairs;
    switch (truth) {
        case duql::Truth::Yes:
            ++t.yes;
            break;
        case duql::Truth::No:
            ++t.no;
            break;
        case duql::Truth::Unknown:
            ++t.unknown;
            break;
    }
}

struct Events {
    dstd::Vector<simdjson::padded_string> texts;
    dstd::Vector<dstd::String> raw;
};

Events all_events() {
    Events out;
    for (const char* e : EVENTS) out.raw.emplace_back(e);
    for (const char* e : MORE_EVENTS) out.raw.emplace_back(e);
    for (const auto& r : out.raw) out.texts.emplace_back(r);
    return out;
}

void check_filter(const duql::Query& q, const Events& events, Tally& tally) {
    simdjson::dom::parser dom_parser;
    simdjson::ondemand::parser od_parser;
    dstd::StringArena strings;
    const auto& root = duql::detail::QueryAccess::root(q);
    for (std::size_t i = 0; i < events.texts.size(); ++i) {
        const auto& text = events.texts[i];
        const duql::Truth want = duql::evaluate_truth(
            root, json::JsonValue(dom_of(dom_parser, text)));
        simdjson::ondemand::document doc;
        REQUIRE(od_parser.iterate(text).get(doc) == simdjson::SUCCESS);
        strings.clear();
        const duql::OnDemandEvent event(
            doc, dstd::StringView(text.data(), text.size()), strings);
        CAPTURE(q.source());
        CAPTURE(events.raw[i]);
        for (int round = 0; round < 2; ++round) {
            const duql::Truth got = duql::evaluate_truth(root, event);
            CHECK(static_cast<int>(got) == static_cast<int>(want));
            doc.rewind();
        }
        count(tally, want);
    }
}

}  // namespace

TEST_CASE("the on-demand evaluator equals the DOM evaluator") {
    const Events events = all_events();
    Tally tally;
    std::size_t filters = 0;

    for (const char* text : FILTERS) {
        auto q = duql::Query::from_string(text);
        if (!q) continue;
        ++filters;
        check_filter(*q, events, tally);
    }
    std::size_t extra = 0;
    for (const char* text : MORE_FILTERS) {
        auto q = duql::Query::from_string(text);
        if (!q) continue;
        ++extra;
        check_filter(*q, events, tally);
    }

    const auto golden = dstd::fs::read_file(DUQL_GOLDEN_PATH);
    REQUIRE(golden.has_value());
    dftu_utils_test::LineStream in(*golden);
    simdjson::dom::parser rows;
    std::size_t golden_filters = 0;
    for (dstd::String line; in.next(line);) {
        if (line.empty()) continue;
        simdjson::dom::element row;
        const simdjson::padded_string padded(line);
        REQUIRE(rows.parse(padded).get(row) == simdjson::SUCCESS);
        if (!row["ok"].get_bool().value()) continue;
        auto q = duql::Query::from_string(
            dstd::String(row["q"].get_string().value()));
        if (!q) continue;
        ++golden_filters;
        check_filter(*q, events, tally);
    }

    MESSAGE("filters ", filters, " extra ", extra, " golden ", golden_filters,
            " pairs ", tally.pairs);
    CHECK(golden_filters > 300);
    CHECK(extra > 30);
    CHECK(tally.yes > 100);
    CHECK(tally.no > 100);
    CHECK(tally.unknown > 100);
}

TEST_CASE("a field read twice in one evaluation gives the same value") {
    auto q = duql::Query::from_string(
        R"(dur > 1000 and dur < 3000 and args.size > 1 and args.size < 9999 and name == "read" and name != "write")");
    REQUIRE(q.has_value());
    const simdjson::padded_string text(
        dstd::String(R"({"name":"read","dur":2000,"args":{"size":4096}})"));
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    REQUIRE(parser.iterate(text).get(doc) == simdjson::SUCCESS);
    dstd::StringArena strings;
    const duql::OnDemandEvent event(
        doc, dstd::StringView(text.data(), text.size()), strings);
    const auto& root = duql::detail::QueryAccess::root(*q);
    for (int i = 0; i < 3; ++i) {
        CHECK(duql::evaluate(root, event));
        doc.rewind();
    }
}

namespace {

// Lines that parse as a document but have a syntax error inside.
constexpr const char* BROKEN_BEFORE[] = {
    R"({"a":1,"b":})",
    R"({"dur":,"name":"read"})",
    R"({name:"read"})",
    R"({"x":[1,2,"name":"read"})",
};

// A bad token that skipping a value does not look at, so it counts only when
// the filter reads field x.
constexpr const char* BROKEN_TOKEN[] = {
    R"({"x":01,"name":"read"})",  R"({"x":tru,"name":"read"})",
    R"({"x":nul,"name":"read"})", R"({"x":.5,"name":"read"})",
    R"({"x":-,"name":"read"})",   R"({"x":1x,"name":"read"})",
    R"({"x":1e,"name":"read"})",  R"({"x":+1,"name":"read"})",
};

constexpr const char* FILTER_SHAPES[] = {
    R"(name == "read")",
    R"(not (name == "read"))",
    R"(name == "x" or name == "read")",
    R"(not exists(zzz))",
    R"(exists(zzz))",
    R"(dur > 1 or not (dur > 1))",
    R"(any(x) == 1)",
    R"((dur ?? 0) >= 0)",
};

}  // namespace

TEST_CASE("a line broken before the field it reads is dropped, never kept") {
    json::Document doc;
    for (const char* line : BROKEN_BEFORE) {
        auto parsed = doc.parse(line);
        if (!parsed) continue;
        for (const char* filter : FILTER_SHAPES) {
            auto q = duql::Query::from_string(filter);
            REQUIRE(q.has_value());
            CAPTURE(line);
            CAPTURE(filter);
            for (int i = 0; i < 3; ++i) CHECK_FALSE(q->matches(*parsed));
        }
        int members = 0;
        parsed->for_each_member(
            [&](dstd::StringView, json::Value) { ++members; });
        CHECK(members == 0);
        CHECK_FALSE(parsed->find("name").has_value());
    }
}

TEST_CASE("a bad token is dropped when the filter reads it and not otherwise") {
    json::Document doc;
    const char* reads_x[] = {"x == 1", "not (x == 1)",
                             R"(x == 1 or name == "read")",
                             R"(not (x == 1) or name == "read")"};
    for (const char* line : BROKEN_TOKEN) {
        for (const char* filter : reads_x) {
            auto parsed = doc.parse(line);
            if (!parsed) continue;
            auto q = duql::Query::from_string(filter);
            REQUIRE(q.has_value());
            CAPTURE(line);
            CAPTURE(filter);
            CHECK_FALSE(q->matches(*parsed));
            CHECK_FALSE(q->matches(*parsed));
        }
        auto parsed = doc.parse(line);
        if (!parsed) continue;
        auto q = duql::Query::from_string(R"(name == "read")");
        REQUIRE(q.has_value());
        CAPTURE(line);
        CHECK(q->matches(*parsed));
    }
}

TEST_CASE("a number too large for a double is an infinity, not a bad token") {
    json::Document doc;
    struct Case {
        const char* line;
        const char* filter;
        bool expected;
    };
    const Case cases[] = {
        {R"({"name":"read","args":{"a":1e400}})", "args.a > 1", true},
        {R"({"name":"read","args":{"a":1e400}})", "a > 1e308", true},
        {R"({"name":"read","args":{"a":1e400}})", "args.a < 1", false},
        {R"({"name":"read","args":{"a":1e400}})", "args.a == 1", false},
        {R"({"name":"read","args":{"a":-1e400}})", "args.a < -1e308", true},
        {R"({"name":"read","args":{"a":-1e400}})", "args.a > 0", false},
        {R"({"name":"read","args":{"a":1.5E+999}})", "args.a >= 1", true},
        {R"({"name":"read","dur":1e400})", "dur > 5", true},
        {R"({"name":"read","dur":1e400})", "not (dur > 5)", false},
        {R"({"name":"read","dur":1e400,"pid":4})", "dur > 5 and pid == 4",
         true},
        {R"({"name":"read","args":{"a":1e400}})",
         R"(lower(name) == "read" and args.a > 1)", true},
        {R"({"name":"read","args":{"a":1e400}})", R"(any(args.l) > 1)", false},
        {R"({"name":"read","args":{"l":[1,1e400]}})", "any(args.l) > 1000",
         true},
    };
    for (const Case& c : cases) {
        auto parsed = doc.parse(c.line);
        REQUIRE(parsed.has_value());
        auto q = duql::Query::from_string(c.filter);
        REQUIRE(q.has_value());
        INFO(dstd::String(c.line) << " | " << dstd::String(c.filter));
        CHECK(q->matches(*parsed) == c.expected);
        CHECK(q->matches(*parsed) == c.expected);
    }
}

TEST_CASE("a string with an invalid escape is its raw text") {
    json::Document doc;
    struct Case {
        const char* line;
        const char* filter;
        bool expected;
    };
    const Case cases[] = {
        {R"({"name":"a\qb","dur":5})", R"(name == "a\\qb")", true},
        {R"({"name":"A\qB","dur":5})", R"(lower(name) == "a\\qb")", true},
        {R"({"name":"a\qb","dur":5})", R"(lower(name) == "a\\qb" and dur > 1)",
         true},
        {R"({"name":"a\qb","dur":5})", R"(name == "aqb")", false},
        {R"({"name":"\ud800","dur":5})", R"(name == "\\ud800")", true},
        {R"({"name":"\udc00","dur":5})", R"(lower(name) == "\\udc00")", true},
        {R"({"name":"\u12","dur":5})", R"(lower(name) == "\\u12")", true},
        {R"({"args":{"p":"x\qy"},"dur":5})", R"(args.p == "x\\qy")", true},
        {R"({"k\q":1,"name":"read","dur":5})",
         R"(lower(name) == "read" and dur > 1)", true},
        {R"({"args":{"k\q":1,"n":2},"name":"read"})",
         R"(lower(name) == "read" and args.n == 2)", true},
        {R"({"name":"read","x":"\q"})", R"(x == 1 or name == "read")", true},
        {R"({"name":"read","x":"\q"})", "x == 1", false},
    };
    for (const Case& c : cases) {
        auto parsed = doc.parse(c.line);
        REQUIRE(parsed.has_value());
        auto q = duql::Query::from_string(c.filter);
        REQUIRE(q.has_value());
        INFO(dstd::String(c.line) << " | " << dstd::String(c.filter));
        CHECK(q->matches(*parsed) == c.expected);
        CHECK(q->matches(*parsed) == c.expected);
    }
}

TEST_CASE("a line broken after the field it reads is kept for that field") {
    json::Document doc;
    const char* line = R"({"name":"read","dur":})";
    auto parsed = doc.parse(line);
    REQUIRE(parsed.has_value());
    auto name_only = duql::Query::from_string(R"(name == "read")");
    auto uses_dur = duql::Query::from_string(R"(name == "read" and dur > 1)");
    auto not_dur = duql::Query::from_string("not (dur > 1)");
    REQUIRE(name_only.has_value());
    REQUIRE(uses_dur.has_value());
    REQUIRE(not_dur.has_value());
    CHECK(name_only->matches(*parsed));
    CHECK(name_only->matches(*parsed));
    CHECK_FALSE(uses_dur->matches(*parsed));
    CHECK_FALSE(not_dur->matches(*parsed));
    CHECK_FALSE(name_only->matches(*parsed));
}

TEST_CASE("a 20-digit integer above uint64 is unknown, like a longer one") {
    json::Document doc;
    for (const char* digits : {"18446744073709551616", "99999999999999999999",
                               "123456789012345678901"}) {
        auto parsed = doc.parse(dstd::String(R"({"big":)") + digits +
                                R"(,"name":"read"})");
        REQUIRE(parsed.has_value());
        auto q = duql::Query::from_string(R"(big > 1 or name == "read")");
        auto only_big = duql::Query::from_string("big > 1");
        auto none = duql::Query::from_string("not (big > 1)");
        REQUIRE(q.has_value());
        CHECK(q->matches(*parsed));
        CHECK_FALSE(only_big->matches(*parsed));
        CHECK_FALSE(none->matches(*parsed));
        CHECK(q->matches(*parsed));
    }
}

TEST_CASE("a bigint is unknown, and does not break the line") {
    json::Document doc;
    auto parsed =
        doc.parse(R"({"big":123456789012345678901234567890,"name":"read"})");
    REQUIRE(parsed.has_value());
    auto q = duql::Query::from_string(R"(big > 1 or name == "read")");
    auto only_big = duql::Query::from_string("big > 1");
    auto none = duql::Query::from_string("not (big > 1)");
    REQUIRE(q.has_value());
    CHECK(q->matches(*parsed));
    CHECK_FALSE(only_big->matches(*parsed));
    CHECK_FALSE(none->matches(*parsed));
    CHECK(q->matches(*parsed));
}

TEST_CASE(
    "a syntax error in a part that is never read does not drop the line") {
    json::Document doc;
    auto parsed = doc.parse(R"({"name":"read","dur":5,"zz":tru,"yy":[1,2})");
    REQUIRE(parsed.has_value());
    auto q = duql::Query::from_string(R"(name == "read" and dur > 1)");
    REQUIRE(q.has_value());
    CHECK(q->matches(*parsed));
    CHECK(q->matches(*parsed));
}

TEST_CASE("a broken line stays broken only until the next parse") {
    json::Document doc;
    auto q = duql::Query::from_string(R"(name == "read")");
    REQUIRE(q.has_value());
    {
        auto bad = doc.parse(R"({"dur":,"name":"read"})");
        REQUIRE(bad.has_value());
        CHECK_FALSE(q->matches(*bad));
    }
    auto good = doc.parse(R"({"name":"read"})");
    REQUIRE(good.has_value());
    CHECK(q->matches(*good));
}

TEST_CASE("mixed reads and queries on malformed lines never crash") {
    // Every call below is a documented use of a value the previous call just
    // returned. A line that Document::parse accepts but cannot be read to the
    // end must stay safe under any order of them.
    constexpr const char* BASE[] = {
        R"({"name":"read","cat":"POSIX","pid":1,"dur":2000,"args":{"size":4096,"tags":["a","b"],"x":{"y":1.5}}})",
        ("{\"ph\":\"X\",\"name\":\"w\\u00e9\",\"ts\":-5,\"dur\":1e2,"
         "\"args\":{\"fname\":\"/a/b\",\"fd\":7,\"n\":null,\"t\":true}}"),
        R"({"tags":[1,2,[3,{"k":"v"}],null,true],"name":"MPI_Send","x":"\"q\"","dur":123456789012345678901})",
    };
    constexpr const char* TOKENS[] = {
        "tru", "nul", "01", "-",   "1e", "\"\\q", "\"\\u12", "}",
        "]",   ",",   ":",  "{",   "[",  "\"",    "+1",      ".5",
        "\\",  "x",   " ",  "0x1", "1.", "fals"};
    constexpr std::size_t TOKEN_COUNT = sizeof(TOKENS) / sizeof(TOKENS[0]);

    dstd::Vector<duql::Query> queries;
    for (const char* f : FILTERS)
        if (auto q = duql::Query::from_string(f))
            queries.push_back(std::move(*q));
    for (const char* f :
         {R"(any(args.tags) == "a")", R"(args.x.y > 1)",
          R"(tags[2].1.k == "v")", R"(x == 1 or not exists(zz))",
          R"(t == true)", R"(any(tags) == 1)"})
        if (auto q = duql::Query::from_string(f))
            queries.push_back(std::move(*q));
    REQUIRE_FALSE(queries.empty());

    std::mt19937_64 rng(777);
    const auto pick = [&](std::size_t n) {
        return static_cast<std::size_t>(rng() % n);
    };
    json::Document doc;
    std::size_t accepted = 0;
    for (int iteration = 0; iteration < 40000; ++iteration) {
        dstd::String line = BASE[pick(3)];
        for (std::size_t edits = 1 + pick(2); edits > 0; --edits) {
            const std::size_t at = pick(line.size() + 1);
            switch (pick(3)) {
                case 0:
                    if (at < line.size()) line.erase(at, 1 + pick(3));
                    break;
                case 1:
                    line.insert(at, TOKENS[pick(TOKEN_COUNT)]);
                    break;
                default:
                    if (at < line.size())
                        line.replace(at, 1 + pick(2),
                                     TOKENS[pick(TOKEN_COUNT)]);
                    break;
            }
        }
        auto parsed = doc.parse(line);
        if (!parsed) continue;
        ++accepted;
        for (int step = 0; step < 8; ++step) {
            const std::size_t op = pick(5);
            const std::size_t query_index = pick(queries.size());
            switch (op) {
                case 0: {
                    const auto& q = queries[query_index];
                    const bool first = q.matches(*parsed);
                    if (!json::detail::ValueAccess::broken(*parsed)) {
                        const bool second = q.matches(*parsed);
                        if (!json::detail::ValueAccess::broken(*parsed))
                            CHECK(first == second);
                    }
                    break;
                }
                case 1: {
                    auto c = parsed->find(pick(2) ? "args" : "name");
                    if (!c) break;
                    (void)c->type();
                    (void)c->raw();
                    if (pick(2)) {
                        (void)c->as_int();
                    } else if (pick(2)) {
                        (void)c->as_string();
                    } else {
                        c->for_each_member(
                            [&](dstd::StringView, json::Value v) {
                                (void)v.as_double();
                                (void)v.as_bool();
                                v.for_each_element(
                                    [&](json::Value e) { (void)e.as_int(); });
                            });
                    }
                    break;
                }
                case 2:
                    parsed->for_each_member([&](dstd::StringView,
                                                json::Value v) {
                        (void)v.type();
                        (void)v.as_int();
                        (void)v.as_string();
                        v.for_each_member([&](dstd::StringView, json::Value w) {
                            (void)w.as_double();
                        });
                    });
                    break;
                case 3:
                    json::detail::ValueAccess::rewind(*parsed);
                    break;
                default:
                    if (auto c = parsed->find("tags"))
                        c->for_each_element([&](json::Value e) {
                            (void)e.as_string();
                            (void)e.as_bool();
                        });
                    break;
            }
        }
        if (json::detail::ValueAccess::broken(*parsed))
            for (const auto& q : queries) CHECK_FALSE(q.matches(*parsed));
    }
    CHECK(accepted > 10000);
}
