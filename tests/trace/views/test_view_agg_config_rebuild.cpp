// SPDX-License-Identifier: MIT
// A trace rewritten after its root was indexed is rebuilt through a view with
// the aggregation settings the root was built with, whatever they are, and the
// tier of the other trace folder that shares the root is left as it was.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <fstream>
#include <type_traits>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

namespace {

constexpr std::size_t MEMBER_BYTES = 2048;
constexpr std::uint64_t BASE_TS = 1'000'000'000;

void write_trace(dstd::StringView gz_path, int pid, int reads, int writes) {
    const dstd::String gz(gz_path);
    const dstd::String plain = gz.substr(0, gz.size() - 3);
    {
        dftu_utils_test::FileOut o(plain);
        for (int j = 0; j < reads; ++j)
            o << R"({"ph":1,"name":"read","cat":"POSIX","pid":)" << pid
              << R"(,"tid":1,"ts":)" << (BASE_TS + j * 100) << R"(,"dur":)"
              << 10 + j % 7 << R"(,"args":{"hhash":"h"}})" << "\n";
        for (int j = 0; j < writes; ++j)
            o << R"({"ph":1,"name":"fwrite","cat":"STDIO","pid":)" << pid
              << R"(,"tid":1,"ts":)" << (BASE_TS + 50000 + j * 100)
              << R"(,"dur":)" << 20 + j % 5 << R"(,"args":{"hhash":"h"}})"
              << "\n";
    }
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               MEMBER_BYTES));
    REQUIRE(dstd::fs::remove(plain).has_value());
}

dstd::Result<dataframe::DataFrame> collect_of(dstd::Vector<ViewFile> files) {
    return View::from_files(files)
        .group_by({GroupKey::name()})
        .agg({{AggOp::Count, "", "n"}})
        .collect()
        .get();
}

dstd::Map<dstd::String, double> counts_of(dstd::Vector<ViewFile> files) {
    const dataframe::DataFrame got = dftu_utils_test::ok(collect_of(files));
    dstd::Map<dstd::String, double> out;
    for (std::int64_t r = 0; r < got.num_rows(); ++r)
        out[bstr(got, r, "name")] = bnum(got, r, "n");
    return out;
}

dstd::Map<dstd::String, double> expect(int reads, int writes) {
    dstd::Map<dstd::String, double> out;
    if (reads) out["read"] = reads;
    if (writes) out["fwrite"] = writes;
    return out;
}

dstd::Map<dstd::String, double> plus(dstd::Map<dstd::String, double> a,
                                     const dstd::Map<dstd::String, double>& b) {
    for (const auto& [k, v] : b) a[k] += v;
    return a;
}

void rewritten_trace_keeps_the_root_config(
    trace::index::schemas::dft::agg::AggregationConfig config) {
    TestEnvironment env{10};
    REQUIRE(env.is_valid());
    dstd::Vector<dstd::String> gz;
    for (int i = 0; i < 2; ++i) {
        const dstd::String dir = env.dir() + "/d" + dstd::to_string(i);
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        gz.push_back(dir + "/t.pfw.gz");
    }
    write_trace(gz[0], 100, 60, 20);
    write_trace(gz[1], 101, 40, 30);

    trace::index::IndexerOptions o;
    o.index_dir = env.dir() + "/idx";
    o.checkpoint_size = MEMBER_BYTES;
    o.aggregation = config;
    dftu_utils_test::ok(
        dftu_utils_test::ok(
            trace::index::Indexer::open(dftu_utils_test::dstd_paths(gz), o))
            .build());
    const dstd::String root =
        dstd::String(determine_index_path(gz.front(), o.index_dir));
    const dstd::Vector<ViewFile> left{{gz[0], root}};
    const dstd::Vector<ViewFile> right{{gz[1], root}};
    const dstd::Vector<ViewFile> both{{gz[0], root}, {gz[1], root}};

    CHECK(counts_of(left) == expect(60, 20));
    CHECK(counts_of(right) == expect(40, 30));

    write_trace(gz[0], 100, 7, 33);
    CHECK(counts_of(left) == expect(7, 33));
    // The other folder's tier answers with its trace gone.
    REQUIRE(dstd::fs::remove(gz[1]).has_value());
    CHECK(counts_of(right) == expect(40, 30));
    CHECK(counts_of(both) == plus(expect(7, 33), expect(40, 30)));

    auto index = dftu_utils_test::ok(trace::index::Index::open(root));
    const auto stored =
        dftu_utils_test::ok(trace::index::schemas::dft::agg::tier::read_config(
            dftu_utils_test::ok(index.snapshot())));
    REQUIRE(stored);
    CHECK(stored->params_hash == config.params_hash());
}

