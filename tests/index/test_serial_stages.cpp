// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <sys/stat.h>
#include <testing_utilities.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace dftracer::utils;
namespace build = dftracer::utils::index::build;
namespace istore = dftracer::utils::index::store;
namespace scan = dftracer::utils::utilities::filesystem;
namespace views = dftracer::utils::trace::views;

namespace {

constexpr int SCAN_FILES = 200;
constexpr int NESTED_FILES = 10;
constexpr std::size_t WORKERS = 8;
constexpr int VIEW_FILES = 12;
constexpr std::size_t VIEW_WORKERS = 4;

void write_sized_file(const std::string& path, std::size_t bytes) {
    std::ofstream out(path.c_str(), std::ios::binary);
    out << std::string(bytes, 'x');
}

std::string write_small_trace(const std::string& dir, int n) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < 40; ++i) {
        out << R"({"id":)" << i << R"(,"name":"read","cat":"POSIX","pid":1,)"
            << R"("tid":1,"ph":"X","ts":)" << 1000 + i * 10
            << R"(,"dur":3,"args":{"fname":"/data/f)" << n << R"("}})"
            << "\n";
    }
    out << "]\n";
    char name[32];
    std::snprintf(name, sizeof(name), "/t%04d.pfw.gz", n);
    return dftu_utils_test::write_gz_trace(dir + name, out.str());
}

std::vector<scan::FileEntry> scan_dir(const std::string& dir) {
    std::vector<scan::FileEntry> entries;
    Runtime rt(4);
    auto task = run_coro_scope(
        rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
            scan::PatternDirectoryScannerUtility scanner;
            const scan::PatternDirectoryScannerUtilityInput input(
                dir, std::vector<std::string>{".pfw"}, true);
            entries = co_await scanner(scope, input);
        });
    rt.submit(std::move(task), "scan-dir").wait();
    rt.shutdown();
    return entries;
}

// The scan as the CLIs run it, from a coroutine that get() drives on the main
// thread.
coro::CoroTask<std::vector<scan::FileEntry>> cli_scan_task(std::string dir) {
    auto entries = scan::scan_on_pool<scan::PatternDirectoryScannerUtility>(
        WORKERS, 2,
        scan::PatternDirectoryScannerUtilityInput(
            std::move(dir), std::vector<std::string>{".pfw"}, true, false));
    co_return entries;
}

std::vector<scan::FileEntry> cli_scan(const std::string& dir) {
    return cli_scan_task(dir).get();
}

std::vector<std::string> write_view_traces(const std::string& dir, int count) {
    std::vector<std::string> files;
    for (int i = 0; i < count; ++i) files.push_back(write_small_trace(dir, i));
    return files;
}

std::string write_corrupt_trace(const std::string& dir, int n) {
    char name[32];
    std::snprintf(name, sizeof(name), "/t%04d.pfw.gz", n);
    const auto path = dir + name;
    std::ofstream out(path.c_str(), std::ios::binary);
    out << std::string(4096, 'x');
    return path;
}

std::string view_index_root(const std::string& file,
                            const std::string& index_dir) {
    return trace::internal::determine_index_path(file, index_dir);
}

void cold_count(const std::vector<std::string>& files,
                const std::string& index_dir) {
    std::vector<views::ViewFile> view_files;
    for (const auto& f : files) {
        views::ViewFile vf;
        vf.file_path = f;
        vf.index_path = view_index_root(f, index_dir);
        view_files.push_back(std::move(vf));
    }
    const auto view = views::View::from_files(std::move(view_files))
                          .group_by({views::GroupKey::name()})
                          .agg({{views::AggOp::Count, "", "n"}});
    default_runtime().run_blocking("serial-stages-view",
                                   [&](CoroScope&) -> coro::CoroTask<void> {
                                       (void)co_await view.collect();
                                   });
}

// Written once and shared by the cases that read it.
struct ViewRun {
    ViewRun() : env(10) {
        files = write_view_traces(env.get_dir(), VIEW_FILES);
        configure_default_runtime({.threads = VIEW_WORKERS, .eager = true});
    }

    dftu_utils_test::TestEnvironment env;
    std::vector<std::string> files;
};

ViewRun& view_run() {
    static ViewRun run;
    return run;
}

}  // namespace

