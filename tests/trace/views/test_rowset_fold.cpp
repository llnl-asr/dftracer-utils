// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/extensions/rowset_fold.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/index/record_schema.h>
#include <dftracer/utils/trace/index/source.h>
#include <dftracer/utils/trace/views/fold.h>
#include <doctest/doctest.h>
#include <simdjson.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <fstream>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

using namespace dftracer::utils::trace::views::detail;
using namespace dftracer::utils::trace::index::extensions;

namespace {

FoldEvent extract_fold_event(simdjson::dom::element root,
                             dstd::StringIntern& intern, bool needs_args,
                             const dstd::Vector<dstd::String>* extra = nullptr,
                             bool capture_schema = false) {
    FoldEvent ev;
    extract_fold_event_into(ev, root, intern, needs_args, extra,
                            capture_schema);
    return ev;
}

dstd::String padded(dstd::String s) {
    s.resize(s.size() + simdjson::SIMDJSON_PADDING, '\0');
    return s;
}

dstd::Vector<FoldEvent> fold_events_of(const dstd::Vector<dstd::String>& lines,
                                       dstd::StringIntern& intern) {
    dstd::Vector<FoldEvent> out;
    simdjson::dom::parser parser;
    for (const auto& l : lines) {
        dstd::String buf = padded(l);
        auto doc = parser.parse(buf.data(), l.size(), false);
        REQUIRE_FALSE(doc.error());
        out.push_back(extract_fold_event(doc.value_unsafe(), intern, true));
    }
    return out;
}

// Every kind of dftracer metadata, with `ph` written as a letter and as a
// number, between data events.
dstd::Vector<dstd::String> trace_lines() {
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

dstd::String write_trace(TestEnvironment& env,
                         const dstd::Vector<dstd::String>& lines,
                         bool build = true) {
    const dstd::String pfw = env.dir() + "/rows.pfw";
    {
        dftu_utils_test::FileOut o(pfw);
        for (const auto& l : lines) o << l << "\n";
    }
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    if (build)
        dftu_utils_test::ok(
            dftu_utils_test::ok(trace::index::Indexer::open(
                                    {dstd::String(dstd::StringView(gz))}))
                .build());
    return gz;
}

// Row `r` of `f` as text, column by column.
dstd::String row_text(const dataframe::DataFrame& f, std::int64_t r) {
    dstd::String out;
    for (std::size_t c = 0; c < f.columns.size(); ++c) {
        const dataframe::Series col = f.columns[c].materialize();
        out += f.names[c] + "=";
        if (col.is_null(r))
            out += "null";
        else if (col.type() == dataframe::TypeId::String)
            out += col.string_at(r);
        else if (col.type() == dataframe::TypeId::Int64)
            out += dstd::to_string(col.data<std::int64_t>()[r]);
        else if (col.type() == dataframe::TypeId::Uint64)
            out += dstd::to_string(col.data<std::uint64_t>()[r]);
        else
            out += "?";
        out += ";";
    }
    return out;
}

dstd::Vector<dstd::String> rows_of(const dataframe::DataFrame& f) {
    dstd::Vector<dstd::String> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        out.push_back(row_text(f, r));
    return out;
}

bool interned(const dstd::StringIntern& intern, dstd::StringView text) {
    for (std::size_t i = 0; i < intern.entry_count(); ++i)
        if (intern.resolve(intern.entry_id(i)) == text) return true;
    return false;
}

}  // namespace

TEST_SUITE("RowSetFold") {
    TEST_CASE("the built-in sources index their lookup row sets") {
        dstd::Vector<dstd::String> names;
        for (const auto& r : trace::index::indexed_rowsets(
                 dftu_utils_test::ok_ref(trace::index::get_schema("dftracer"))))
            names.emplace_back(r.name);
        CHECK(names ==
              dstd::Vector<dstd::String>{"files", "hosts", "strings", "ranks"});
        names.clear();
        for (const auto& r : trace::index::indexed_rowsets(
                 dftu_utils_test::ok_ref(trace::index::get_schema("genesis"))))
            names.emplace_back(r.name);
        CHECK(names.back() == "runs");
        CHECK(trace::index::indexed_rowsets(
                  dftu_utils_test::ok_ref(trace::index::get_schema("generic")))
                  .empty());
    }

    TEST_CASE("a row set that aggregates or reads another is not indexed") {
        const auto& s = dftu_utils_test::ok_ref(trace::index::register_schema(
            "id: rs_mixed\nsource: |\n"
            "  big = where x > 1 | select x, y;\n"
            "  counts = where x > 1 | group y { n = count() };\n"
            "  keyed = where y in (from big | select y)\n",
            "test"));
        dstd::Vector<dstd::String> names;
        for (const auto& r : trace::index::indexed_rowsets(s))
            names.emplace_back(r.name);
        CHECK(names == dstd::Vector<dstd::String>{"big"});
    }

    TEST_CASE("each stored row set equals the pipeline over every record") {
        TestEnvironment env(10);
        const dstd::String gz = write_trace(env, trace_lines());
        const auto ixr = dftu_utils_test::ok(
            trace::index::Indexer::open({dstd::String(dstd::StringView(gz))}));
        const View v = View::from_file(
            dstd::String(gz),
            dstd::String(dstd::String(determine_index_path(gz, ""))));
        for (const char* name : {"files", "hosts", "strings", "ranks"}) {
            CAPTURE(name);
            const dataframe::DataFrame stored =
                dftu_utils_test::ok(ixr.rowset(name));
            const dataframe::DataFrame scanned =
                run(v.duql(dstd::String("from ") + name + " | where true")
                        .collect());
            CHECK(rows_of(stored) == rows_of(scanned));
        }
        CHECK(rows_of(dftu_utils_test::ok(ixr.rowset("files"))) ==
              dstd::Vector<dstd::String>{"fhash=fh1;path=/data/a.bin;",
                                         "fhash=fh2;path=/data/b.bin;"});
        CHECK(rows_of(dftu_utils_test::ok(ixr.rowset("ranks"))) ==
              dstd::Vector<dstd::String>{"pid=7;rank=3;"});
    }

    TEST_CASE("the stored genesis runs equal the pipeline over every record") {
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines = {
            R"({"gtype":"run","run":"ab","app":"laghos","nodes":4})",
            R"({"gtype":"run","run":"cd","app":"lulesh","nodes":8})"};
        for (int i = 0; i < 30; ++i)
            lines.push_back(
                R"({"gtype":"func","run":"ab","path":"main;f","ts":1,"depth":1,"count":2})");
        const dstd::String gz = write_trace(env, lines);
        const auto ixr = dftu_utils_test::ok(
            trace::index::Indexer::open({dstd::String(dstd::StringView(gz))}));
        const View v = View::from_file(
            dstd::String(gz),
            dstd::String(dstd::String(determine_index_path(gz, ""))));
        const dataframe::DataFrame stored =
            dftu_utils_test::ok(ixr.rowset("runs"));
        CHECK(rows_of(stored) ==
              rows_of(run(v.duql("from runs | where true").collect())));
        REQUIRE(stored.num_rows() == 2);
        CHECK(row_text(stored, 1).find("app=lulesh;") != dstd::String::npos);
    }

    TEST_CASE("the first genesis aggregate leaves out the run records") {
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines = {
            R"({"gtype":"run","run":"ab","app":"laghos","nodes":4})"};
        for (int i = 0; i < 30; ++i)
            lines.push_back(
                R"({"gtype":"func","run":"ab","path":"main;f","ts":1,"depth":1,"count":2})");
        const dstd::String gz = write_trace(env, lines, /*build=*/false);
        const View v =
            View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))))
                .group_by({GroupKey::field("gtype")})
                .agg({{AggOp::Count, "", "n"}});
        for (int touch = 0; touch < 2; ++touch) {
            CAPTURE(touch);
            const dataframe::DataFrame f =
                dftu_utils_test::ok(v.collect().get());
            CHECK(rows_of(f) == dstd::Vector<dstd::String>{"gtype=func;n=30;"});
        }
    }

    TEST_CASE("a distinct row set keeps each row once across units") {
        dstd::StringIntern intern;
        auto lines = trace_lines();
        lines.push_back(lines[0]);
        auto events = fold_events_of(lines, intern);
        RowSetFold fold(intern,
                        trace::index::indexed_rowsets(dftu_utils_test::ok_ref(
                            trace::index::get_schema("dftracer"))));
        ScanUnit unit;
        unit.file_path = "x";
        for (int i = 0; i < 2; ++i) {
            dftu_utils_test::ok(fold.step(
                FoldBatch{dstd::Span<const FoldEvent>(events), unit}));
            fold.seal_unit(unit);
        }
        CHECK(fold.entry_count() == 5);
    }

    TEST_CASE("a fold over budget keeps no rows") {
        dstd::StringIntern intern;
        auto events = fold_events_of(trace_lines(), intern);
        RowSetFold fold(intern,
                        trace::index::indexed_rowsets(dftu_utils_test::ok_ref(
                            trace::index::get_schema("dftracer"))),
                        16);
        ScanUnit unit;
        unit.file_path = "x";
        dftu_utils_test::ok(
            fold.step(FoldBatch{dstd::Span<const FoldEvent>(events), unit}));
        fold.seal_unit(unit);
        CHECK(fold.abandoned());
        CHECK(fold.entry_count() == 0);
    }

    TEST_CASE("a dropped unit leaves no rows") {
        dstd::StringIntern intern;
        auto events = fold_events_of(trace_lines(), intern);
        RowSetFold fold(intern,
                        trace::index::indexed_rowsets(dftu_utils_test::ok_ref(
                            trace::index::get_schema("dftracer"))));
        ScanUnit unit;
        unit.file_path = "x";
        dftu_utils_test::ok(
            fold.step(FoldBatch{dstd::Span<const FoldEvent>(events), unit}));
        fold.drop_unit(unit);
        CHECK(fold.entry_count() == 0);
        dftu_utils_test::ok(
            fold.step(FoldBatch{dstd::Span<const FoldEvent>(events), unit}));
        fold.seal_unit(unit);
        CHECK(fold.entry_count() == 5);
    }

    TEST_CASE("a never_index leaf is cataloged but its value is not interned") {
        const dstd::String line =
            R"({"gtype":"func","run":"ab","path":"main;f","ts":1,)"
            R"("dur":{"sketch":"UNIQUE_SKETCH_BLOB","count":2}})";
        const auto& genesis =
            dftu_utils_test::ok_ref(trace::index::get_schema("genesis"));
        for (const bool skip : {true, false}) {
            CAPTURE(skip);
            dstd::StringIntern intern;
            simdjson::dom::parser parser;
            dstd::String buf = padded(line);
            auto doc = parser.parse(buf.data(), line.size(), false);
            REQUIRE_FALSE(doc.error());
            FoldEvent ev;
            decode_record_into(ev, doc.value_unsafe(), intern, true, nullptr,
                               &genesis, nullptr, INDEX_MAX_CHILDREN, nullptr,
                               skip);
            const std::uint32_t id = intern.get_or_insert("dur.sketch");
            bool cataloged = false;
            for (const auto& [leaf, tag] : ev.schema_leaves)
                cataloged |= leaf == id;
            bool kept = false;
            for (const auto& [key, value] : ev.args) kept |= key == id;
            CHECK(cataloged);
            CHECK(kept == !skip);
            CHECK(interned(intern, "UNIQUE_SKETCH_BLOB") == !skip);
            CHECK(interned(intern, "main;f"));
        }
    }

    TEST_CASE("an index build skips never_index values and queries find them") {
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines;
        for (int i = 0; i < 30; ++i)
            lines.push_back(
                R"({"gtype":"func","run":"ab","path":"main;f","ts":1,"dur":{"sketch":"SK)" +
                dstd::to_string(i) + R"(","count":2}})");
        const dstd::String gz = write_trace(env, lines);
        const auto ixr = dftu_utils_test::ok(
            trace::index::Indexer::open({dstd::String(dstd::StringView(gz))}));
        const View v = View::from_file(
            dstd::String(gz),
            dstd::String(dstd::String(determine_index_path(gz, ""))));
        const dataframe::DataFrame f = dftu_utils_test::ok(
            v.duql("where dur.sketch == \"SK7\" | select path, dur.count")
                .collect()
                .get());
        REQUIRE(f.num_rows() == 1);
        const dataframe::DataFrame all = dftu_utils_test::ok(
            v.duql("where dur.sketch in [\"SK7\", \"SK8\", \"SK99\"]")
                .group_by({GroupKey::field("gtype")})
                .agg({{AggOp::Count, "", "n"}})
                .collect()
                .get());
        CHECK(rows_of(all) == dstd::Vector<dstd::String>{"gtype=func;n=2;"});
    }
}
