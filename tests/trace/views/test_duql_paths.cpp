#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "test_view_common.h"

template <>
struct doctest::StringMaker<std::set<std::string>> {
    static doctest::String convert(const std::set<std::string>& names) {
        std::string out = "{";
        for (const auto& n : names) out += (out.size() > 1 ? "," : "") + n;
        return (out + "}").c_str();
    }
};

namespace {

using Names = std::set<std::string>;
using dataframe::DataFrame;
using dataframe::Series;
namespace reader = dftracer::utils::utilities::reader;

// One event per line, named by its role. `x` sits under args as a number, an
// empty string, a string, null or not at all; `lvl` is a top-level string that
// is present, empty, null or absent. `sh` holds a top-level null `x` over an
// args `x` of 1, which must not fall back to the args value.
constexpr const char* EVENTS[] = {
    R"({"name":"a1","cat":"POSIX","lvl":"hi","args":{"x":1}})",
    R"({"name":"a2","cat":"STDIO","lvl":"","args":{"x":2}})",
    R"({"name":"a3","cat":"STDIO","lvl":null,"args":{"x":3}})",
    R"({"name":"e","cat":"STDIO","args":{"x":""}})",
    R"({"name":"s","cat":"STDIO","args":{"x":"a"}})",
    R"({"name":"n","cat":"STDIO","args":{"x":null}})",
    R"({"name":"m","cat":"STDIO","args":{}})",
    R"({"name":"mp","cat":"POSIX","args":{}})",
    R"({"name":"np","cat":"POSIX","args":{"x":null}})",
    R"({"name":"sh","cat":"STDIO","x":null,"args":{"x":1}})",
};

struct Case {
    const char* filter;
    Names expected;
    // Whether every literal compared with x is a number (true) or a string.
    bool numeric_x;
};

const std::vector<Case>& cases() {
    static const std::vector<Case> c = {
        {"x == 1", {"a1"}, true},
        {"x != 1", {"a2", "a3"}, true},
        {"not (x == 1)", {"a2", "a3"}, true},
        {R"(x == "")", {"e"}, false},
        {R"(x != "")", {"s"}, false},
        {"x in [1, 2]", {"a1", "a2"}, true},
        {"x not in [1, 2]", {"a3"}, true},
        {"not (x in [1])", {"a2", "a3"}, true},
        {R"(x == 1 or cat == "POSIX")", {"a1", "mp", "np"}, true},
        {R"(not (x == 1 and cat == "STDIO"))",
         {"a1", "a2", "a3", "mp", "np"},
         true},
        {R"(lvl == "")", {"a2"}, false},
        {R"(lvl != "")", {"a1"}, false},
        {R"(not (lvl == "hi"))", {"a2"}, false},
    };
    return c;
}

std::string write_trace(TestEnvironment& env) {
    const std::string plain = env.get_dir() + "/paths.pfw";
    {
        std::ofstream out(plain);
        int ts = 1000;
        for (const char* ev : EVENTS) {
            std::string line(ev);
            line.insert(1, R"("ph":"X","pid":1,"tid":1,"ts":)" +
                               std::to_string(ts) + R"(,"dur":10,)");
            out << line << "\n";
            ts += 100;
        }
    }
    const std::string gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip(plain, gz));
    fs::remove(plain);
    dftracer::utils::index::Indexer::open({gz}).build();
    return gz;
}

std::string name_of(std::string_view line) {
    simdjson::dom::parser p;
    simdjson::padded_string s(line);
    std::string_view name;
    REQUIRE(p.parse(s)["name"].get(name) == simdjson::SUCCESS);
    return std::string(name);
}

Names names_of(const DataFrame& df) {
    Names out;
    const auto col = bcol(df, "name");
    REQUIRE(col >= 0);
    for (std::int64_t i = 0; i < df.num_rows(); ++i)
        out.insert(bstr(df, i, "name"));
    return out;
}

View view_of(const std::string& gz, const char* filter) {
    return View::from_file(gz, determine_index_path(gz, "")).duql(filter);
}

coro::CoroTask<Names> reader_lines(
    coro::AsyncGenerator<utilities::fileio::lines::Line> gen) {
    Names out;
    while (auto line = co_await gen.next()) out.insert(name_of(line->content));
    co_return out;
}

coro::CoroTask<Names> reader_json(coro::AsyncGenerator<reader::JsonLine> gen) {
    Names out;
    while (auto opt = co_await gen.next())
        if (auto v = opt->parser->get_string("name")) out.emplace(*v);
    co_return out;
}

