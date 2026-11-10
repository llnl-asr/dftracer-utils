// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <generator_ok.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <fstream>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

using namespace dftracer::utils::trace;

namespace {

// A trace with one CM metadata record (args.name=time_metric, args.value=NS)
// plus a few normal complete events. `numeric_ph` writes the current integer
// "ph" form (4 = metadata, 1 = complete); otherwise the legacy letter form
// ("M"/"X"), so the same code path is exercised through read_phase either way.
dstd::String create_metadata_trace(TestEnvironment& env, bool numeric_ph) {
    const dstd::String tag = numeric_ph ? "num" : "str";
    const dstd::String dir = env.dir() + "/" + tag;
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String pfw = dir + "/meta.pfw";
    dftu_utils_test::FileOut ofs(pfw);
    if (numeric_ph)
        ofs << R"({"name":"CM","ph":4,"pid":0,"tid":0,"ts":0,"args":{"name":"time_metric","value":"NS"}})"
            << "\n";
    else
        ofs << R"({"name":"CM","ph":"M","pid":0,"tid":0,"ts":0,"args":{"name":"time_metric","value":"NS"}})"
            << "\n";
    const char* ev_ph = numeric_ph ? "1" : "\"X\"";
    for (int i = 0; i < 5; ++i)
        ofs << R"({"name":"read","cat":"POSIX","ph":)" << ev_ph
            << R"(,"pid":1,"tid":1,"ts":)" << (1000 + i * 100) << R"(,"dur":)"
            << (10 + i) << R"(,"args":{}})" << "\n";
    ofs.close();
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    return gz;
}

// Build the index once (a metadata-phase query is not bootstrap-eligible, so an
// index must already exist).
void prime_index(const dstd::String& gz, const dstd::String& idx) {
    StringSink sink;
    dftu_utils_test::ok(View::from_file(dstd::String(gz), dstd::String(idx))
                            .sink_json(sink)
                            .get());
}

// Rows whose top-level name column equals `name`.
std::int64_t rows_named(const dataframe::DataFrame& df,
                        const dstd::String& name) {
    if (!bhas(df, "name")) return 0;
    std::int64_t n = 0;
    for (std::int64_t i = 0; i < df.num_rows(); ++i)
        if (bstr(df, i, "name") == name) ++n;
    return n;
}

void check_metadata_variant(bool numeric_ph) {
    TestEnvironment env(200);
    const dstd::String gz = create_metadata_trace(env, numeric_ph);
    const dstd::String idx = dstd::String(determine_index_path(gz, ""));
    prime_index(gz, idx);

    // phase("metadata") returns the CM record as a row, args flattened.
    views::detail::scan::ScanPlan meta = views::detail::scan::phase(
        views::detail::scan::from_file(dstd::String(gz), dstd::String(idx)),
        Phase::Metadata);
    dataframe::LazyFrame meta_lazy = views::detail::scan::collect(meta);
    dataframe::DataFrame via_lazy = run(meta_lazy.collect());
    dataframe::DataFrame via_eager =
        run(views::detail::scan::collect_frame(meta));

    REQUIRE(via_lazy.num_rows() > 0);
    REQUIRE(via_eager.num_rows() == via_lazy.num_rows());
    CHECK(bhas(via_lazy, "args.name"));
    CHECK(bhas(via_lazy, "args.value"));
    CHECK(rows_named(via_lazy, "CM") == 1);
    CHECK(rows_named(via_eager, "CM") == 1);
    bool found = false;
    for (std::int64_t i = 0; i < via_lazy.num_rows(); ++i)
        if (bstr(via_lazy, i, "args.name") == "time_metric" &&
            bstr(via_lazy, i, "args.value") == "NS")
            found = true;
    CHECK(found);

    // The query filter applies to metadata rows (both collect paths).
    views::detail::scan::ScanPlan filtered = views::detail::scan::filter(
        views::detail::scan::phase(
            views::detail::scan::from_file(dstd::String(gz), dstd::String(idx)),
            Phase::Metadata),
        dftu_utils_test::ok(
            duql::Query::from_string("args.name == \"time_metric\"")));
    dataframe::LazyFrame filtered_lazy = views::detail::scan::collect(filtered);
    dataframe::DataFrame f_lazy = run(filtered_lazy.collect());
    dataframe::DataFrame f_eager =
        run(views::detail::scan::collect_frame(filtered));
    CHECK(f_lazy.num_rows() == 1);
    CHECK(f_eager.num_rows() == 1);
    CHECK(bstr(f_lazy, 0, "args.value") == "NS");

    // A non-matching metadata filter returns nothing (the filter really runs).
    views::detail::scan::ScanPlan no_match = views::detail::scan::filter(
        views::detail::scan::phase(
            views::detail::scan::from_file(dstd::String(gz), dstd::String(idx)),
            Phase::Metadata),
        dftu_utils_test::ok(duql::Query::from_string("args.name == \"nope\"")));
    dataframe::LazyFrame no_match_lazy = views::detail::scan::collect(no_match);
    CHECK(run(no_match_lazy.collect()).num_rows() == 0);
    CHECK(run(views::detail::scan::collect_frame(no_match)).num_rows() == 0);

    // A normal event row query never includes the metadata record.
    views::detail::scan::ScanPlan events = views::detail::scan::phase(
        views::detail::scan::from_file(dstd::String(gz), dstd::String(idx)),
        Phase::Events);
    dataframe::LazyFrame events_lazy = views::detail::scan::collect(events);
    dataframe::DataFrame ev_lazy = run(events_lazy.collect());
    CHECK(ev_lazy.num_rows() == 5);
    CHECK(rows_named(ev_lazy, "CM") == 0);
    CHECK(rows_named(ev_lazy, "read") == 5);
}

}  // namespace

