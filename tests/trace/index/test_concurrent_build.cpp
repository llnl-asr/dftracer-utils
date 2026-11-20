// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/build/batch_builder.h>
#include <dftracer/utils/trace/index/build/resolve_and_build.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <doctest/doctest.h>
#include <stdlib.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::index;

namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
int g_arrived = 0;
bool g_overlapped = false;

// The first build to parse its file waits here for a second one to parse a
// file, as long as the second one can. A second build that waits for the first
// on the lock does not come, so the first goes on after the delay.
void hold_first_build() {
    std::unique_lock<std::mutex> lock(g_mutex);
    const int mine = ++g_arrived;
    g_cv.notify_all();
    if (mine == 1) {
        g_cv.wait_for(lock, std::chrono::seconds(1),
                      [] { return g_arrived >= 2; });
        g_overlapped = g_arrived >= 2;
    }
}

// Runs `first`, and `second` once the first is held at the probe. Both must
// succeed, and the second must not have reached the probe while the first was
// held.
template <class First, class Second>
void run_overlapping(First first, Second second, dstd::StringView what) {
    INFO(doctest::String(what.data(), static_cast<unsigned>(what.size())));
    g_arrived = 0;
    g_overlapped = false;
    trace::index::build::g_before_write_probe.store(&hold_first_build);
    bool first_ok = false;
    bool second_ok = false;
    dstd::Thread first_thread([&] { first_ok = first(); });
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        REQUIRE(g_cv.wait_for(lock, std::chrono::seconds(30),
                              [] { return g_arrived >= 1; }));
    }
    dstd::Thread second_thread([&] { second_ok = second(); });
    first_thread.join();
    second_thread.join();
    trace::index::build::g_before_write_probe.store(nullptr);
    CHECK(first_ok);
    CHECK(second_ok);
    CHECK_FALSE(g_overlapped);
}

bool build_files(Runtime& rt, dstd::StringView file) {
    IndexerOptions options;
    options.runtime = &rt;
    auto opened = Indexer::open({dstd::String(file)}, options);
    if (!opened) return false;
    auto built = opened->build();
    return built.has_value() && built->needs_work.empty();
}

bool build_directory(Runtime& rt, dstd::StringView directory) {
    trace::index::build::ResolveAndBuildInput in;
    in.directory = dstd::String(directory);
    in.require_checkpoints = true;
    return rt
        .submit(trace::index::build::resolve_and_build_index(std::move(in)))
        .get()
        .has_value();
}

}  // namespace

TEST_SUITE("trace/index/concurrent_build") {
    TEST_CASE("a build that starts while another builds the same index waits") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = env.create_dft_test_gzip_file(2000);
        const dstd::String path{dstd::StringView(trace)};
        run_overlapping([&] { return build_files(rt, path); },
                        [&] { return build_files(rt, path); }, "same spelling");
        auto ix = dftu_utils_test::ok(Indexer::open({path}, [&] {
            IndexerOptions o;
            o.runtime = &rt;
            return o;
        }()));
        CHECK(dftu_utils_test::ok(ix.status()).needs_work.empty());
        rt.shutdown();
    }

    TEST_CASE("two builds of one directory input wait for each other") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = env.create_dft_test_gzip_file(2000);
        const dstd::String dir{
            dstd::Path(dstd::StringView(trace)).view().parent().str()};
        run_overlapping([&] { return build_directory(rt, dir); },
                        [&] { return build_directory(rt, dir); }, "directory");
        rt.shutdown();
    }

    TEST_CASE("two spellings of one index root share one lock") {
        Runtime rt(4);
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = env.create_dft_test_gzip_file(2000);
        const dstd::PathView file{dstd::StringView(trace)};
        const dstd::String parent{file.parent().str()};
        const dstd::String name{file.filename()};
        const dstd::String plain{file.str()};
        const dstd::String dotted = parent + "/./" + name;
        REQUIRE(plain != dotted);
        run_overlapping([&] { return build_files(rt, plain); },
                        [&] { return build_files(rt, dotted); }, "dotted");

        // Another file, by a path relative to the working directory and by its
        // absolute path.
        const auto other = env.create_dft_test_gzip_file(2100);
        // The working directory is a real path, so the relative and the
        // absolute spelling name one directory for the store too.
        char* real =
            ::realpath(dstd::String(dstd::StringView(other)).c_str(), nullptr);
        REQUIRE(real != nullptr);
        const dstd::String absolute{real};
        ::free(real);
        const dstd::PathView other_file{absolute};
        const dstd::String above{other_file.parent().parent().str()};
        const dstd::String relative =
            dstd::String(other_file.parent().filename()) + "/" +
            dstd::String(other_file.filename());
        char* here = ::getcwd(nullptr, 0);
        REQUIRE(here != nullptr);
        REQUIRE(::chdir(above.c_str()) == 0);
        run_overlapping([&] { return build_files(rt, relative); },
                        [&] { return build_files(rt, absolute); }, "relative");
        REQUIRE(::chdir(here) == 0);
        ::free(here);
        rt.shutdown();
    }
}
