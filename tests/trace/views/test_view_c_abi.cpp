// The trace View C ABI (dftu_view_*): every builder/terminal run through the
// C handle must agree with the equivalent dftracer::utils::trace::views::View
// call, and a builder must never consume its input.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/trace/views/abi.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "test_view_common.h"

using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::OpArgs;

namespace {

// Widens a FLAT numeric column's row `row` to double regardless of its
// stored width, matching test_view_common.h's bnum() so a value can be
// compared across the C ABI and C++ result the same test derives.
double abi_num(dftu_dataframe* df, std::int64_t row, const char* name) {
    dftu_series* c = dftu_dataframe_column(df, name);
    REQUIRE(c != nullptr);
    double v = 0.0;
    switch (dftu_series_type(c)) {
        case DFTU_TYPE_INT64:
            v = static_cast<double>(
                static_cast<const std::int64_t*>(dftu_series_data(c))[row]);
            break;
        case DFTU_TYPE_UINT64:
            v = static_cast<double>(
                static_cast<const std::uint64_t*>(dftu_series_data(c))[row]);
            break;
        case DFTU_TYPE_INT32:
            v = static_cast<double>(
                static_cast<const std::int32_t*>(dftu_series_data(c))[row]);
            break;
        case DFTU_TYPE_UINT32:
            v = static_cast<double>(
                static_cast<const std::uint32_t*>(dftu_series_data(c))[row]);
            break;
        case DFTU_TYPE_FLOAT64:
            v = static_cast<const double*>(dftu_series_data(c))[row];
            break;
        default:
            // A key column is text; reading it as a number would read past
            // its bytes. Compare it with abi_str.
            dftu_series_free(c);
            FAIL("abi_num: column '" << name << "' is not numeric");
            return 0.0;
    }
    dftu_series_free(c);
    return v;
}

std::string abi_str(dftu_dataframe* df, std::int64_t row, const char* name) {
    dftu_series* c = dftu_dataframe_column(df, name);
    REQUIRE(c != nullptr);
    const int32_t* offs = dftu_series_offsets(c);
    REQUIRE(offs != nullptr);
    const char* data = static_cast<const char*>(dftu_series_data(c));
    std::string s(data + offs[row], data + offs[row + 1]);
    dftu_series_free(c);
    return s;
}

// A trace with two distinct (pid, name, dur) groups, for group_by/agg
// coverage: pid 1 has two "read" events at dur=10, pid 2 has two "write"
// events at dur=20.
std::string write_group_trace(TestEnvironment& env) {
    std::string pfw = env.get_dir() + "/grp.pfw";
    std::ofstream ofs(pfw);
    ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":10,"args":{}})"
        << "\n";
    ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1100,"dur":10,"args":{}})"
        << "\n";
    ofs << R"({"ph":"X","name":"write","cat":"POSIX","pid":2,"tid":1,"ts":1000,"dur":20,"args":{}})"
        << "\n";
    ofs << R"({"ph":"X","name":"write","cat":"POSIX","pid":2,"tid":1,"ts":1100,"dur":20,"args":{}})"
        << "\n";
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

// A trace with two disjoint-interval pid groups of varying dur, for the
// AggOp values write_group_trace's flat dur=10/dur=20 groups cannot exercise:
// ArgMax (needs a group with more than one distinct value to pick a winner),
// Pct, and the occupancy ops (Busy is an exact interval union - since every
// event here is disjoint from its groupmates, it equals sum(dur)).
// pid 1: "read" dur=10, "readv" dur=50, "pread" dur=20 (total 80).
// pid 2: "write" dur=5, "writev" dur=30, "pwrite" dur=15 (total 50).
std::string write_argmax_trace(TestEnvironment& env) {
    std::string pfw = env.get_dir() + "/argmax.pfw";
    std::ofstream ofs(pfw);
    ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":10,"args":{}})"
        << "\n";
    ofs << R"({"ph":"X","name":"readv","cat":"POSIX","pid":1,"tid":1,"ts":1050,"dur":50,"args":{}})"
        << "\n";
    ofs << R"({"ph":"X","name":"pread","cat":"POSIX","pid":1,"tid":1,"ts":1200,"dur":20,"args":{}})"
        << "\n";
    ofs << R"({"ph":"X","name":"write","cat":"POSIX","pid":2,"tid":1,"ts":1000,"dur":5,"args":{}})"
        << "\n";
    ofs << R"({"ph":"X","name":"writev","cat":"POSIX","pid":2,"tid":1,"ts":1010,"dur":30,"args":{}})"
        << "\n";
    ofs << R"({"ph":"X","name":"pwrite","cat":"POSIX","pid":2,"tid":1,"ts":1100,"dur":15,"args":{}})"
        << "\n";
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

// Writes a small trace of `n` "ev" events at `dir`/name.pfw.gz and builds its
// index, for the directory-scan test.
std::string write_and_index(const std::string& dir, const std::string& name,
                            int n, int pid) {
    fs::create_directories(dir);
    std::string pfw = dir + "/" + name + ".pfw";
    std::ofstream ofs(pfw);
    for (int i = 0; i < n; ++i)
        ofs << R"({"ph":"X","name":")" << name << R"(","cat":"POSIX","pid":)"
            << pid << R"(,"tid":1,"ts":)" << (1000 + i * 100) << R"(,"dur":)"
            << (10 + i) << R"(,"args":{}})" << "\n";
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    std::string idx = determine_index_path(gz, "");
    StringSink sink;
    View::from_file(gz, idx).metadata(false).sink_json(sink).get();
    return gz;
}

}  // namespace