TEST_SUITE("View - metadata phase row query") {
    TEST_CASE(
        "phase(Metadata) row query returns filtered metadata (string ph)") {
        check_metadata_variant(/*numeric_ph=*/false);
    }

    TEST_CASE(
        "phase(Metadata) row query returns filtered metadata (numeric ph)") {
        check_metadata_variant(/*numeric_ph=*/true);
    }
}

namespace {

// 40 threads, each named by a thread_name record just before its events, and
// 4 FH records, over small gzip members so the records spread across chunks.
// Only thread 0 writes "open" events.
dstd::String create_threaded_trace(TestEnvironment& env) {
    const dstd::String pfw = env.dir() + "/threads.pfw";
    {
        dftu_utils_test::FileOut ofs(pfw);
        ofs << "[\n";
        for (int t = 0; t < 40; ++t) {
            ofs << R"({"name":"thread_name","ph":"M","pid":1,"tid":)" << t
                << R"(,"args":{"name":"t)" << t << R"("}})" << "\n";
            if (t % 10 == 0)
                ofs << R"({"name":"FH","ph":"M","pid":1,"tid":)" << t
                    << R"(,"args":{"name":"/f)" << t << R"(","value":"h)" << t
                    << R"("}})" << "\n";
            for (int i = 0; i < 50; ++i)
                ofs << R"({"name":")" << (t == 0 ? "open" : "read")
                    << R"(","cat":"POSIX","ph":"X","pid":1,"tid":)" << t
                    << R"(,"ts":)" << (t * 1000 + i) << R"(,"dur":1,"args":{}})"
                    << "\n";
        }
        ofs << "]\n";
    }
    const dstd::String gz = pfw + ".gz";
    REQUIRE(
        dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, 4 * 1024));
    dstd::fs::remove(pfw).discard();
    dftu_utils_test::ok(
        dftu_utils_test::ok(dftracer::utils::index::Indexer::open(
                                {dstd::String(dstd::StringView(gz))}))
            .build());
    return gz;
}

}  // namespace