// A root of two dirs whose config record cannot be read back. A changed trace
// of one dir must not clear the tier of the other: the read of the changed
// dir is an error that names the root, and the other dir keeps its records.
template <class Record>
void unreadable_config_clears_nothing(const Record& record) {
    TestEnvironment env{10};
    REQUIRE(env.is_valid());
    dstd::Vector<dstd::String> gz;
    for (int i = 0; i < 2; ++i) {
        const dstd::String dir = env.dir() + "/d" + dstd::to_string(i);
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        gz.push_back(dir + "/t.pfw.gz");
    }
    write_trace(gz[0], 100, 60, 20);
    write_trace(gz[1], 101, 40, 30);
    trace::index::IndexerOptions o;
    o.index_dir = env.dir() + "/idx";
    o.checkpoint_size = MEMBER_BYTES;
    dftu_utils_test::ok(
        dftu_utils_test::ok(
            trace::index::Indexer::open(dftu_utils_test::dstd_paths(gz), o))
            .build());
    const dstd::String root =
        dstd::String(determine_index_path(gz.front(), o.index_dir));
    auto index = dftu_utils_test::ok(trace::index::Index::open(root));
    if constexpr (std::is_same_v<Record, dstd::String>) {
        run(trace::index::put_agg_meta(index, "config", record));
    } else {
        dftu_utils_test::ok(
            trace::index::schemas::dft::agg::tier::write_config(index, record));
    }

    auto right_ready = [&] {
        auto snapshot = dftu_utils_test::ok(index.snapshot());
        const auto file = snapshot.file(fileio::get_logical_path(gz[1]));
        REQUIRE(file);
        const auto state = trace::index::extension_state(
            snapshot, *file, trace::index::store::IndexExtension::Agg);
        return state && state->ready;
    };
    REQUIRE(right_ready());

    write_trace(gz[0], 100, 7, 33);
    REQUIRE(dstd::fs::remove(gz[1]).has_value());
    const auto changed = collect_of({{gz[0], root}});
    REQUIRE(!changed.has_value());
    CHECK(dstd::String(changed.error().message()).find(root) !=
          dstd::String::npos);
    // The other dir either answers from its tier or errors; its records stay.
    const auto other = collect_of({{gz[1], root}});
    if (other.has_value()) {
        dstd::Map<dstd::String, double> got;
        for (std::int64_t r = 0; r < other->num_rows(); ++r)
            got[bstr(*other, r, "name")] = bnum(*other, r, "n");
        CHECK(got == expect(40, 30));
    }
    CHECK(right_ready());
}

}  // namespace

TEST_SUITE("ViewAggConfigRebuild") {
    TEST_CASE("the standard settings") {
        rewritten_trace_keeps_the_root_config(
            trace::index::schemas::dft::agg::standard_aggregation_config());
    }

    TEST_CASE("compute_percentiles off") {
        auto c = trace::index::schemas::dft::agg::standard_aggregation_config();
        c.compute_percentiles = false;
        rewritten_trace_keeps_the_root_config(c);
    }

    TEST_CASE("relative time") {
        auto c = trace::index::schemas::dft::agg::standard_aggregation_config();
        c.use_relative_time = true;
        c.reference_timestamp = BASE_TS - 5;
        rewritten_trace_keeps_the_root_config(c);
    }

    TEST_CASE("normalize_time") {
        auto c = trace::index::schemas::dft::agg::standard_aggregation_config();
        c.normalize_time = true;
        rewritten_trace_keeps_the_root_config(c);
    }

    TEST_CASE("compute_statistics off") {
        auto c = trace::index::schemas::dft::agg::standard_aggregation_config();
        c.compute_statistics = false;
        rewritten_trace_keeps_the_root_config(c);
    }

    TEST_CASE("another percentile list") {
        auto c = trace::index::schemas::dft::agg::standard_aggregation_config();
        c.percentiles = {0.5, 0.99};
        rewritten_trace_keeps_the_root_config(c);
    }

    TEST_CASE("another sketch accuracy") {
        auto c = trace::index::schemas::dft::agg::standard_aggregation_config();
        c.sketch_accuracy = 0.05;
        rewritten_trace_keeps_the_root_config(c);
    }

    TEST_CASE("a config record of the earlier layout, short") {
        dstd::String record;
        dstd::append_u64_be(record, 1'000'000);
        dstd::append_u64_be(record, 0x1234);
        record.push_back('\x01');
        unreadable_config_clears_nothing(record);
    }

    TEST_CASE("a config record of the earlier layout, with extra keys") {
        dstd::String record;
        dstd::append_u64_be(record, 1'000'000);
        dstd::append_u64_be(record, 0x1234);
        record.push_back('\x01');
        record += "first_extra_group_key\x1fsecond_extra_group_key";
        REQUIRE(record.size() >= 37);
        unreadable_config_clears_nothing(record);
    }

    TEST_CASE("a stored hash its settings do not give") {
        auto c = trace::index::schemas::dft::agg::standard_aggregation_config();
        auto stored = trace::index::schemas::dft::agg::tier::make_config(c);
        stored.params_hash ^= 1;
        unreadable_config_clears_nothing(stored);
    }

    TEST_CASE("an extra group key the record cannot hold is refused") {
        TestEnvironment env{10};
        REQUIRE(env.is_valid());
        auto index = dftu_utils_test::ok(
            trace::index::Index::open(env.dir() + "/.dftindex"));
        for (const char* bad : {"",
                                "a\x1f"
                                "b"}) {
            auto c =
                trace::index::schemas::dft::agg::standard_aggregation_config();
            c.extra_group_keys = {"fine", bad};
            const auto written =
                trace::index::schemas::dft::agg::tier::write_config(
                    index,
                    trace::index::schemas::dft::agg::tier::make_config(c));
            CHECK(!written.has_value());
        }
        auto c = trace::index::schemas::dft::agg::standard_aggregation_config();
        c.extra_group_keys = {"epoch", "step"};
        dftu_utils_test::ok(trace::index::schemas::dft::agg::tier::write_config(
            index, trace::index::schemas::dft::agg::tier::make_config(c)));
        const auto read = dftu_utils_test::ok(
            trace::index::schemas::dft::agg::tier::read_config(
                dftu_utils_test::ok(index.snapshot())));
        REQUIRE(read);
        CHECK(read->extra_group_keys == c.extra_group_keys);
    }
}
