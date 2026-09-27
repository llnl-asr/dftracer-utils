#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/extensions/rowset_fold.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/source.h>
#include <dftracer/utils/trace/views/fold.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <fstream>
#include <string>
#include <vector>

#include "test_view_common.h"

using namespace dftracer::utils::trace::views::detail;
using namespace dftracer::utils::index::extensions;
namespace ix = dftracer::utils::index;
namespace df = dftracer::utils::dataframe;
using dftracer::utils::StringIntern;

namespace {

std::string padded(std::string s) {
    s.resize(s.size() + simdjson::SIMDJSON_PADDING, '\0');
    return s;
}

std::vector<FoldEvent> fold_events_of(const std::vector<std::string>& lines,
                                      StringIntern& intern) {
    std::vector<FoldEvent> out;
    simdjson::dom::parser parser;
    for (const auto& l : lines) {
        std::string buf = padded(l);
        auto doc = parser.parse(buf.data(), l.size(), false);
        REQUIRE_FALSE(doc.error());
        out.push_back(extract_fold_event(doc.value_unsafe(), intern, true));
    }
    return out;
}

// Every kind of dftracer metadata, with `ph` written as a letter and as a
// number, between data events.
std::vector<std::string> trace_lines() {
    return {
        R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"/data/a.bin","value":"fh1"}})",
        R"({"name":"HH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"node0","value":"hh1"}})",
        R"({"name":"SH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"s","value":"sh1"}})",
        R"({"name":"PR","cat":"dftracer","pid":7,"tid":1,"ph":"M","args":{"name":"rank","value":"3"}})",
        R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1,"dur":2,"args":{"fhash":"fh1"}})",
        R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":4,"args":{"name":"/data/b.bin","value":"fh2"}})",
        R"({"ph":"X","name":"write","cat":"POSIX","pid":1,"tid":1,"ts":5,"dur":2,"args":{"fhash":"fh2"}})",
    };
}

std::string write_trace(TestEnvironment& env,
                        const std::vector<std::string>& lines) {
    const std::string pfw = env.get_dir() + "/rows.pfw";
    {
        std::ofstream o(pfw);
        for (const auto& l : lines) o << l << "\n";
    }
    const std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    ix::Indexer::open({gz}).build();
    return gz;
}

// Row `r` of `f` as text, column by column.
std::string row_text(const df::DataFrame& f, std::int64_t r) {
    std::string out;
    for (std::size_t c = 0; c < f.columns.size(); ++c) {
        const df::Series col = f.columns[c].materialize();
        out += f.names[c] + "=";
        if (col.is_null(r))
            out += "null";
        else if (col.type() == df::TypeId::String)
            out += col.string_at(r);
        else if (col.type() == df::TypeId::Int64)
            out += std::to_string(col.data<std::int64_t>()[r]);
        else if (col.type() == df::TypeId::Uint64)
            out += std::to_string(col.data<std::uint64_t>()[r]);
        else
            out += "?";
        out += ";";
    }
    return out;
}

std::vector<std::string> rows_of(const df::DataFrame& f) {
    std::vector<std::string> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        out.push_back(row_text(f, r));
    return out;
}

}  // namespace

