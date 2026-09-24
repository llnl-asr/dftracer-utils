#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>

#include <fstream>
#include <string>

#include "test_view_common.h"

namespace scan = dftracer::utils::trace::views::detail::scan;
using scan::ScanPlan;

namespace {

// A trace with one CM metadata record (args.name=time_metric, args.value=NS)
// plus a few normal complete events. `numeric_ph` writes the current integer
// "ph" form (4 = metadata, 1 = complete); otherwise the legacy letter form
// ("M"/"X"), so the same code path is exercised through read_phase either way.
std::string create_metadata_trace(TestEnvironment& env, bool numeric_ph) {
    const std::string tag = numeric_ph ? "num" : "str";
    const std::string dir = env.get_dir() + "/" + tag;
    fs::create_directories(dir);
    const std::string pfw = dir + "/meta.pfw";
    std::ofstream ofs(pfw);
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
    const std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

// Build the index once (a metadata-phase query is not bootstrap-eligible, so an
// index must already exist).
void prime_index(const std::string& gz, const std::string& idx) {
    StringSink sink;
    View::from_file(gz, idx).sink_json(sink).get();
}

// Rows whose top-level name column equals `name`.
std::int64_t rows_named(const dataframe::DataFrame& df,
                        const std::string& name) {
    if (!bhas(df, "name")) return 0;
    std::int64_t n = 0;
    for (std::int64_t i = 0; i < df.num_rows(); ++i)
        if (bstr(df, i, "name") == name) ++n;
    return n;
}

void check_metadata_variant(bool numeric_ph) {
    TestEnvironment env(200);
    const std::string gz = create_metadata_trace(env, numeric_ph);
    const std::string idx = determine_index_path(gz, "");
    prime_index(gz, idx);

    // phase("metadata") returns the CM record as a row, args flattened.
    ScanPlan meta = scan::phase(scan::from_file(gz, idx), Phase::Metadata);
    dataframe::LazyFrame meta_lazy = scan::collect(meta);
    dataframe::DataFrame via_lazy = run(meta_lazy.collect());
    dataframe::DataFrame via_eager = run(scan::collect_frame(meta));

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
    ScanPlan filtered =
        scan::query(scan::phase(scan::from_file(gz, idx), Phase::Metadata),
                    "args.name == \"time_metric\"");
    dataframe::LazyFrame filtered_lazy = scan::collect(filtered);
    dataframe::DataFrame f_lazy = run(filtered_lazy.collect());
    dataframe::DataFrame f_eager = run(scan::collect_frame(filtered));
    CHECK(f_lazy.num_rows() == 1);
    CHECK(f_eager.num_rows() == 1);
    CHECK(bstr(f_lazy, 0, "args.value") == "NS");

    // A non-matching metadata filter returns nothing (the filter really runs).
    ScanPlan no_match =
        scan::query(scan::phase(scan::from_file(gz, idx), Phase::Metadata),
                    "args.name == \"nope\"");
    dataframe::LazyFrame no_match_lazy = scan::collect(no_match);
    CHECK(run(no_match_lazy.collect()).num_rows() == 0);
    CHECK(run(scan::collect_frame(no_match)).num_rows() == 0);

    // A normal event row query never includes the metadata record.
    ScanPlan events = scan::phase(scan::from_file(gz, idx), Phase::Events);
    dataframe::LazyFrame events_lazy = scan::collect(events);
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
std::string create_threaded_trace(TestEnvironment& env) {
    const std::string pfw = env.get_dir() + "/threads.pfw";
    {
        std::ofstream ofs(pfw);
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
    const std::string gz = pfw + ".gz";
    REQUIRE(
        dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, 4 * 1024));
    fs::remove(pfw);
    dftracer::utils::index::Indexer::open({gz}).build();
    return gz;
}

}  // namespace

TEST_SUITE("View - metadata under pruning") {
    TEST_CASE("the index counts metadata and context records per chunk") {
        TestEnvironment env(200);
        const std::string gz = create_threaded_trace(env);
        dftracer::utils::index::store::IndexDatabase db(
            determine_index_path(gz, ""),
            dftracer::utils::index::store::IndexOpenMode::ReadOnly);
        const int fid = db.get_file_info_id(
            dftracer::utils::index::store::internal::get_logical_path(gz));
        auto counts = db.chunk_metadata(fid);
        REQUIRE(counts.has_value());
        std::uint64_t records = 0, context = 0, with_context = 0;
        for (const auto& [chunk, m] : *counts) {
            records += m.records;
            context += m.context;
            with_context += m.context > 0 ? 1 : 0;
        }
        CHECK(records == 44);
        CHECK(context == 40);
        CHECK(with_context > 1);
        CHECK(with_context < counts->size());
    }

    TEST_CASE("a pruned export keeps every thread_name record") {
        TestEnvironment env(200);
        const std::string gz = create_threaded_trace(env);
        const std::string idx = determine_index_path(gz, "");
        StringSink full;
        View::from_file(gz, idx).sink_json(full).get();
        StringSink pruned;
        View::from_file(gz, idx)
            .query(R"(name == "open")")
            .sink_json(pruned)
            .get();
        CHECK(count_containing(full.lines(), "thread_name") == 40);
        CHECK(count_containing(pruned.lines(), "thread_name") == 40);
        CHECK(count_containing(pruned.lines(), R"("name":"open")") == 50);
    }

    TEST_CASE("a metadata query finds records in any chunk") {
        TestEnvironment env(200);
        const std::string gz = create_threaded_trace(env);
        const std::string idx = determine_index_path(gz, "");
        ScanPlan q =
            scan::query(scan::phase(scan::from_file(gz, idx), Phase::Metadata),
                        R"(args.name == "t37")");
        CHECK(run(scan::collect_frame(q)).num_rows() == 1);
        ScanPlan all =
            scan::query(scan::phase(scan::from_file(gz, idx), Phase::Metadata),
                        R"(name == "thread_name")");
        CHECK(run(scan::collect_frame(all)).num_rows() == 40);
    }

    TEST_CASE("a metadata query reads no chunk of a file without metadata") {
        TestEnvironment env(200);
        const std::string pfw = env.get_dir() + "/plain.pfw";
        {
            std::ofstream ofs(pfw);
            for (int i = 0; i < 2000; ++i)
                ofs << R"({"name":"read","cat":"POSIX","ph":"X","pid":1,)"
                    << R"("tid":1,"ts":)" << i << R"(,"dur":1,"args":{}})"
                    << "\n";
        }
        const std::string gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz,
                                                                   4 * 1024));
        dftracer::utils::index::Indexer::open({gz}).build();
        StringSink sink;
        const auto stats = run(View::from_file(gz, determine_index_path(gz, ""))
                                   .phase(Phase::Metadata)
                                   .sink_json(sink));
        CHECK(stats.chunks_scanned == 0);
        CHECK(sink.lines().empty());
    }