TEST_SUITE("View - metadata under pruning") {
    TEST_CASE("the index counts metadata records per chunk") {
        TestEnvironment env(200);
        const dstd::String gz = create_threaded_trace(env);
        const auto snapshot = dftu_utils_test::index_snapshot(
            dstd::String(determine_index_path(gz, "")));
        auto counts =
            dftu_utils_test::index_take(dftracer::utils::index::chunk_meta(
                snapshot, dftu_utils_test::require_indexed_file(snapshot, gz)));
        REQUIRE(counts.has_value());
        std::uint64_t records = 0, with_records = 0;
        for (const auto& [chunk, m] : *counts) {
            records += m.records;
            with_records += m.records > 0 ? 1 : 0;
        }
        CHECK(records == 44);
        CHECK(with_records > 1);
        CHECK(with_records < counts->size());
    }

    TEST_CASE("metadata records are rows only under all") {
        TestEnvironment env(200);
        const dstd::String gz = create_threaded_trace(env);
        const dstd::String idx = dstd::String(determine_index_path(gz, ""));
        StringSink data;
        dftu_utils_test::ok(View::from_file(dstd::String(gz), dstd::String(idx))
                                .sink_json(data)
                                .get());
        CHECK(count_containing(data.lines(), "thread_name") == 0);
        StringSink every;
        dftu_utils_test::ok(View::from_file(dstd::String(gz), dstd::String(idx))
                                .phase(Phase::Any)
                                .sink_json(every)
                                .get());
        CHECK(count_containing(every.lines(), "thread_name") == 40);
        StringSink pruned;
        dftu_utils_test::ok(View::from_file(dstd::String(gz), dstd::String(idx))
                                .phase(Phase::Any)
                                .duql(R"(name in ["open", "thread_name"])")
                                .sink_json(pruned)
                                .get());
        CHECK(count_containing(pruned.lines(), "thread_name") == 40);
        CHECK(count_containing(pruned.lines(), R"("name":"open")") == 50);
    }

    TEST_CASE("a metadata query finds records in any chunk") {
        TestEnvironment env(200);
        const dstd::String gz = create_threaded_trace(env);
        const dstd::String idx = dstd::String(determine_index_path(gz, ""));
        views::detail::scan::ScanPlan q = views::detail::scan::filter(
            views::detail::scan::phase(views::detail::scan::from_file(
                                           dstd::String(gz), dstd::String(idx)),
                                       Phase::Metadata),
            dftu_utils_test::ok(
                duql::Query::from_string(R"(args.name == "t37")")));
        CHECK(run(views::detail::scan::collect_frame(q)).num_rows() == 1);
        views::detail::scan::ScanPlan all = views::detail::scan::filter(
            views::detail::scan::phase(views::detail::scan::from_file(
                                           dstd::String(gz), dstd::String(idx)),
                                       Phase::Metadata),
            dftu_utils_test::ok(
                duql::Query::from_string(R"(name == "thread_name")")));
        CHECK(run(views::detail::scan::collect_frame(all)).num_rows() == 40);
    }

    TEST_CASE("a metadata query reads no chunk of a file without metadata") {
        TestEnvironment env(200);
        const dstd::String pfw = env.dir() + "/plain.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            for (int i = 0; i < 2000; ++i)
                ofs << R"({"name":"read","cat":"POSIX","ph":"X","pid":1,)"
                    << R"("tid":1,"ts":)" << i << R"(,"dur":1,"args":{}})"
                    << "\n";
        }
        const dstd::String gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz,
                                                                   4 * 1024));
        dftu_utils_test::ok(
            dftu_utils_test::ok(dftracer::utils::index::Indexer::open(
                                    {dstd::String(dstd::StringView(gz))}))
                .build());
        StringSink sink;
        const auto stats =
            run(View::from_file(
                    dstd::String(gz),
                    dstd::String(dstd::String(determine_index_path(gz, ""))))
                    .phase(Phase::Metadata)
                    .sink_json(sink));
        CHECK(stats.chunks_scanned == 0);
        CHECK(sink.lines().empty());
    }

    TEST_CASE("an index without dft.metadata still returns every record") {
        TestEnvironment env(200);
        const dstd::String gz = create_threaded_trace(env);
        const dstd::String idx = dstd::String(determine_index_path(gz, ""));
        dftu_utils_test::ok(
            dftu_utils_test::ok(dftracer::utils::index::Indexer::open(
                                    {dstd::String(dstd::StringView(gz))}))
                .drop_extension("dft.metadata"));
        {
            const auto snapshot = dftu_utils_test::index_snapshot(idx);
            CHECK_FALSE(dftu_utils_test::index_take(
                            dftracer::utils::index::chunk_meta(
                                snapshot, dftu_utils_test::require_indexed_file(
                                              snapshot, gz)))
                            .has_value());
        }
        views::detail::scan::ScanPlan all = views::detail::scan::filter(
            views::detail::scan::phase(views::detail::scan::from_file(
                                           dstd::String(gz), dstd::String(idx)),
                                       Phase::Metadata),
            dftu_utils_test::ok(
                duql::Query::from_string(R"(name == "thread_name")")));
        CHECK(run(views::detail::scan::collect_frame(all)).num_rows() == 40);
        StringSink pruned;
        dftu_utils_test::ok(View::from_file(dstd::String(gz), dstd::String(idx))
                                .phase(Phase::Any)
                                .duql(R"(name in ["open", "thread_name"])")
                                .sink_json(pruned)
                                .get());
        CHECK(count_containing(pruned.lines(), "thread_name") == 40);
    }

    TEST_CASE("the trace reader keeps metadata lines its query matches") {
        TestEnvironment env(200);
        const dstd::String gz = create_threaded_trace(env);
        auto count_with = [&](const dstd::String& text, bool skip) {
            auto run = [&]() -> coro::CoroTask<std::size_t> {
                dftracer::utils::utilities::reader::TraceReader reader(
                    {.file_path = dstd::String(gz)});
                dftracer::utils::utilities::reader::ReadConfig cfg;
                cfg.query = text;
                cfg.skip_pruning = skip;
                auto gen = dftu_utils_test::ok_items(reader.read_json(cfg));
                std::size_t n = 0;
                while (auto line = co_await gen.next()) ++n;
                co_return n;
            };
            return run().get();
        };
        auto count = [&](const dstd::String& t) {
            return count_with(t, false);
        };
        auto count_all = [&](const dstd::String& t) {
            return count_with(t, true);
        };
        CHECK(dftu_utils_test::ok(count("")) == 2044);
        CHECK(dftu_utils_test::ok(count_all(R"(ph == "M")")) == 44);
        CHECK(dftu_utils_test::ok(count(R"(name == "thread_name")")) == 40);
        CHECK(dftu_utils_test::ok(count_all(R"(name == "thread_name")")) == 40);
        CHECK(dftu_utils_test::ok(count(R"(args.name == "t37")")) == 1);
        CHECK(dftu_utils_test::ok(count(R"(name == "open")")) == 50);
    }

    TEST_CASE("metadata evidence prunes chunks no record can match") {
        TestEnvironment env(200);
        const dstd::String gz = create_threaded_trace(env);
        auto chunks = [&](const dstd::String& text,
                          index::plan::MetadataUse use) {
            const auto q = dftu_utils_test::ok(
                dftracer::utils::duql::Query::from_string(text));
            auto out = index::plan::prune_file(
                           {.index_path = determine_index_path(gz, ""),
                            .file_path = dstd::String(dstd::StringView(gz)),
                            .query = &q,
                            .metadata = use})
                           .get();
            REQUIRE(out.has_value());
            REQUIRE_FALSE(out->all_chunks);
            return out->candidates.size();
        };
        const auto total = chunks("dur >= 0", index::plan::MetadataUse::All);
        const auto open =
            chunks(R"(name == "open")", index::plan::MetadataUse::None);
        // No metadata record is named "open" or has a dur.
        CHECK(chunks(R"(name == "open")", index::plan::MetadataUse::All) ==
              open);
        CHECK(chunks("dur > 5", index::plan::MetadataUse::Records) == 0);
        const auto named = chunks(R"(name == "thread_name")",
                                  index::plan::MetadataUse::Records);
        CHECK(named > 1);
        CHECK(named < total);
        CHECK(chunks(R"(name == "FH")", index::plan::MetadataUse::Records) <=
              4);
        CHECK(chunks(R"(args.name == "t37")",
                     index::plan::MetadataUse::Records) >= 1);
        CHECK(chunks(R"(args.name == "t37")", index::plan::MetadataUse::All) >=
              1);
    }
}
