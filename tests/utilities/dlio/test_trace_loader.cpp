// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/dlio/trace_loader.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;

namespace {

constexpr int FILES = 2;
constexpr int EVENTS_PER_NAME = 300;
constexpr std::uint64_t BASE_TS = 1'000'000'000;
constexpr std::size_t CHECKPOINT_SIZE = 6000;

struct Component {
    const char* cat;
    const char* name;
    std::uint64_t base_dur;
    std::uint64_t pid_offset;
};

constexpr Component COMPONENTS[] = {{"dataloader", "fetch.block", 100, 0},
                                    {"dataloader", "fetch.iter", 50, 0},
                                    {"data", "preprocess", 80, 0}};

// One rank per file, all events inside one 5 s window, several gzip members
// per file.
dstd::String write_rank(const dstd::String& dir, int file,
                        std::uint64_t& dur_sum) {
    const dstd::String pfw = dir + "/rank_" + dstd::to_string(file) + ".pfw";
    {
        dstd::String ofs;
        const std::uint64_t pid = 1000 + static_cast<std::uint64_t>(file);
        std::uint64_t ts = BASE_TS;
        std::uint64_t id = 1;
        ofs += "[\n";
        for (int i = 0; i < EVENTS_PER_NAME; ++i) {
            for (const auto& c : COMPONENTS) {
                const std::uint64_t dur =
                    c.base_dur + static_cast<std::uint64_t>((i * 7) % 90);
                if (dstd::String(c.name) == "fetch.block") dur_sum += dur;
                ofs += common::str_cat(R"({"id":)", id++, R"(,"pid":)", pid,
                                       R"(,"tid":)", pid, R"(,"name":")",
                                       c.name, R"(","cat":")", c.cat,
                                       R"(","ph":"X","ts":)", ts, R"(,"dur":)",
                                       dur, R"(,"args":{}})", "\n");
                ts += dur;
            }
        }
        ofs += "]\n";
        dstd::fs::write_file(dstd::PathView(pfw), ofs).discard();
    }
    const dstd::String gz = pfw + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
        pfw, gz, CHECKPOINT_SIZE));
    return gz;
}

}  // namespace

TEST_SUITE("dlio trace loader") {
    TEST_CASE("one rank per file is answered from the partials alone") {
        dftu_utils_test::TestEnvironment env{10};
        dstd::Vector<dstd::String> files;
        std::uint64_t dur_sum = 0;
        for (int i = 0; i < FILES; ++i)
            files.emplace_back(write_rank(env.dir(), i, dur_sum));

        index::schemas::dft::agg::AggregationConfig config;
        config.time_interval_us = 5'000'000;
        config.compute_statistics = true;
        config.compute_percentiles = true;

        index::IndexerOptions o;
        o.index_dir = env.dir() + "/idx";
        o.checkpoint_size = CHECKPOINT_SIZE;
        o.parallelism = 2;
        o.aggregation = config;
        dstd::Vector<dstd::String> paths;
        for (const auto& f : files) paths.emplace_back(f);
        auto ix = dftu_utils_test::ok(index::Indexer::open(
            dstd::Vector<dstd::String>(paths.begin(), paths.end()), o));
        dftu_utils_test::ok(ix.build());
        const dstd::String index_path = dstd::String(
            trace::internal::determine_index_path(paths[0], o.index_dir));

        const dlio::AggregatedTraces a = dftu_utils_test::ok(
            dlio::load_aggregated_traces(index_path, files, config));
        REQUIRE(a.any_data);
        CHECK(a.chunks_reread == 0);
        CHECK(a.quantiles_available);
        CHECK(a.rank_count == FILES);
        CHECK(a.trace_fetch_block_metrics.sample_count ==
              static_cast<std::uint64_t>(FILES * EVENTS_PER_NAME));
        CHECK(a.trace_fetch_block_metrics.accumulated_time ==
              doctest::Approx(static_cast<double>(dur_sum) * 1e-6));

        const dlio::AggregatedTraces b = dftu_utils_test::ok(
            dlio::load_aggregated_traces(index_path, files, config));
        CHECK(a.computation_times == b.computation_times);
        CHECK(a.preprocess_times == b.preprocess_times);
    }
}