// The DataFrame projection of the trace for one filter: a typed column holds
// one type, so x keeps only the values of the filter's literal type and every
// other value becomes null, which gives the same result by the spec rule. `sh`
// has no column form and no filter selects it, so it is left out.
DataFrame frame_of(bool numeric_x) {
    const std::vector<std::string_view> name{"a1", "a2", "a3", "e", "s",
                                             "n",  "m",  "mp", "np"};
    const std::vector<std::string_view> cat{"POSIX", "STDIO", "STDIO",
                                            "STDIO", "STDIO", "STDIO",
                                            "STDIO", "POSIX", "POSIX"};
    DataFrame df;
    df.names = {"name", "cat", "x", "lvl"};
    df.columns.push_back(Series::strings(name));
    df.columns.push_back(Series::strings(cat));
    if (numeric_x) {
        const std::int64_t x[] = {1, 2, 3, 0, 0, 0, 0, 0, 0};
        const std::uint8_t valid[] = {0b00000111, 0};
        df.columns.push_back(Series::flat_i64(x, 9, valid));
    } else {
        const std::vector<std::string_view> x{"", "", "", "", "a",
                                              "", "", "", ""};
        const std::uint8_t valid[] = {0b00011000, 0};
        df.columns.push_back(Series::strings(x, valid));
    }
    const std::vector<std::string_view> lvl{"hi", "", "", "", "",
                                            "",   "", "", ""};
    const std::uint8_t lvl_valid[] = {0b00000011, 0};
    df.columns.push_back(Series::strings(lvl, lvl_valid));
    return df;
}

}  // namespace

