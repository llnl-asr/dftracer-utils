// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace dftracer::utils;

namespace {

constexpr int FILE_COUNT = 4;

struct IndexRun {
    int exit_code = -1;
    long indexed = -1;
    long skipped = -1;
};

long field(const dstd::String& out, const dstd::String& label) {
    const auto at = out.find(label);
    if (at == dstd::String::npos) return -1;
    return std::strtol(out.c_str() + at + label.size(), nullptr, 10);
}

IndexRun run_index(const dstd::String& dir, bool force) {
    const dstd::String binary = DFTRACER_UTILS_TEST_INDEX_BINARY;
    REQUIRE_MESSAGE(::access(binary.c_str(), X_OK) == 0,
                    binary << " is not built");
    dftu_utils_test::set_test_library_path(binary);
    dstd::Vector<dstd::String> args{dir};
    if (force) args.push_back("--force");
    IndexRun run;
    const dstd::String out = dftu_utils_test::run_process_capture(
        binary, args, false, &run.exit_code);
    run.indexed = field(out, "Files processed: ");
    run.skipped = field(out, "Files skipped: ");
    return run;
}

dstd::Vector<dstd::String> index_files(const dstd::String& dir) {
    dstd::Vector<dstd::String> files;
    const dstd::Path root = dstd::Path(dir) / ".dftindex";
    if (!dstd::fs::exists(root).value()) return files;
    for (const auto& entry : dftu_utils_test::walk_dir(root))
        if ((entry.kind == dstd::fs::FileKind::File))
            files.push_back(dstd::String(entry.path.str()) + " " +
                            dstd::to_string(entry.size));
    std::sort(files.begin(), files.end());
    return files;
}

dstd::String trace_path(const dstd::String& dir, int id) {
    return dir + "/trace_" + dstd::to_string(id) + ".pfw.gz";
}

void write_trace(dftu_utils_test::TestEnvironment& env, int id,
                 int num_events) {
    auto made = env.create_dft_test_gzip_file(num_events);
    REQUIRE(!made.empty());
    REQUIRE(dstd::fs::rename(made, trace_path(env.get_dir(), id)).has_value());
}

}  // namespace

TEST_CASE("a second dftracer_index skips files that did not change") {
    dftu_utils_test::TestEnvironment env(50);
    REQUIRE(env.is_valid());
    const dstd::String dir = env.get_dir();
    for (int i = 0; i < FILE_COUNT; ++i) write_trace(env, i, 50 + i);

    const auto first = run_index(dir, false);
    REQUIRE(first.exit_code == 0);
    CHECK(first.indexed == FILE_COUNT);
    CHECK(first.skipped == 0);
    const auto stored = index_files(dir);

    SUBCASE("unchanged files are skipped and nothing is written") {
        const auto second = run_index(dir, false);
        CHECK(second.exit_code == 0);
        CHECK(second.indexed == 0);
        CHECK(second.skipped == FILE_COUNT);
        CHECK_FALSE(stored.empty());
        CHECK(index_files(dir) == stored);
    }

    SUBCASE("a file with other content is indexed again") {
        write_trace(env, 1, 200);
        const auto second = run_index(dir, false);
        CHECK(second.exit_code == 0);
        CHECK(second.indexed == 1);
        CHECK(second.skipped == FILE_COUNT - 1);
    }

    SUBCASE("a file with a newer mtime is indexed again") {
        const dstd::String path = trace_path(dir, 2);
        REQUIRE(dstd::fs::set_mtime_ns(
                    path, dstd::fs::mtime_ns(path).value() + 60'000'000'000ULL)
                    .has_value());
        const auto second = run_index(dir, false);
        CHECK(second.exit_code == 0);
        CHECK(second.indexed == 1);
        CHECK(second.skipped == FILE_COUNT - 1);
    }

    SUBCASE("force indexes every file") {
        const auto second = run_index(dir, true);
        CHECK(second.exit_code == 0);
        CHECK(second.indexed == FILE_COUNT);
        CHECK(second.skipped == 0);
    }
}
