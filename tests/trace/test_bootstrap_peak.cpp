#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/index_fold_driver.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>
#include <testing_utilities.h>
#include <trace/bootstrap_test_common.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
namespace dataframe = dftracer::utils::dataframe;
namespace build = dftracer::utils::index::build;
using bootstrap_test::count_by_name;
using bootstrap_test::counts;

namespace {

constexpr std::size_t PEAK_WORKERS = 8;
constexpr std::size_t FEW_WORKERS = 2;
// Small, so each worker fills its staged index buffer within a few files and
// 64 files hold as many buffered bytes as 256.
constexpr std::size_t SINK_LIMIT = std::size_t{256} << 10;
// Small, so the blocks RocksDB caches as each file's index is read do not
// grow the peak with the file count; the store, not the bootstrap, owns them.
constexpr std::size_t BLOCK_CACHE_LIMIT = std::size_t{1} << 20;
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

constexpr const char* FILES_ENV = "DFTRACER_UTILS_TEST_BOOTSTRAP_FILES";

struct BootstrapRun {
    std::size_t peak = 0;
    std::int64_t events = 0;
};

// One bootstrap of `files` big files in a new process of this binary.
BootstrapRun run_bootstrap(int files) {
    ::setenv(FILES_ENV, std::to_string(files).c_str(), 1);
    const auto child = fresh_process::run("one bootstrap of big files");
    ::unsetenv(FILES_ENV);
    REQUIRE(child.ok());
    BootstrapRun out;
    const auto at = child.output.find("PEAK ");
    REQUIRE(at != std::string::npos);
    unsigned long long peak = 0;
    long long events = 0;
    REQUIRE(std::sscanf(child.output.c_str() + at, "PEAK %llu EVENTS %lld",
                        &peak, &events) == 2);
    out.peak = static_cast<std::size_t>(peak);
    out.events = events;
    return out;
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

    // Run by the case below in a new process, so each measured run starts
    // from the same empty process: in one process, what an earlier run left
    // behind moves the next run's peak by about 10 MB either way.
    TEST_CASE("one bootstrap of big files" * doctest::skip()) {
        fresh_process::announce();
        const auto n = Env::get<int>(FILES_ENV);
        REQUIRE(n.has_value());
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < *n; ++i)
            files.push_back(write_big_trace(env.get_dir(), i));
        build::g_max_sink_bytes.store(SINK_LIMIT);
        index::store::g_block_cache_bytes.store(BLOCK_CACHE_LIMIT);
        set_default_runtime(std::make_shared<Runtime>(PEAK_WORKERS));
        dataframe::DataFrame df;
        const auto peak = peak_live(
            [&] { df = count_by_name(files, env.get_dir() + "/idx"); });
        set_default_runtime(nullptr);
        std::fprintf(stderr, "PEAK %zu EVENTS %lld\n", peak,
                     static_cast<long long>(total_of(df)));
    }

    TEST_CASE("big files peak the same for 64 files and for 256") {
        const auto small = run_bootstrap(FEW_FILES);
        const auto large = run_bootstrap(MANY_FILES);
        MESSAGE("peak live bytes: 64 files " << small.peak << ", 256 files "
                                             << large.peak);

        const auto gap = large.peak > small.peak ? large.peak - small.peak
                                                 : small.peak - large.peak;
        CHECK(small.peak > 0);
        CHECK(gap < PER_WORKER_BUDGET);
        CHECK(large.peak < PEAK_BUDGET);
        CHECK(small.events == FEW_FILES * BIG_EVENTS);
        CHECK(large.events == MANY_FILES * BIG_EVENTS);
    }
}