TEST_SUITE("RowSetFold") {
    TEST_CASE("the built-in sources index their lookup row sets") {
        std::vector<std::string> names;
        for (const auto& r : ix::indexed_rowsets(ix::get_schema("dftracer")))
            names.push_back(r.name);
        CHECK(names ==
              std::vector<std::string>{"files", "hosts", "strings", "ranks"});
        names.clear();
        for (const auto& r : ix::indexed_rowsets(ix::get_schema("genesis")))
            names.push_back(r.name);
        CHECK(names.back() == "runs");
        CHECK(ix::indexed_rowsets(ix::get_schema("generic")).empty());
    }

    TEST_CASE("a row set that aggregates or reads another is not indexed") {
        const auto& s = ix::register_schema(
            "id: rs_mixed\nsource: |\n"
            "  big = where x > 1 | select x, y;\n"
            "  counts = where x > 1 | group y { n = count() };\n"
            "  keyed = where y in (from big | select y)\n",
            "test");
        std::vector<std::string> names;
        for (const auto& r : ix::indexed_rowsets(s)) names.push_back(r.name);
        CHECK(names == std::vector<std::string>{"big"});
    }

    TEST_CASE("each stored row set equals the pipeline over every record") {
        TestEnvironment env(10);
        const std::string gz = write_trace(env, trace_lines());
        const auto ixr = ix::Indexer::open({gz});
        const View v = View::from_file(gz, determine_index_path(gz, ""));
        for (const char* name : {"files", "hosts", "strings", "ranks"}) {
            CAPTURE(name);
            const df::DataFrame stored = ixr.rowset(name);
            const df::DataFrame scanned =
                run(v.duql(std::string("from ") + name + " | where true")
                        .collect());
            CHECK(rows_of(stored) == rows_of(scanned));
        }
        CHECK(rows_of(ixr.rowset("files")) ==
              std::vector<std::string>{"fhash=fh1;path=/data/a.bin;",
                                       "fhash=fh2;path=/data/b.bin;"});
        CHECK(rows_of(ixr.rowset("ranks")) ==
              std::vector<std::string>{"pid=7;rank=3;"});
    }

    TEST_CASE("the stored genesis runs equal the pipeline over every record") {
        TestEnvironment env(10);
        std::vector<std::string> lines = {
            R"({"id":0,"name":"RUN","cat":"genesis","pid":0,"tid":0,"ph":4,"args":{"run":"ab","app":"laghos","nodes":4}})",
            R"({"id":0,"name":"RUN","cat":"genesis","pid":0,"tid":0,"ph":4,"args":{"run":"cd","app":"lulesh","nodes":8}})"};
        for (int i = 0; i < 30; ++i)
            lines.push_back(
                R"({"id":1,"name":"f","cat":"c","pid":0,"tid":0,"ph":3,"ts":1,"args":{"run":"ab","path":"main;f","depth":1,"count":2}})");
        const std::string gz = write_trace(env, lines);
        const auto ixr = ix::Indexer::open({gz});
        const View v = View::from_file(gz, determine_index_path(gz, ""));
        const df::DataFrame stored = ixr.rowset("runs");
        CHECK(rows_of(stored) ==
              rows_of(run(v.duql("from runs | where true").collect())));
        REQUIRE(stored.num_rows() == 2);
        CHECK(row_text(stored, 1).find("app=lulesh;") != std::string::npos);
    }

    TEST_CASE("a distinct row set keeps each row once across units") {
        StringIntern intern;
        auto lines = trace_lines();
        lines.push_back(lines[0]);
        auto events = fold_events_of(lines, intern);
        RowSetFold fold(intern,
                        ix::indexed_rowsets(ix::get_schema("dftracer")));
        ScanUnit unit;
        unit.file_path = "x";
        for (int i = 0; i < 2; ++i) {
            fold.step(FoldBatch{std::span<const FoldEvent>(events), unit});
            fold.seal_unit(unit);
        }
        CHECK(fold.entry_count() == 5);
    }

    TEST_CASE("a fold over budget keeps no rows") {
        StringIntern intern;
        auto events = fold_events_of(trace_lines(), intern);
        RowSetFold fold(intern, ix::indexed_rowsets(ix::get_schema("dftracer")),
                        16);
        ScanUnit unit;
        unit.file_path = "x";
        fold.step(FoldBatch{std::span<const FoldEvent>(events), unit});
        fold.seal_unit(unit);
        CHECK(fold.abandoned());
        CHECK(fold.entry_count() == 0);
    }

    TEST_CASE("a dropped unit leaves no rows") {
        StringIntern intern;
        auto events = fold_events_of(trace_lines(), intern);
        RowSetFold fold(intern,
                        ix::indexed_rowsets(ix::get_schema("dftracer")));
        ScanUnit unit;
        unit.file_path = "x";
        fold.step(FoldBatch{std::span<const FoldEvent>(events), unit});
        fold.drop_unit(unit);
        CHECK(fold.entry_count() == 0);
        fold.step(FoldBatch{std::span<const FoldEvent>(events), unit});
        fold.seal_unit(unit);
        CHECK(fold.entry_count() == 5);
    }
}