TEST_SUITE("SerialStages") {
    TEST_CASE("a directory scan stats its files in parallel batches") {
        dftu_utils_test::TestEnvironment env(10);
        const std::string dir = env.get_dir() + "/scan";
        REQUIRE(dstd::fs::create_directories(dir + "/sub").has_value());
        for (int i = 0; i < SCAN_FILES; ++i)
            write_sized_file(dir + "/f" + std::to_string(i) + ".pfw",
                             static_cast<std::size_t>(i) * 7 + 1);
        for (int i = 0; i < NESTED_FILES; ++i)
            write_sized_file(dir + "/sub/n" + std::to_string(i) + ".pfw",
                             static_cast<std::size_t>(i) + 100);
        write_sized_file(dir + "/skip.txt", 5);

        std::vector<std::string> expected;
        for (const auto& e : dftu_utils_test::list_dir(dir))
            if ((e.kind == dstd::fs::FileKind::File) &&
                e.path.view().extension() == ".pfw")
                expected.push_back(std::string(e.path.str()));
        for (const auto& e : dftu_utils_test::list_dir(dir + "/sub"))
            expected.push_back(std::string(e.path.str()));

        const auto before = scan::scan_stat_batches().load();
        const auto entries = scan_dir(dir);
        const auto batches = scan::scan_stat_batches().load() - before;

        REQUIRE(entries.size() == expected.size());
        CHECK(entries.size() == SCAN_FILES + NESTED_FILES);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            CHECK(entries[i].path.str() == expected[i]);
            CHECK(entries[i].is_regular_file);
            CHECK(entries[i].size ==
                  dstd::fs::file_size(entries[i].path.c_str()).value());
            CHECK(entries[i].mtime > 0);
        }
        CHECK(batches > 2);
    }

    TEST_CASE("a CLI scan stats its files on the pool, not the main thread") {
        dftu_utils_test::TestEnvironment env(10);
        const std::string dir = env.get_dir() + "/cli_scan";
        REQUIRE(dstd::fs::create_directories(dir + "/a").has_value());
        REQUIRE(dstd::fs::create_directories(dir + "/b").has_value());
        for (int i = 0; i < SCAN_FILES; ++i)
            write_sized_file(
                dir + (i % 2 ? "/a/f" : "/b/f") + std::to_string(i) + ".pfw",
                static_cast<std::size_t>(i) + 1);

        scan::scan_stat_threads().clear();
        // A CLI drives its run coroutine with get() on the main thread, with
        // no executor running there but the main run loop.
        const auto entries = cli_scan(dir);

        CHECK(entries.size() == SCAN_FILES);
        CHECK(scan::scan_stat_threads().count() >= 1);
        CHECK_FALSE(
            scan::scan_stat_threads().contains(std::this_thread::get_id()));
    }

    TEST_CASE("a bootstrap that fails midway marks only complete files") {
        const auto& run = view_run();
        auto files = run.files;
        files.push_back(write_corrupt_trace(run.env.get_dir(), VIEW_FILES));
        const auto index_dir = run.env.get_dir() + "/failed";
        configure_default_runtime({.threads = 2, .eager = true});
        try {
            cold_count(files, index_dir);
        } catch (const std::exception&) {
        }

        const auto root = view_index_root(files.front(), index_dir);
        const auto snapshot = dftu_utils_test::index_snapshot(root);
        std::size_t marked = 0;
        for (const auto& f : files) {
            const auto file = dftu_utils_test::find_indexed_file(snapshot, f);
            if (!file) continue;
            ++marked;
            INFO("file " << f);
            CHECK(dftracer::utils::index::extension_current(
                snapshot, *file, istore::IndexExtension::Members));
            const auto meta = dftu_utils_test::index_take(
                dftracer::utils::index::file_metadata(snapshot, *file));
            REQUIRE(meta.has_value());
            CHECK(meta->num_lines > 0);
            CHECK_FALSE(dftu_utils_test::index_take(
                            dftracer::utils::index::members(snapshot, *file))
                            .empty());
            CHECK(dftu_utils_test::pruning_tier_current(snapshot, *file));
            CHECK(dftracer::utils::index::extension_state(
                      snapshot, *file, istore::IndexExtension::Profile)
                      .has_value());
        }
        MESSAGE("marked " << marked << " of " << files.size());
        REQUIRE(marked > 0);
        CHECK(marked < files.size());
        CHECK_FALSE(dftu_utils_test::find_indexed_file(snapshot, files.back())
                        .has_value());
    }
}