    TEST_CASE("an index without dft.metadata still returns every record") {
        TestEnvironment env(200);
        const std::string gz = create_threaded_trace(env);
        const std::string idx = determine_index_path(gz, "");
        dftracer::utils::index::Indexer::open({gz}).drop_extension(
            "dft.metadata");
        {
            dftracer::utils::index::store::IndexDatabase db(
                idx, dftracer::utils::index::store::IndexOpenMode::ReadOnly);
            CHECK_FALSE(
                db.chunk_metadata(
                      db.get_file_info_id(dftracer::utils::index::store::
                                              internal::get_logical_path(gz)))
                    .has_value());
        }
        ScanPlan all =
            scan::query(scan::phase(scan::from_file(gz, idx), Phase::Metadata),
                        R"(name == "thread_name")");
        CHECK(run(scan::collect_frame(all)).num_rows() == 40);
        StringSink pruned;
        View::from_file(gz, idx)
            .query(R"(name == "open")")
            .sink_json(pruned)
            .get();
        CHECK(count_containing(pruned.lines(), "thread_name") == 40);
    }

    TEST_CASE("the trace reader keeps metadata lines its query matches") {
        TestEnvironment env(200);
        const std::string gz = create_threaded_trace(env);
        auto count_with = [&](const std::string& text, bool skip) {
            auto run = [&]() -> coro::CoroTask<std::size_t> {
                dftracer::utils::utilities::reader::TraceReader reader(
                    {.file_path = gz});
                dftracer::utils::utilities::reader::ReadConfig cfg;
                cfg.query = text;
                cfg.skip_pruning = skip;
                auto gen = reader.read_json(cfg);
                std::size_t n = 0;
                while (auto line = co_await gen.next()) ++n;
                co_return n;
            };
            return run().get();
        };
        auto count = [&](const std::string& t) { return count_with(t, false); };
        auto count_all = [&](const std::string& t) {
            return count_with(t, true);
        };
        CHECK(count("") == 2044);
        CHECK(count_all(R"(ph == "M")") == 44);
        CHECK(count(R"(name == "thread_name")") == 40);
        CHECK(count_all(R"(name == "thread_name")") == 40);
        CHECK(count(R"(args.name == "t37")") == 1);
        CHECK(count(R"(name == "open")") == 50);
    }

    TEST_CASE("metadata evidence prunes chunks no record can match") {
        TestEnvironment env(200);
        const std::string gz = create_threaded_trace(env);
        namespace ip = dftracer::utils::index::plan;
        auto chunks = [&](const std::string& text, ip::MetadataUse use) {
            const auto q = dftracer::utils::query::parse_or_throw(text);
            auto out =
                ip::prune_file({.index_path = determine_index_path(gz, ""),
                                .file_path = gz,
                                .query = &q,
                                .metadata = use})
                    .get();
            REQUIRE(out.has_value());
            REQUIRE_FALSE(out->all_chunks);
            return out->candidates.size();
        };
        const auto total = chunks("dur >= 0", ip::MetadataUse::ALL);
        const auto open = chunks(R"(name == "open")", ip::MetadataUse::NONE);
        // No metadata record is named "open" or has a dur.
        CHECK(chunks(R"(name == "open")", ip::MetadataUse::ALL) == open);
        CHECK(chunks("dur > 5", ip::MetadataUse::RECORDS) == 0);
        const auto named =
            chunks(R"(name == "thread_name")", ip::MetadataUse::RECORDS);
        CHECK(named > 1);
        CHECK(named < total);
        CHECK(chunks(R"(name == "FH")", ip::MetadataUse::RECORDS) <= 4);
        CHECK(chunks(R"(args.name == "t37")", ip::MetadataUse::RECORDS) >= 1);
        CHECK(chunks(R"(args.name == "t37")", ip::MetadataUse::ALL) >= 1);
    }
}
