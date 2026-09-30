#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/duql/abi.h>
#include <dftracer/utils/duql/builder.h>
#include <dftracer/utils/duql/internal/duql_handle.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/duql/syntax/parser.h>
#include <dftracer/utils/duql/syntax/tree.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace dftracer::utils::duql;
using dftracer::utils::json::JsonValue;

namespace {

struct JsonDoc {
    simdjson::dom::parser parser;
    simdjson::dom::element elem;
    bool valid = false;

    explicit JsonDoc(const char* json) {
        auto result = parser.parse(json, std::strlen(json));
        if (!result.error()) {
            elem = result.value_unsafe();
            valid = true;
        }
    }
    JsonValue root() { return valid ? JsonValue(elem) : JsonValue(); }
};

// Owns a dftu_duql and its serialized string across the assertions.
struct AbiDuql {
    dftu_duql* h;
    explicit AbiDuql(dftu_duql* q) : h(q) {}
    ~AbiDuql() { dftu_duql_free(h); }
    std::string str() const {
        char* s = dftu_duql_to_string(h);
        std::string out = s ? s : "";
        dftu_duql_string_free(s);
        return out;
    }
    const Query& duql() const { return duql_handle_unwrap(h); }
};

}  // namespace

TEST_CASE("C++ fluent Field builder matches parse") {
    // (cat == "POSIX") && (dur > 100), Python-style.
    auto built = ((Field("cat") == "POSIX") && (Field("dur") > 100)).build();
    REQUIRE(built.has_value());
    auto parsed = Query::from_string(R"(cat == "POSIX" and dur > 100)");
    REQUIRE(parsed.has_value());
    CHECK(built->to_string() == parsed->to_string());

    JsonDoc hit(R"({"cat":"POSIX","dur":200})");
    JsonDoc miss(R"({"cat":"POSIX","dur":50})");
    CHECK(built->evaluate(hit.root()));
    CHECK_FALSE(built->evaluate(miss.root()));

    CHECK(Field("name").like("%read%").to_string() ==
          Query::from_string(R"(name like "%read%")")->to_string());
    CHECK(Field("pid").in({1, 2, 3}).to_string() ==
          Query::from_string("pid in [1, 2, 3]")->to_string());
}

TEST_CASE("C++ builder compare matches parse") {
    auto built = field_eq("cat", "POSIX").build();
    REQUIRE(built.has_value());
    auto parsed = Query::from_string(R"(cat == "POSIX")");
    REQUIRE(parsed.has_value());
    CHECK(built->to_string() == parsed->to_string());

    JsonDoc match(R"({"cat":"POSIX"})");
    JsonDoc no_match(R"({"cat":"STDIO"})");
    CHECK(built->evaluate(match.root()) == parsed->evaluate(match.root()));
    CHECK(built->evaluate(no_match.root()) ==
          parsed->evaluate(no_match.root()));
    CHECK(built->evaluate(match.root()));
    CHECK_FALSE(built->evaluate(no_match.root()));
}

TEST_CASE("C++ builder numeric typing round-trips") {
    CHECK(field_gt("dur", 50).to_string() == "dur > 50");
    CHECK(field_lt("off", -5).to_string() == "off < -5");
    CHECK(field_eq("ratio", 1.5).to_string() ==
          Query::from_string("ratio == 1.5")->to_string());
    CHECK(field_eq("ok", true).to_string() == "ok == true");
}

TEST_CASE("C++ builder combinators match parse") {
    auto built = (field_eq("cat", "POSIX") && field_gt("dur", 50)).build();
    REQUIRE(built.has_value());
    auto parsed = Query::from_string(R"(cat == "POSIX" and dur > 50)");
    REQUIRE(parsed.has_value());
    CHECK(built->to_string() == parsed->to_string());

    const char* events[] = {R"({"cat":"POSIX","dur":100})",
                            R"({"cat":"POSIX","dur":10})",
                            R"({"cat":"STDIO","dur":100})"};
    for (const char* ev : events) {
        JsonDoc doc(ev);
        CHECK(built->evaluate(doc.root()) == parsed->evaluate(doc.root()));
    }
}

