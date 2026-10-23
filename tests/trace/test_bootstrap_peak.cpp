#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/index_fold_driver.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "bootstrap_test_common.h"

using namespace dftracer::utils;
namespace dataframe = dftracer::utils::dataframe;
namespace build = dftracer::utils::index::build;
using bootstrap_test::count_by_name;
using bootstrap_test::counts;

namespace {

constexpr std::size_t PEAK_WORKERS = 8;
constexpr std::size_t FEW_WORKERS = 2;
constexpr std::size_t SINK_LIMIT = std::size_t{2} << 20;
// What a worker may hold to parse a file, whatever the file.
constexpr std::size_t PER_WORKER_BUDGET = std::size_t{24} << 20;
// What the whole bootstrap holds at PEAK_WORKERS, whatever the files.
constexpr std::size_t PEAK_BUDGET = std::size_t{128} << 20;
constexpr int PEAK_FILES = 16;
constexpr int MANY_FILES = 256;
constexpr int FEW_FILES = 64;
// A file of about 1.3 MiB inflated is more than one parse piece.
constexpr int BIG_EVENTS = 2000;
constexpr std::size_t NOTE_BYTES = 600;

std::string big_trace_text() {
    std::ostringstream out;
    const std::string note(NOTE_BYTES, 'x');
    out << "[\n";
    for (int i = 0; i < BIG_EVENTS; ++i) {
        out << R"({"id":)" << i << R"(,"name":"op)" << i % 40
            << R"(","cat":"POSIX","pid":)" << 1 + i % 7 << R"(,"tid":)"
            << 10 + i % 5 << R"(,"ph":"X","ts":)" << 1000 + i * 10
            << R"(,"dur":)" << 3 + i % 50 << R"(,"args":{"fname":"/data/d)"
            << i % 9 << "/f" << (i * 31) % 700 << R"(","size":)"
            << (i * 17) % 4096;
        out << R"(,"note":")" << note << R"("}})" << "\n";
    }
    out << "]\n";
    return out.str();
}

// A file of more than one parse piece: the parse buffers of a file in flight
// are what the peak tests measure. Every file has the same text, so a
// run spends its time in the bootstrap and not in making the input.
std::string write_big_trace(const std::string& dir, int n) {
    static const std::string text = big_trace_text();
    char name[32];
    std::snprintf(name, sizeof(name), "/b%04d.pfw.gz", n);
    return dftu_utils_test::write_gz_trace(dir + name, text);
}

// The most live bytes above the start that the memory manager reports each
// time a worker finishes parsing a piece, while `run` goes.
template <class Fn>
std::size_t peak_live(Fn&& run) {
    build::g_parse_peak_live.store(0);
    build::g_probe_parse.store(true);
    const auto base = memory::Manager::get().stats().live;
    run();
    build::g_probe_parse.store(false);
    const auto top = build::g_parse_peak_live.load();
    return top > base ? top - base : 0;
}

std::int64_t total_of(const dataframe::DataFrame& df) {
    std::int64_t total = 0;
    for (const auto& [name, n] : counts(df)) total += n;
    return total;
}

}  // namespace

TEST_SUITE("BootstrapPeak") {
    TEST_CASE("the peak grows by a fixed budget for each extra worker") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < PEAK_FILES; ++i)
            files.push_back(write_big_trace(env.get_dir(), i));

        const auto limit = build::g_max_sink_bytes.exchange(SINK_LIMIT);
        dataframe::DataFrame few_df, many_df;
        set_default_runtime(std::make_shared<Runtime>(FEW_WORKERS));
        const auto few_peak = peak_live(
            [&] { few_df = count_by_name(files, env.get_dir() + "/idx_few"); });
        set_default_runtime(nullptr);
        set_default_runtime(std::make_shared<Runtime>(PEAK_WORKERS));
        const auto many_peak = peak_live([&] {
            many_df = count_by_name(files, env.get_dir() + "/idx_many");
        });
        build::g_max_sink_bytes.store(limit);
        MESSAGE("peak live bytes: " << FEW_WORKERS << " workers " << few_peak
                                    << ", " << PEAK_WORKERS << " workers "
                                    << many_peak);

        CHECK(few_peak > 0);
        CHECK(many_peak <=
              few_peak + (PEAK_WORKERS - FEW_WORKERS) * PER_WORKER_BUDGET);
        CHECK(counts(few_df) == counts(many_df));
        CHECK(total_of(many_df) == PEAK_FILES * BIG_EVENTS);
        set_default_runtime(nullptr);
        CHECK_FALSE(memory::Manager::get().leaked());
    }

    TEST_CASE("big files peak the same for 64 files and for 256") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < MANY_FILES; ++i)
            files.push_back(write_big_trace(env.get_dir(), i));
        const std::vector<std::string> few(files.begin(),
                                           files.begin() + FEW_FILES);

        // A fresh runtime for each run, so what a worker thread keeps does
        // not count against the next run's start.
        const auto limit = build::g_max_sink_bytes.exchange(SINK_LIMIT);
        dataframe::DataFrame small_df, large_df;
        set_default_runtime(std::make_shared<Runtime>(PEAK_WORKERS));
        const auto small_peak = peak_live(
            [&] { small_df = count_by_name(few, env.get_dir() + "/idx64"); });
        set_default_runtime(nullptr);
        set_default_runtime(std::make_shared<Runtime>(PEAK_WORKERS));
        const auto large_peak = peak_live([&] {
            large_df = count_by_name(files, env.get_dir() + "/idx256");
        });
        build::g_max_sink_bytes.store(limit);
        MESSAGE("peak live bytes: 64 files " << small_peak << ", 256 files "
                                             << large_peak);

        const auto gap = large_peak > small_peak ? large_peak - small_peak
                                                 : small_peak - large_peak;
        CHECK(small_peak > 0);
        CHECK(gap < PER_WORKER_BUDGET);
        CHECK(large_peak < PEAK_BUDGET);
        CHECK(total_of(large_df) == MANY_FILES * BIG_EVENTS);
        set_default_runtime(nullptr);
        CHECK_FALSE(memory::Manager::get().leaked());
    }
}
