#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
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