TEST_SUITE("View C ABI") {
    TEST_CASE("from_files + filter + collect matches the C++ plan") {
        const auto& s = shared_trace();
        const char* paths[1] = {s.gz.c_str()};
        const char* idxs[1] = {s.idx.c_str()};

        dftu_view* v = dftu_view_from_files(paths, idxs, 1);
        REQUIRE(v != nullptr);
        dftu_query* q = dftu_query_cmp_str("cat", DFTU_QCMP_EQ, "POSIX");
        REQUIRE(q != nullptr);
        dftu_view* filtered = dftu_view_filter(v, q);
        REQUIRE(filtered != nullptr);
        dftu_query_free(q);

        dftu_dataframe* df = dftu_view_collect(filtered, nullptr);
        REQUIRE(df != nullptr);

        View cpp_view = View::from_file(s.gz, s.idx).query(R"(cat == "POSIX")");
        DataFrame cpp_df = run(cpp_view.collect());

        CHECK(dftu_dataframe_num_rows(df) == cpp_df.num_rows());
        CHECK(dftu_dataframe_num_rows(df) == 30);
        for (std::int64_t i = 0; i < cpp_df.num_rows(); ++i) {
            CHECK(abi_str(df, i, "name") == bstr(cpp_df, i, "name"));
            CHECK(abi_num(df, i, "ts") == bnum(cpp_df, i, "ts"));
        }

        dftu_dataframe_free(df);
        dftu_view_free(filtered);
        dftu_view_free(v);
    }

    TEST_CASE("group_by + agg via the C ABI matches the C++ equivalent") {
        TestEnvironment env(50);
        std::string gz = write_group_trace(env);
        std::string idx = determine_index_path(gz, "");
        StringSink sink;
        View::from_file(gz, idx).metadata(false).sink_json(sink).get();

        const char* paths[1] = {gz.c_str()};
        const char* idxs[1] = {idx.c_str()};

        SUBCASE("Kind::Name") {
            dftu_view* v = dftu_view_from_files(paths, idxs, 1);
            REQUIRE(v != nullptr);
            dftu_group_key keys[1] = {{DFTU_GROUP_KEY_NAME, nullptr,
                                       DFTU_GROUP_TRANSFORM_NONE, nullptr, 0}};
            dftu_view* grouped = dftu_view_group_by(v, keys, 1);
            REQUIRE(grouped != nullptr);
            dftu_view_agg_spec aggs[1] = {
                {DFTU_VIEW_AGG_COUNT, nullptr, "n", nullptr, 0.0}};
            dftu_view* agged = dftu_view_agg(grouped, aggs, 1);
            REQUIRE(agged != nullptr);
            dftu_dataframe* df = dftu_view_collect(agged, nullptr);
            REQUIRE(df != nullptr);

            View cpp_view = View::from_file(gz, idx)
                                .group_by({GroupKey::name()})
                                .agg({{AggOp::Count, "", "n"}});
            DataFrame cpp_df = run(cpp_view.collect());

            REQUIRE(dftu_dataframe_num_rows(df) == cpp_df.num_rows());
            REQUIRE(dftu_dataframe_num_rows(df) == 2);
            for (std::int64_t i = 0; i < cpp_df.num_rows(); ++i) {
                CHECK(abi_str(df, i, "name") == bstr(cpp_df, i, "name"));
                CHECK(abi_num(df, i, "n") == bnum(cpp_df, i, "n"));
                CHECK(abi_num(df, i, "n") == 2);
            }

            dftu_dataframe_free(df);
            dftu_view_free(agged);
            dftu_view_free(grouped);
            dftu_view_free(v);
        }

        SUBCASE("Kind::Pid") {
            dftu_view* v = dftu_view_from_files(paths, idxs, 1);
            REQUIRE(v != nullptr);
            dftu_group_key keys[1] = {{DFTU_GROUP_KEY_PID, nullptr,
                                       DFTU_GROUP_TRANSFORM_NONE, nullptr, 0}};
            dftu_view* grouped = dftu_view_group_by(v, keys, 1);
            REQUIRE(grouped != nullptr);
            dftu_view_agg_spec aggs[1] = {
                {DFTU_VIEW_AGG_SUM, "dur", "total_dur", nullptr, 0.0}};
            dftu_view* agged = dftu_view_agg(grouped, aggs, 1);
            REQUIRE(agged != nullptr);
            dftu_dataframe* df = dftu_view_collect(agged, nullptr);
            REQUIRE(df != nullptr);

            View cpp_view = View::from_file(gz, idx)
                                .group_by({GroupKey::pid()})
                                .agg({{AggOp::Sum, "dur", "total_dur"}});
            DataFrame cpp_df = run(cpp_view.collect());

            REQUIRE(dftu_dataframe_num_rows(df) == cpp_df.num_rows());
            REQUIRE(dftu_dataframe_num_rows(df) == 2);
            for (std::int64_t i = 0; i < cpp_df.num_rows(); ++i) {
                CHECK(abi_str(df, i, "pid") == bstr(cpp_df, i, "pid"));
                CHECK(abi_num(df, i, "total_dur") ==
                      bnum(cpp_df, i, "total_dur"));
            }
            // pid 1 -> 2 events of dur=10; pid 2 -> 2 events of dur=20.
            CHECK((abi_num(df, 0, "total_dur") == 20 ||
                   abi_num(df, 0, "total_dur") == 40));

            dftu_dataframe_free(df);
            dftu_view_free(agged);
            dftu_view_free(grouped);
            dftu_view_free(v);
        }

        SUBCASE("Kind::Field with arg") {
            dftu_view* v = dftu_view_from_files(paths, idxs, 1);
            REQUIRE(v != nullptr);
            dftu_group_key keys[1] = {{DFTU_GROUP_KEY_FIELD, "dur",
                                       DFTU_GROUP_TRANSFORM_NONE, nullptr, 0}};
            dftu_view* grouped = dftu_view_group_by(v, keys, 1);
            REQUIRE(grouped != nullptr);
            dftu_view_agg_spec aggs[1] = {
                {DFTU_VIEW_AGG_COUNT, nullptr, "n", nullptr, 0.0}};
            dftu_view* agged = dftu_view_agg(grouped, aggs, 1);
            REQUIRE(agged != nullptr);
            dftu_dataframe* df = dftu_view_collect(agged, nullptr);
            REQUIRE(df != nullptr);

            View cpp_view = View::from_file(gz, idx)
                                .group_by({GroupKey::field("dur")})
                                .agg({{AggOp::Count, "", "n"}});
            DataFrame cpp_df = run(cpp_view.collect());

            REQUIRE(dftu_dataframe_num_rows(df) == cpp_df.num_rows());
            REQUIRE(dftu_dataframe_num_rows(df) == 2);
            for (std::int64_t i = 0; i < cpp_df.num_rows(); ++i) {
                CHECK(abi_str(df, i, "dur") == bstr(cpp_df, i, "dur"));
                CHECK(abi_num(df, i, "n") == 2);
            }

            dftu_dataframe_free(df);
            dftu_view_free(agged);
            dftu_view_free(grouped);
            dftu_view_free(v);
        }

        SUBCASE("Kind::ArgMax with by") {
            std::string am_gz = write_argmax_trace(env);
            std::string am_idx = determine_index_path(am_gz, "");
            StringSink am_sink;
            View::from_file(am_gz, am_idx)
                .metadata(false)
                .sink_json(am_sink)
                .get();
            const char* am_paths[1] = {am_gz.c_str()};
            const char* am_idxs[1] = {am_idx.c_str()};

            dftu_view* v = dftu_view_from_files(am_paths, am_idxs, 1);
            REQUIRE(v != nullptr);
            dftu_group_key keys[1] = {{DFTU_GROUP_KEY_PID, nullptr,
                                       DFTU_GROUP_TRANSFORM_NONE, nullptr, 0}};
            dftu_view* grouped = dftu_view_group_by(v, keys, 1);
            REQUIRE(grouped != nullptr);
            dftu_view_agg_spec aggs[1] = {
                {DFTU_VIEW_AGG_ARG_MAX, "name", "argmax_name", "dur", 0.0}};
            dftu_view* agged = dftu_view_agg(grouped, aggs, 1);
            REQUIRE(agged != nullptr);
            dftu_dataframe* df = dftu_view_collect(agged, nullptr);
            REQUIRE(df != nullptr);

            View cpp_view =
                View::from_file(am_gz, am_idx)
                    .group_by({GroupKey::pid()})
                    .agg({{AggOp::ArgMax, "name", "argmax_name", "dur"}});
            DataFrame cpp_df = run(cpp_view.collect());

            REQUIRE(dftu_dataframe_num_rows(df) == cpp_df.num_rows());
            REQUIRE(dftu_dataframe_num_rows(df) == 2);
            for (std::int64_t i = 0; i < cpp_df.num_rows(); ++i) {
                CHECK(abi_str(df, i, "pid") == bstr(cpp_df, i, "pid"));
                CHECK(abi_str(df, i, "argmax_name") ==
                      bstr(cpp_df, i, "argmax_name"));
                // pid 1's max-dur event is "readv" (50); pid 2's is "writev"
                // (30).
                CHECK((abi_str(df, i, "argmax_name") == "readv" ||
                       abi_str(df, i, "argmax_name") == "writev"));
            }

            dftu_dataframe_free(df);
            dftu_view_free(agged);
            dftu_view_free(grouped);
            dftu_view_free(v);
        }

        SUBCASE("Kind::Pct with q") {
            std::string am_gz = write_argmax_trace(env);
            std::string am_idx = determine_index_path(am_gz, "");
            StringSink am_sink;
            View::from_file(am_gz, am_idx)
                .metadata(false)
                .sink_json(am_sink)
                .get();
            const char* am_paths[1] = {am_gz.c_str()};
            const char* am_idxs[1] = {am_idx.c_str()};

            dftu_view* v = dftu_view_from_files(am_paths, am_idxs, 1);
            REQUIRE(v != nullptr);
            dftu_group_key keys[1] = {{DFTU_GROUP_KEY_PID, nullptr,
                                       DFTU_GROUP_TRANSFORM_NONE, nullptr, 0}};
            dftu_view* grouped = dftu_view_group_by(v, keys, 1);
            REQUIRE(grouped != nullptr);
            dftu_view_agg_spec aggs[1] = {
                {DFTU_VIEW_AGG_PCT, "dur", "p50_dur", nullptr, 0.5}};
            dftu_view* agged = dftu_view_agg(grouped, aggs, 1);
            REQUIRE(agged != nullptr);
            dftu_dataframe* df = dftu_view_collect(agged, nullptr);
            REQUIRE(df != nullptr);

            View cpp_view = View::from_file(am_gz, am_idx)
                                .group_by({GroupKey::pid()})
                                .agg({{AggOp::Pct, "dur", "p50_dur", "", 0.5}});
            DataFrame cpp_df = run(cpp_view.collect());

            REQUIRE(dftu_dataframe_num_rows(df) == cpp_df.num_rows());
            REQUIRE(dftu_dataframe_num_rows(df) == 2);
            for (std::int64_t i = 0; i < cpp_df.num_rows(); ++i) {
                CHECK(abi_str(df, i, "pid") == bstr(cpp_df, i, "pid"));
                CHECK(abi_num(df, i, "p50_dur") == bnum(cpp_df, i, "p50_dur"));
            }

            dftu_dataframe_free(df);
            dftu_view_free(agged);
            dftu_view_free(grouped);
            dftu_view_free(v);
        }

        SUBCASE("Kind::Busy occupancy") {
            std::string am_gz = write_argmax_trace(env);
            std::string am_idx = determine_index_path(am_gz, "");
            StringSink am_sink;
            View::from_file(am_gz, am_idx)
                .metadata(false)
                .sink_json(am_sink)
                .get();
            const char* am_paths[1] = {am_gz.c_str()};
            const char* am_idxs[1] = {am_idx.c_str()};

            dftu_view* v = dftu_view_from_files(am_paths, am_idxs, 1);
            REQUIRE(v != nullptr);
            dftu_group_key keys[1] = {{DFTU_GROUP_KEY_PID, nullptr,
                                       DFTU_GROUP_TRANSFORM_NONE, nullptr, 0}};
            dftu_view* grouped = dftu_view_group_by(v, keys, 1);
            REQUIRE(grouped != nullptr);
            dftu_view_agg_spec aggs[1] = {
                {DFTU_VIEW_AGG_BUSY, "", "busy_us", nullptr, 0.0}};
            dftu_view* agged = dftu_view_agg(grouped, aggs, 1);
            REQUIRE(agged != nullptr);
            dftu_dataframe* df = dftu_view_collect(agged, nullptr);
            REQUIRE(df != nullptr);

            View cpp_view = View::from_file(am_gz, am_idx)
                                .group_by({GroupKey::pid()})
                                .agg({{AggOp::Busy, "", "busy_us"}});
            DataFrame cpp_df = run(cpp_view.collect());

            REQUIRE(dftu_dataframe_num_rows(df) == cpp_df.num_rows());
            REQUIRE(dftu_dataframe_num_rows(df) == 2);
            for (std::int64_t i = 0; i < cpp_df.num_rows(); ++i) {
                CHECK(abi_str(df, i, "pid") == bstr(cpp_df, i, "pid"));
                CHECK(abi_num(df, i, "busy_us") == bnum(cpp_df, i, "busy_us"));
            }
            // Every event is disjoint from its groupmates, so busy == sum(dur):
            // pid 1 -> 80, pid 2 -> 50.
            CHECK((abi_num(df, 0, "busy_us") == 80 ||
                   abi_num(df, 0, "busy_us") == 50));

            dftu_dataframe_free(df);
            dftu_view_free(agged);
            dftu_view_free(grouped);
            dftu_view_free(v);
        }
    }

    TEST_CASE("a builder does not consume its input") {
        const auto& s = shared_trace();
        const char* paths[1] = {s.gz.c_str()};
        const char* idxs[1] = {s.idx.c_str()};
        dftu_view* base = dftu_view_from_files(paths, idxs, 1);
        REQUIRE(base != nullptr);

        dftu_query* q = dftu_query_cmp_str("cat", DFTU_QCMP_EQ, "POSIX");
        REQUIRE(q != nullptr);
        dftu_view* posix_only = dftu_view_filter(base, q);
        dftu_query_free(q);
        REQUIRE(posix_only != nullptr);

        dftu_view* limited = dftu_view_limit(base, 5);
        REQUIRE(limited != nullptr);

        dftu_dataframe* base_df = dftu_view_collect(base, nullptr);
        dftu_dataframe* posix_df = dftu_view_collect(posix_only, nullptr);
        dftu_dataframe* limited_df = dftu_view_collect(limited, nullptr);
        REQUIRE(base_df != nullptr);
        REQUIRE(posix_df != nullptr);
        REQUIRE(limited_df != nullptr);

        CHECK(dftu_dataframe_num_rows(base_df) == 50);
        CHECK(dftu_dataframe_num_rows(posix_df) == 30);
        CHECK(dftu_dataframe_num_rows(limited_df) == 5);

        dftu_dataframe_free(base_df);
        dftu_dataframe_free(posix_df);
        dftu_dataframe_free(limited_df);
        dftu_view_free(limited);
        dftu_view_free(posix_only);
        dftu_view_free(base);
    }

    TEST_CASE("dftu_view_lazy scans through the dftu.lazy.* op vocabulary") {
        const auto& s = shared_trace();
        const char* paths[1] = {s.gz.c_str()};
        const char* idxs[1] = {s.idx.c_str()};
        dftu_view* v = dftu_view_from_files(paths, idxs, 1);
        REQUIRE(v != nullptr);
        dftu_query* q = dftu_query_cmp_str("cat", DFTU_QCMP_EQ, "POSIX");
        REQUIRE(q != nullptr);
        dftu_view* posix_only = dftu_view_filter(v, q);
        dftu_query_free(q);
        REQUIRE(posix_only != nullptr);

        dftu_lazyframe* lf = dftu_view_lazy(posix_only);
        REQUIRE(lf != nullptr);

        const dftu_op_desc* sort_op = dftu_op_find("dftu.lazy.sort_by");
        REQUIRE(sort_op != nullptr);
        OpArgs sort_args;
        sort_args.str(1, "ts").i32(2, 0);
        const dftu_lazyframe* sort_in[1] = {lf};
        dftu_lazyframe* sorted =
            dftu_op_run_lazy(sort_op, sort_in, 1, sort_args);
        REQUIRE(sorted != nullptr);

        const dftu_op_desc* head_op = dftu_op_find("dftu.lazy.head");
        REQUIRE(head_op != nullptr);
        OpArgs head_args;
        head_args.i64(1, 5);
        const dftu_lazyframe* head_in[1] = {sorted};
        dftu_lazyframe* headed =
            dftu_op_run_lazy(head_op, head_in, 1, head_args);
        REQUIRE(headed != nullptr);

        dftu_dataframe* via_lazy = dftu_lazyframe_collect(headed, 0);
        REQUIRE(via_lazy != nullptr);

        View cpp_view = View::from_file(s.gz, s.idx)
                            .query(R"(cat == "POSIX")")
                            .sort_by("ts", false)
                            .head(5);
        DataFrame via_eager = run(cpp_view.collect());

        REQUIRE(dftu_dataframe_num_rows(via_lazy) == via_eager.num_rows());
        REQUIRE(dftu_dataframe_num_rows(via_lazy) == 5);
        for (std::int64_t i = 0; i < via_eager.num_rows(); ++i) {
            CHECK(abi_num(via_lazy, i, "ts") == bnum(via_eager, i, "ts"));
            CHECK(abi_str(via_lazy, i, "name") == bstr(via_eager, i, "name"));
        }

        dftu_dataframe_free(via_lazy);
        dftu_lazyframe_free(headed);
        dftu_lazyframe_free(sorted);
        dftu_lazyframe_free(lf);
        dftu_view_free(posix_only);
        dftu_view_free(v);
    }

    TEST_CASE("rt = NULL uses the default runtime; a non-NULL runtime works") {
        const auto& s = shared_trace();
        const char* paths[1] = {s.gz.c_str()};
        const char* idxs[1] = {s.idx.c_str()};
        dftu_view* v = dftu_view_from_files(paths, idxs, 1);
        REQUIRE(v != nullptr);

        dftu_dataframe* via_default = dftu_view_collect(v, nullptr);
        REQUIRE(via_default != nullptr);

        dftu_runtime* rt = dftu_runtime_new(2, 1);
        REQUIRE(rt != nullptr);
        dftu_dataframe* via_custom = dftu_view_collect(v, rt);
        REQUIRE(via_custom != nullptr);

        CHECK(dftu_dataframe_num_rows(via_default) ==
              dftu_dataframe_num_rows(via_custom));
        CHECK(dftu_dataframe_num_rows(via_default) == 50);

        dftu_dataframe_free(via_default);
        dftu_dataframe_free(via_custom);
        dftu_runtime_free(rt);
        dftu_view_free(v);
    }

    TEST_CASE(
        "from_directory scans recursively, matching an explicit "
        "from_files listing") {
        TestEnvironment env(10);
        std::string root = env.get_dir() + "/root";
        std::string gz_a = write_and_index(root + "/a", "eva", 3, 1);
        std::string gz_b = write_and_index(root + "/b", "evb", 4, 2);

        dftu_view* dirv =
            dftu_view_from_directory(root.c_str(), nullptr, nullptr);
        REQUIRE(dirv != nullptr);
        dftu_dataframe* dir_df = dftu_view_collect(dirv, nullptr);
        REQUIRE(dir_df != nullptr);

        const char* paths[2] = {gz_a.c_str(), gz_b.c_str()};
        dftu_view* explicit_v = dftu_view_from_files(paths, nullptr, 2);
        REQUIRE(explicit_v != nullptr);
        dftu_dataframe* explicit_df = dftu_view_collect(explicit_v, nullptr);
        REQUIRE(explicit_df != nullptr);

        REQUIRE(dftu_dataframe_num_rows(dir_df) ==
                dftu_dataframe_num_rows(explicit_df));
        REQUIRE(dftu_dataframe_num_rows(dir_df) == 7);
        // Files are scanned in parallel, so rows from different files
        // interleave in an order neither listing controls. Only the multiset
        // of rows is defined.
        auto rows = [](dftu_dataframe* df) {
            std::vector<std::pair<std::string, double>> out;
            for (std::int64_t i = 0; i < dftu_dataframe_num_rows(df); ++i)
                out.emplace_back(abi_str(df, i, "name"), abi_num(df, i, "pid"));
            std::sort(out.begin(), out.end());
            return out;
        };
        CHECK(rows(dir_df) == rows(explicit_df));

        dftu_dataframe_free(dir_df);
        dftu_dataframe_free(explicit_df);
        dftu_view_free(dirv);
        dftu_view_free(explicit_v);
    }
}