TEST_CASE("C++ builder or / not match parse") {
    auto built =
        (!(field_eq("cat", "STDIO") || field_eq("cat", "MPI"))).build();
    REQUIRE(built.has_value());
    auto parsed = Query::from_string(R"(not (cat == "STDIO" or cat == "MPI"))");
    REQUIRE(parsed.has_value());
    CHECK(built->to_string() == parsed->to_string());
    JsonDoc doc(R"({"cat":"POSIX"})");
    CHECK(built->evaluate(doc.root()) == parsed->evaluate(doc.root()));
    CHECK(built->evaluate(doc.root()));
}

TEST_CASE("C++ builder in / match round-trip") {
    auto in = field_in("cat", std::vector<std::string>{"POSIX", "STDIO"});
    CHECK(in.to_string() ==
          Query::from_string(R"(cat in ["POSIX", "STDIO"])")->to_string());

    auto ints = field_in("pid", std::vector<std::int64_t>{1, 2, 3});
    CHECK(ints.to_string() ==
          Query::from_string("pid in [1, 2, 3]")->to_string());

    auto like = field_match("name", MatchOp::LIKE, "MPI_%").build();
    REQUIRE(like.has_value());
    auto parsed = Query::from_string(R"(name like "MPI_%")");
    REQUIRE(parsed.has_value());
    CHECK(like->to_string() == parsed->to_string());
    JsonDoc a(R"({"name":"MPI_Send"})");
    JsonDoc b(R"({"name":"POSIX_read"})");
    CHECK(like->evaluate(a.root()) == parsed->evaluate(a.root()));
    CHECK(like->evaluate(b.root()) == parsed->evaluate(b.root()));
    CHECK(like->evaluate(a.root()));
    CHECK_FALSE(like->evaluate(b.root()));
}

TEST_CASE("C ABI builder compare matches parse") {
    AbiDuql built(dftu_duql_cmp_str("cat", DFTU_DUQL_CMP_EQ, "POSIX"));
    REQUIRE(built.h != nullptr);
    AbiDuql parsed(dftu_duql_parse(R"(cat == "POSIX")"));
    REQUIRE(parsed.h != nullptr);
    CHECK(built.str() == parsed.str());

    JsonDoc match(R"({"cat":"POSIX"})");
    JsonDoc no_match(R"({"cat":"STDIO"})");
    CHECK(built.duql().evaluate(match.root()));
    CHECK_FALSE(built.duql().evaluate(no_match.root()));
}

TEST_CASE("C ABI builder numeric and combinators") {
    AbiDuql gt(dftu_duql_cmp_i64("dur", DFTU_DUQL_CMP_GT, 50));
    REQUIRE(gt.h != nullptr);
    CHECK(gt.str() == "dur > 50");

    // dftu_duql_and consumes its arguments.
    AbiDuql both(
        dftu_duql_and(dftu_duql_cmp_str("cat", DFTU_DUQL_CMP_EQ, "POSIX"),
                      dftu_duql_cmp_i64("dur", DFTU_DUQL_CMP_GT, 50)));
    REQUIRE(both.h != nullptr);
    AbiDuql parsed(dftu_duql_parse(R"(cat == "POSIX" and dur > 50)"));
    REQUIRE(parsed.h != nullptr);
    CHECK(both.str() == parsed.str());

    JsonDoc doc(R"({"cat":"POSIX","dur":100})");
    JsonDoc doc2(R"({"cat":"POSIX","dur":10})");
    CHECK(both.duql().evaluate(doc.root()) ==
          parsed.duql().evaluate(doc.root()));
    CHECK(both.duql().evaluate(doc2.root()) ==
          parsed.duql().evaluate(doc2.root()));
    CHECK(both.duql().evaluate(doc.root()));
    CHECK_FALSE(both.duql().evaluate(doc2.root()));
}