TEST_SUITE("duql paths") {
    TEST_CASE("every path selects the records the three-valued rule keeps") {
        TestEnvironment env(10);
        const std::string gz = write_trace(env);
        reader::TraceReader tr({.file_path = gz, .index_dir = env.get_dir()});
        REQUIRE(tr.has_index());

        for (const auto& c : cases()) {
            CAPTURE(std::string(c.filter));

            const auto rows = run(view_of(gz, c.filter).collect());
            CHECK(names_of(rows) == c.expected);

            const auto grouped = run(view_of(gz, c.filter)
                                         .group_by({GroupKey::name()})
                                         .agg({{AggOp::Count, "", "n"}})
                                         .collect());
            CHECK(names_of(grouped) == c.expected);

            const auto counted = run(
                view_of(gz, c.filter).agg({{AggOp::Count, "", "n"}}).collect());
            REQUIRE(counted.num_rows() == 1);
            CHECK(static_cast<std::size_t>(bnum(counted, 0, "n")) ==
                  c.expected.size());

            StringSink sink;
            run(view_of(gz, c.filter).sink_json(sink));
            Names sunk;
            for (const auto& line : sink.lines()) sunk.insert(name_of(line));
            CHECK(sunk == c.expected);

            reader::ReadConfig rc;
            rc.query = c.filter;
            CHECK(run(reader_lines(tr.read_lines(rc))) == c.expected);
            CHECK(run(reader_json(tr.read_json(rc))) == c.expected);

            const DataFrame df = frame_of(c.numeric_x);
            const auto q = duql::Query::from_string(c.filter);
            REQUIRE(q.has_value());
            CHECK(names_of(df.filter(df.mask(*q))) == c.expected);
        }
    }

    TEST_CASE("every path agrees on expression filters") {
        // `tags` is an array, empty, null, missing or a one-element array;
        // `ok` is a bool, null, missing or the number 1; `o` an object or
        // empty; `x` numbers and one string.
        constexpr const char* RECORDS[] = {
            R"({"name":"t1","cat":"C","args":{"x":4,"tags":["a","b"],"ok":true,"o":{"k":1}}})",
            R"({"name":"t2","cat":"C","args":{"x":5,"tags":[],"ok":false,"o":{}}})",
            R"({"name":"t3","cat":"C","args":{"x":6,"tags":null,"ok":null}})",
            R"({"name":"t4","cat":"C","args":{"x":7}})",
            R"({"name":"t5","cat":"C","args":{"x":"7","tags":["b"],"ok":1}})",
        };
        struct ExprCase {
            const char* filter;
            Names expected;
            bool frame;  // Reads only x, so a typed frame can hold it.
        };
        const std::vector<ExprCase> expr_cases = {
            {"x * 2 > 9", {"t2", "t3", "t4"}, true},
            {"x % 2 == 0", {"t1", "t3"}, true},
            {"x between 5 and 6", {"t2", "t3"}, true},
            {"not (x // 2 == 2)", {"t3", "t4"}, true},
            {R"(type(x) == "string")", {"t5"}, false},
            {"len(tags) == 2", {"t1"}, false},
            {"len(tags) == 0", {"t2"}, false},
            {"exists(tags)", {"t1", "t2", "t3", "t5"}, false},
            {"tags is null", {"t3"}, false},
            {"tags is missing", {"t4"}, false},
            {R"(contains(tags, "b"))", {"t1", "t5"}, false},
            {R"(tags[-1] == "b")", {"t1", "t5"}, false},
            {"ok == true", {"t1"}, false},
            {"not ok", {"t2"}, false},
            {"(ok ?? false) == false", {"t2", "t3", "t4"}, false},
            {R"(json(o) == '{"k":1}')", {"t1"}, false},
            {"len(o) == 0", {"t2"}, false},
        };

        TestEnvironment env(10);
        const std::string plain = env.get_dir() + "/exprs.pfw";
        {
            std::ofstream out(plain);
            int ts = 1000;
            for (const char* ev : RECORDS) {
                std::string line(ev);
                line.insert(1, R"("ph":"X","pid":1,"tid":1,"ts":)" +
                                   std::to_string(ts) + R"(,"dur":10,)");
                out << line << "\n";
                ts += 100;
            }
        }
        const std::string gz = plain + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(plain, gz));
        fs::remove(plain);
        dftracer::utils::index::Indexer::open({gz}).build();
        reader::TraceReader tr({.file_path = gz, .index_dir = env.get_dir()});
        REQUIRE(tr.has_index());

        DataFrame frame;
        {
            const std::vector<std::string_view> name{"t1", "t2", "t3", "t4",
                                                     "t5"};
            const std::int64_t x[] = {4, 5, 6, 7, 0};
            const std::uint8_t valid[] = {0b00001111};
            frame.names = {"name", "x"};
            frame.columns.push_back(Series::strings(name));
            frame.columns.push_back(Series::flat_i64(x, 5, valid));
        }

        for (const auto& c : expr_cases) {
            CAPTURE(std::string(c.filter));
            CHECK(names_of(run(view_of(gz, c.filter).collect())) == c.expected);
            CHECK(names_of(run(view_of(gz, c.filter)
                                   .group_by({GroupKey::name()})
                                   .agg({{AggOp::Count, "", "n"}})
                                   .collect())) == c.expected);
            StringSink sink;
            run(view_of(gz, c.filter).sink_json(sink));
            Names sunk;
            for (const auto& line : sink.lines()) sunk.insert(name_of(line));
            CHECK(sunk == c.expected);
            reader::ReadConfig rc;
            rc.query = c.filter;
            CHECK(run(reader_lines(tr.read_lines(rc))) == c.expected);
            CHECK(run(reader_json(tr.read_json(rc))) == c.expected);
            if (c.frame) {
                const auto q = duql::Query::from_string(c.filter);
                REQUIRE(q.has_value());
                CHECK(names_of(frame.filter(frame.mask(*q))) == c.expected);
            }
        }
    }

    TEST_CASE("exists() keeps a file whose only value is an empty array") {
        TestEnvironment env(10);
        const std::string plain = env.get_dir() + "/empty.pfw";
        {
            std::ofstream out(plain);
            out << R"({"ph":"X","pid":1,"tid":1,"ts":1,"dur":1,"name":"e","args":{"tags":[]}})"
                << "\n";
        }
        const std::string gz = plain + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(plain, gz));
        fs::remove(plain);
        dftracer::utils::index::Indexer::open({gz}).build();
        CHECK(names_of(run(view_of(gz, "exists(tags)").collect())) ==
              Names{"e"});
        CHECK(run(view_of(gz, "exists(nothing_here)").collect()).num_rows() ==
              0);
    }

    TEST_CASE("a DataFrame without the column selects as if it were missing") {
        DataFrame df;
        const std::vector<std::string_view> name{"p", "s"};
        const std::vector<std::string_view> cat{"POSIX", "STDIO"};
        df.names = {"name", "cat"};
        df.columns.push_back(Series::strings(name));
        df.columns.push_back(Series::strings(cat));
        auto kept = [&](const char* filter) {
            const auto q = duql::Query::from_string(filter);
            REQUIRE(q.has_value());
            return names_of(df.filter(df.mask(*q)));
        };
        CHECK(kept("x == 1").empty());
        CHECK(kept("not (x == 1)").empty());
        CHECK(kept(R"(x != "")").empty());
        CHECK(kept(R"(x == 1 or cat == "POSIX")") == Names{"p"});
        CHECK(kept(R"(not (x == 1 and cat == "STDIO"))") == Names{"p"});
    }
}
