#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/dlio/trace_loader.h>
#include <doctest/doctest.h>
#include <rocksdb_exit_listener.h>
#include <testing_utilities.h>

#include <cstdint>
#include <fstream>
#include <string>

using namespace dftracer::utils;
namespace agg = dftracer::utils::index::schemas::dft::agg;
namespace dlio = dftracer::utils::utilities::dlio;

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
std::string write_rank(const std::string& dir, int file,
                       std::uint64_t& dur_sum) {
    const std::string pfw = dir + "/rank_" + std::to_string(file) + ".pfw";
    {
        std::ofstream ofs(pfw);
        const std::uint64_t pid = 1000 + static_cast<std::uint64_t>(file);
        std::uint64_t ts = BASE_TS;
        std::uint64_t id = 1;
        ofs << "[\n";
        for (int i = 0; i < EVENTS_PER_NAME; ++i) {
            for (const auto& c : COMPONENTS) {
                const std::uint64_t dur =
                    c.base_dur + static_cast<std::uint64_t>((i * 7) % 90);
                if (std::string(c.name) == "fetch.block") dur_sum += dur;
                ofs << R"({"id":)" << id++ << R"(,"pid":)" << pid
                    << R"(,"tid":)" << pid << R"(,"name":")" << c.name
                    << R"(","cat":")" << c.cat << R"(","ph":"X","ts":)" << ts
                    << R"(,"dur":)" << dur << R"(,"args":{}})" << "\n";
                ts += dur;
            }
        }
        ofs << "]\n";
    }
    const std::string gz = pfw + ".gz";
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
            files.emplace_back(write_rank(env.get_dir(), i, dur_sum));

        agg::AggregationConfig config;
        config.time_interval_us = 5'000'000;
        config.compute_statistics = true;
        config.compute_percentiles = true;
        config.track_process_parents = true;
        config.track_default_args = true;

        index::IndexerOptions o;
        o.index_dir = env.get_dir() + "/idx";
        o.checkpoint_size = CHECKPOINT_SIZE;
        o.parallelism = 2;
        o.aggregation = config;
        std::vector<std::string> paths;
        for (const auto& f : files) paths.emplace_back(f);
        auto ix = index::Indexer::open(paths, o);
        ix.build();
        const std::string index_path =
            trace::internal::determine_index_path(paths[0], o.index_dir);

        const dlio::AggregatedTraces a =
            dlio::load_aggregated_traces(index_path, files, config);
        REQUIRE(a.any_data);
        CHECK(a.chunks_reread == 0);
        CHECK(a.quantiles_available);
        CHECK(a.num_ranks == FILES);
        CHECK(a.trace_fetch_block_metrics.num_samples ==
              static_cast<std::uint64_t>(FILES * EVENTS_PER_NAME));
        CHECK(a.trace_fetch_block_metrics.accumulated_time ==
              doctest::Approx(static_cast<double>(dur_sum) * 1e-6));

        const dlio::AggregatedTraces b =
            dlio::load_aggregated_traces(index_path, files, config);
        CHECK(a.computation_times == b.computation_times);
        CHECK(a.preprocess_times == b.preprocess_times);
    }
}