TEST_CASE("C ABI builder in / not_in / match") {
    const char* cats[] = {"POSIX", "STDIO"};
    AbiDuql in(dftu_duql_in_str("cat", cats, 2));
    REQUIRE(in.h != nullptr);
    AbiDuql in_parsed(dftu_duql_parse(R"(cat in ["POSIX", "STDIO"])"));
    REQUIRE(in_parsed.h != nullptr);
    CHECK(in.str() == in_parsed.str());

    int64_t pids[] = {1, 2, 3};
    AbiDuql nin(dftu_duql_not_in_i64("pid", pids, 3));
    REQUIRE(nin.h != nullptr);
    AbiDuql nin_parsed(dftu_duql_parse("pid not in [1, 2, 3]"));
    REQUIRE(nin_parsed.h != nullptr);
    CHECK(nin.str() == nin_parsed.str());

    AbiDuql match(dftu_duql_match("name", DFTU_DUQL_MATCH_LIKE, "MPI_%"));
    REQUIRE(match.h != nullptr);
    AbiDuql match_parsed(dftu_duql_parse(R"(name like "MPI_%")"));
    REQUIRE(match_parsed.h != nullptr);
    CHECK(match.str() == match_parsed.str());
    JsonDoc a(R"({"name":"MPI_Send"})");
    CHECK(match.duql().evaluate(a.root()) ==
          match_parsed.duql().evaluate(a.root()));
    CHECK(match.duql().evaluate(a.root()));
}

TEST_CASE("C ABI builder or / not") {
    AbiDuql q(dftu_duql_not(
        dftu_duql_or(dftu_duql_cmp_str("cat", DFTU_DUQL_CMP_EQ, "STDIO"),
                     dftu_duql_cmp_str("cat", DFTU_DUQL_CMP_EQ, "MPI"))));
    REQUIRE(q.h != nullptr);
    AbiDuql parsed(dftu_duql_parse(R"(not (cat == "STDIO" or cat == "MPI"))"));
    REQUIRE(parsed.h != nullptr);
    CHECK(q.str() == parsed.str());
    JsonDoc doc(R"({"cat":"POSIX"})");
    CHECK(q.duql().evaluate(doc.root()));
}

TEST_CASE("C ABI builder null field is safe") {
    CHECK(dftu_duql_cmp_i64(nullptr, DFTU_DUQL_CMP_EQ, 1) == nullptr);
    CHECK(dftu_duql_and(nullptr, nullptr) == nullptr);
}

TEST_CASE("any() builds the same leaf from C++, the C ABI and the DSL") {
    auto built = (Field("tags").any() == "a").build();
    REQUIRE(built.has_value());
    CHECK(built->to_string() == R"(any(tags) == "a")");
    AbiDuql c(dftu_duql_cmp_str("any(tags)", DFTU_DUQL_CMP_EQ, "a"));
    REQUIRE(c.h != nullptr);
    CHECK(c.str() == R"(any(tags) == "a")");
    AbiDuql in(dftu_duql_cmp_i64("any(sizes)", DFTU_DUQL_CMP_GT, 100));
    REQUIRE(in.h != nullptr);
    CHECK(in.str() == "any(sizes) > 100");

    JsonDoc hit(R"({"tags":["b","a"],"sizes":[1,200]})");
    JsonDoc miss(R"({"tags":["b"],"sizes":[1]})");
    CHECK(c.duql().evaluate(hit.root()));
    CHECK_FALSE(c.duql().evaluate(miss.root()));
    CHECK(in.duql().evaluate(hit.root()));
    CHECK_FALSE(in.duql().evaluate(miss.root()));
}

namespace {

void check_same_tree(const Pipe& built, const std::string& text) {
    CAPTURE(text);
    CAPTURE(built.raw());
    auto want = syntax::parse(text);
    REQUIRE(want.has_value());
    auto got = syntax::parse(built.raw());
    REQUIRE_MESSAGE(got.has_value(), got.error().format());
    CHECK(syntax::equal(*got, *want));
    CHECK(built.text() == syntax::to_text(*want));
}

}  // namespace

TEST_CASE("pipe builder builds the text tree of every stage and expression") {
    const std::vector<std::pair<Pipe, std::string>> cases = {
        {source("t.pfw.gz")
             .where(c("name") == "read" && c("dur") > 1000)
             .group({"pid"}, {{"n", fn("count")}})
             .sort({-c("n")})
             .take(10),
         R"(from "t.pfw.gz" | where name == "read" and dur > 1000 | group pid { n = count() } | sort -n | take 10)"},
        {Pipe().where(c("a") == nullptr || c("b") == true || c("c") == -5 ||
                      c("d") == 1.5 || c("e") == "x\"y"),
         R"(where a == null or b == true or c == -5 or d == 1.5 or e == 'x"y')"},
        {Pipe().where(c("ts").between(duration(1, "s"), duration(5, "s")) &&
                      c("dur") > duration(250, "ms") &&
                      c("name") == param("n")),
         "where ts between 1s and 5s and dur > 250ms and name == $n"},
        {Pipe().where(lit(5) - 3 == 2 && -c("x") < duration(10, "us") &&
                      idiv(c("a"), 2) % 3 == lit(1.5) * 2 / 4 &&
                      c("a").coalesce(c("b")) + 1 > duration(2.5, "ns")),
         "where 5 - 3 == 2 and -x < 10us and a // 2 % 3 == 1.5 * 2 / 4 and "
         "(a ?? b) + 1 > 2.5ns"},
        {Pipe().where(c("a").iregex("x") &&
                      (c("a").not_regex("^x") || c("b").regex("y")) &&
                      c("c").not_iregex(c("p"))),
         R"(where a ~* "x" and (a !~ "^x" or b ~ "y") and c !~* p)"},
        {Pipe().where(c("x").is_in({1, 2, 3}) && c("y").not_in({"a", "b"})),
         R"(where x in [1, 2, 3] and y not in ["a", "b"])"},
        {Pipe().where(c("cat").icontains("io") &&
                      c("tags").not_icontains("x", true)),
         R"(where "io" in cat and "x" not in any(tags))"},
        {Pipe().where(c("name").like("%Send%") &&
                      c("name").not_ilike("%a_b%", "\\") && c("n").ilike("x") &&
                      c("n").not_like("y")),
         R"(where name like "%Send%" and name not ilike "%a_b%" escape "\\" and n ilike "x" and n not like "y")"},
        {Pipe().where(c("x").is_null() || c("y").is_not_missing() ||
                      c("z").is_not_null() || c("w").is_missing() ||
                      !(c("v") == 1)),
         "where x is null or y is not missing or z is not null or w is "
         "missing or not v == 1"},
        {Pipe().where(c("ts").not_between(1, 2) &&
                      fn("any", {c("tags")}) == "y"),
         R"(where ts not between 1 and 2 and any(tags) == "y")"},
        {Pipe().where(c("run").ref("runs", "app") == "laghos" &&
                      c("k").ref("files", "path", "fhash") == "a"),
         R"(where run -> runs.app == "laghos" and k -> files(fhash).path == "a")"},
        {Pipe().derive({{"e", fn("myplug.entropy", {c("x")}, {{"bins", 8}})},
                        {"q", fn("quantile", {c("dur"), 0.5})},
                        {"l", list({1, 2})}}),
         "derive e = myplug.entropy(x, bins = 8), q = quantile(dur, 0.5), "
         "l = [1, 2]"},
        {Pipe().where(c("xs")[c("i")] == c("xs")[2] && c("xs")[-1] > 0 &&
                      c("xs")[param("n")] > 0 && c("a.b")[c("i") - 1] > 0),
         "where xs[i] == xs[2] and xs[-1] > 0 and xs[$n] > 0 and "
         "a.b[i - 1] > 0"},
        {Pipe().derive(
             {{"l", list({1, "a", list({2})})}, {"s", fn("sort", {c("xs")})}}),
         R"(derive l = [1, "a", [2]], s = sort(xs))"},
        {Pipe().derive({{"s", case_({{c("dur") > 500, "slow"}}, Col("fast"))},
                        {"t", case_({{c("ok"), 1}, {c("bad"), 2}})}}),
         R"(derive s = case { dur > 500 => "slow", else => "fast" }, t = case { ok => 1, bad => 2 })"},
        {Pipe().where(tup({c("a"), c("b")}).is_in({tup({1, 2}), tup({3, 4})})),
         "where (a, b) in [(1, 2), (3, 4)]"},
        {Pipe()
             .where(c("fd").is_in(
                 rowset("data").where(c("name") == "open").select({"fd"})))
             .derive({{"n", sub(rowset("data")
                                    .where(c("run") == c("^.run"))
                                    .agg({{"c", fn("count")}}))}}),
         R"(where fd in (from data | where name == "open" | select fd) | derive n = (from data | where run == ^.run | agg { c = count() }))"},
        {Pipe().where(c("fd").not_in(rowset("data").select({"fd"}))),
         "where fd not in (from data | select fd)"},
        {Pipe()
             .select({"name", Item("d", c("dur") * 2), "ts"})
             .drop({"args.x", "cat"})
             .rename({{"n", "name"}, {"p", "args.path"}}),
         "select name, d = dur * 2, ts | drop args.x, cat | rename n = name, "
         "p = args.path"},
        {Pipe().distinct().distinct({c("pid"), c("tid")}),
         "distinct | distinct pid, tid"},
        {Pipe()
             .group({}, {{"n", fn("count")}})
             .agg({{"n", fn("count")}, {"s", fn("sum", {c("dur")})}}),
         "group { n = count() } | agg { n = count(), s = sum(dur) }"},
        {Pipe()
             .window({"pid", "name"}, {"ts"},
                     {{"n", fn("row_number")},
                      {"gap", c("ts") - fn("lag", {c("ts")})}})
             .window({}, {"ts"}, {{"r", fn("rank")}}),
         "window pid, name sort ts { n = row_number(), gap = ts - lag(ts) } "
         "| window sort ts { r = rank() }"},
        {Pipe().window({"pid"}, {"ts"},
                       {{"a", fn("sum", {c("dur")}).over_rows(3)},
                        {"b", fn("mean", {c("dur")}).over(duration(1, "s"))},
                        {"d", fn("max", {c("dur")}).over(param("w"))},
                        {"e", fn("count").over(5)}}),
         "window pid sort ts { a = sum(dur) over 3 rows, b = mean(dur) over "
         "1s, d = max(dur) over $w, e = count() over 5 }"},
        {Pipe()
             .pivot(c("name"), {"a", "b"}, {{"s", fn("sum", {c("dur")})}})
             .pivot(c("name"), {}, {{"s", fn("sum", {c("dur")})}}),
         R"(pivot name in ["a", "b"] { s = sum(dur) } | pivot name { s = sum(dur) })"},
        {Pipe().unpivot({"a", "b"}, "k", "v"), "unpivot a, b as k, v"},
        {Pipe()
             .distinct({Item("c", c("cat")), Item("d", idiv(c("dur"), 10))})
             .pivot(c("k"), {"a", "b"}, {{"n", fn("count")}}, {"x", ""})
             .take_range(3, param("b")),
         R"(distinct c = cat, d = dur // 10 | pivot k in ["a" as x, "b"] { n = count() } | take 3..$b)"},
        {Pipe().sort({SortKey(-c("dur"), Nulls::LAST),
                      SortKey(c("name"), Nulls::FIRST)}),
         "sort -dur nulls last, name nulls first"},
        {Pipe().take(3, {c("pid")}, {-c("dur")}).take(param("k")),
         "take 3 by pid sort -dur | take $k"},
        {Pipe().skip(5).sample(10, true, 7).sample(100),
         "skip 5 | sample 10% seed 7 | sample 100"},
        {Pipe().expand("tags", "t", "i", true).expand("xs"),
         "expand tags as t with_index i keep_empty | expand xs"},
        {Pipe()
             .parse(c("name"), "(?<op>\\w+)")
             .parse(c("args.fname"), "a\"b\\d")
             .parse(c("p"), param("re"))
             .derive({{"r", fn("regex_replace", {c("name"), "/+", "/"})}}),
         R"x(parse name ~ "(?<op>\\w+)" | parse args.fname ~ "a\"b\\d" | parse p ~ $re | derive r = regex_replace(name, "/+", "/"))x"},
        {Pipe()
             .lookup("files", {"fhash"}, "f")
             .lookup("files", {JoinKey(c("fhash"), c("fh")), "pid"}),
         "lookup files on fhash into f | lookup files on fhash == fh, pid"},
        {Pipe()
             .lookup("files", {"fhash"}, {}, LookupHow::INNER)
             .lookup("files", {"fhash"}, {}, LookupHow::ANTI)
             .lookup("files", {"fhash"}, "f", LookupHow::INNER),
         "lookup files on fhash inner | lookup files on fhash anti | lookup "
         "files on fhash inner into f"},
        {Pipe().lookup(source("f.pfw.gz").where(c("x") == 1), {"pid"}, "m",
                       LookupHow::INNER),
         R"(lookup (from "f.pfw.gz" | where x == 1) on pid inner into m)"},
        {Pipe().lookup(rowset("files").take(3), {"pid"}, {}, LookupHow::ANTI),
         "lookup (from files | take 3) on pid anti"},
        {Pipe().lookup_asof(rowset("s"), {"pid"}, "ts"),
         "lookup (from s) on pid asof ts"},
        {Pipe()
             .lookup_asof("samples", {"pid"}, JoinKey(c("ts"), c("t")),
                          Asof::NEAREST, duration(5, "ms"))
             .lookup_asof("samples", {"pid"}, "ts", Asof::FORWARD),
         "lookup samples on pid asof ts == t nearest within 5ms | lookup "
         "samples on pid asof ts forward"},
        {Pipe().lookup_overlap("spans", {"pid"}, "m"),
         "lookup spans on pid overlap into m"},
        {Pipe().union_with(source("b.pfw.gz").where(c("x") == 1)),
         R"(union (from "b.pfw.gz" | where x == 1))"},
        {Pipe().call(fn("myplug.table", {c("x")}, {{"n", 3}})),
         "call myplug.table(x, n = 3)"},
        {Pipe()
             .time_range(duration(1, "s"), duration(5, "s"), true)
             .call_tree()
             .bucket(duration(1, "s"), true)
             .bucket(duration(2, "s")),
         "time_range 1s .. 5s overlap | call_tree | bucket 1s fill | bucket "
         "2s"},
        {Pipe()
             .bucket(duration(1, "ms"), true, {}, "forward")
             .bucket(duration(1, "ms"), true, "b", "linear", lit(0),
                     duration(6, "ms"))
             .bucket(duration(1, "ms"), true, {}, "zero", param("lo"),
                     param("hi") + duration(1, "ms")),
         "bucket 1ms fill forward | bucket 1ms fill linear from 0 to 6ms as b "
         "| bucket 1ms fill from $lo to $hi + 1ms"},
        {Pipe()
             .bucket(duration(5, "s"), false, {}, "zero", std::nullopt,
                     std::nullopt, duration(1, "s"))
             .bucket(duration(5, "s"), false, {}, "zero", std::nullopt,
                     std::nullopt, duration(1, "s"), duration(500, "ms"))
             .bucket(duration(5, "s"), true, "b", "forward", lit(0),
                     duration(6, "s"), duration(1, "s"), duration(500, "ms"))
             .bucket(duration(5, "s"), false, {}, "zero", std::nullopt,
                     std::nullopt, std::nullopt, duration(500, "ms"))
             .bucket(param("w"), false, {}, "zero", std::nullopt, std::nullopt,
                     param("e"), param("a")),
         "bucket 5s every 1s | bucket 5s every 1s at 500ms | bucket 5s every "
         "1s at 500ms fill forward from 0 to 6s as b | bucket 5s at 500ms | "
         "bucket $w every $e at $a"},
        {Pipe()
             .time_range(param("t0"), param("t0") + duration(10, "s"))
             .time_range(duration(1, "s"), std::nullopt)
             .time_range(std::nullopt, duration(5, "s"), true)
             .bucket(duration(1, "s"), true, "sec")
             .sample(param("n"), false, param("s"))
             .union_with("runs"),
         "time_range $t0 .. $t0 + 10s | time_range 1s .. | time_range .. 5s "
         "overlap | bucket 1s fill as sec | sample $n seed $s | union runs"},
        {Pipe().where(c("name").is_in(param("names")) &&
                      c("name").not_in(param("skip")) &&
                      c("path").like(param("p")) &&
                      c("path").not_ilike(param("q"), "!")),
         R"(where name in $names and name not in $skip and path like $p and path not ilike $q escape "!")"},
        {Pipe()
             .session({"pid"}, duration(5, "ms"), duration(1, "s"), "s")
             .session({}, duration(1, "ms")),
         "session pid gap 5ms max 1s as s | session gap 1ms"},
        {rowset("big")
             .let("big", rowset("data").where(c("dur") > 100))
             .define("slow", {"x"}, c("x") > 5)
             .where(fn("slow", {c("dur")})),
         "let big = from data | where dur > 100;\ndef slow(x) = x > 5;\nfrom "
         "big | where slow(dur)"},
        {rowset("data")
             .define("io_rate", {"d"},
                     Pipe()
                         .where(c("cat") == "POSIX")
                         .bucket(c("d"))
                         .agg({{"b", fn("sum", {c("size")})}}))
             .define("pos", {}, Pipe().where(c("size") > 0))
             .use("io_rate", {duration(1, "ms")})
             .use("pos")
             .sort({c("b")}),
         "def io_rate(d) = where cat == \"POSIX\" | bucket d | agg { b = "
         "sum(size) };\ndef pos = where size > 0;\nfrom data | io_rate(1ms) | "
         "pos() | sort b"},
        {source_param("f").where(c("x") == 1), "from $f | where x == 1"},
        {sources({"a.pfw.gz", "b.pfw.gz"}), R"(from "a.pfw.gz", "b.pfw.gz")"},
        {rowset("all").where(Field("cat") == "POSIX"),
         R"(from all | where cat == "POSIX")"},
    };
    for (const auto& [built, text] : cases) check_same_tree(built, text);
}

TEST_CASE("an index follows a field path") {
    CHECK_THROWS_AS(fn("sort", {c("xs")})[0], std::invalid_argument);
    CHECK_THROWS_AS((c("a") + 1)[0], std::invalid_argument);
    CHECK_THROWS_AS(Col(5)[0], std::invalid_argument);
}

TEST_CASE("pipe bucket rejects a range without fill and a bad mode") {
    CHECK_THROWS_AS(Pipe().bucket(duration(1, "s"), false, {}, "forward"),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        Pipe().bucket(duration(1, "s"), false, {}, "zero", lit(0), lit(1)),
        std::invalid_argument);
    CHECK_THROWS_AS(Pipe().bucket(duration(1, "s"), true, {}, "cubic"),
                    std::invalid_argument);
    CHECK_THROWS_AS(Pipe().bucket(duration(1, "s"), true, {}, "zero", lit(0)),
                    std::invalid_argument);
}

TEST_CASE("pipe builder writes any string as a literal") {
    CHECK(lit("a\"b'c").raw() == R"("a\"b'c")");
    CHECK(lit("ends\\").raw() == R"("ends\\")");
    CHECK(lit("a\nb").raw() == R"("a\nb")");
    CHECK_THROWS_AS(lit(std::nan("")), std::invalid_argument);
    check_same_tree(Pipe().where(c("x") == "q\" s' b\\"),
                    R"(where x == 'q" s\' b\\')");
}

TEST_CASE("pipe builder binds parameters and reports parse errors") {
    Pipe p = Pipe().where(c("name") == param("n")).bind("n", std::string("x"));
    REQUIRE(p.bound().size() == 1);
    CHECK(p.bound()[0].first == "n");
    auto scalar = [](const ParamValue& v) { return std::get<LiteralValue>(v); };
    CHECK(std::get<std::string>(scalar(p.bound()[0].second)) == "x");
    p = p.bind("n", LiteralValue{std::int64_t{3}});
    REQUIRE(p.bound().size() == 1);
    CHECK(std::get<std::int64_t>(scalar(p.bound()[0].second)) == 3);
    p = p.bind("n",
               std::vector<LiteralValue>{std::string("a"), std::string("b")});
    CHECK(std::get<std::vector<LiteralValue>>(p.bound()[0].second).size() == 2);
    CHECK_THROWS_AS((void)Pipe().where(c("a..b") == 1).text(),
                    std::invalid_argument);
}

TEST_CASE("source builder writes the members in order") {
    const std::string text = Source()
                                 .rowset("data", Pipe().where(c("ph") != "M"))
                                 .flag("args_fallback", true)
                                 .text();
    CHECK(text == "data = where ph != \"M\";\ndef args_fallback = true");
    CHECK(Source().flag("x", false).text() == "def x = false");
}

TEST_CASE("source builder writes macros and compiles") {
    const std::string text =
        Source()
            .define("slow", {"t"}, c("dur") > param("t"))
            .define("fast", {"t"}, Pipe().where(c("dur") < param("t")))
            .rowset("files", Pipe()
                                 .where(c("name") == "FH")
                                 .select({{"fhash", c("args.value")}, "pid"})
                                 .distinct())
            .text();
    CHECK(text ==
          "files = where name == \"FH\" | select fhash = args.value, pid | "
          "distinct;\n"
          "def slow(t) = dur > $t;\n"
          "def fast(t) = where dur < $t");
    auto p = compile_program("where true", {}, nullptr, text);
    REQUIRE_MESSAGE(p.has_value(), (p ? "" : p.error().format()));
}

TEST_CASE("source builder refuses a row set that reads a file") {
    CHECK_THROWS_AS(Source().rowset("x", rowset("runs")),
                    std::invalid_argument);
    CHECK_THROWS_AS(Source().rowset("x", source("a.pfw")),
                    std::invalid_argument);
}
