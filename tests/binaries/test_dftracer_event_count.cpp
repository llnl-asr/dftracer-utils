// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace dftracer::utils;

// ============================================================================
// Helpers
// ============================================================================

namespace {

dstd::String create_pfw_gz(dftu_utils_test::TestEnvironment& env,
                           int event_count, int id) {
    auto trace_gz = env.create_dft_test_gzip_file(event_count);
    if (trace_gz.empty()) return "";

    dstd::String pfw_path =
        env.dir() + "/trace_" + dstd::to_string(id) + ".pfw.gz";
    REQUIRE(dstd::fs::rename(trace_gz, pfw_path).has_value());
    return pfw_path;
}

dstd::String find_event_count_binary() {
    return dftu_utils_test::find_binary_by_name(
        "DFTRACER_UTILS_EVENT_COUNT_PATH", "dftracer_event_count");
}

// Run binary, capture stdout, return it as a string.
dstd::String run_event_count_capture(const dstd::String& binary,
                                     const dstd::Vector<dstd::String>& args) {
    return dftu_utils_test::run_process_capture(binary, args);
}

dstd::String find_index_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_UTILS_INDEX_PATH",
                                                "dftracer_index");
}

dstd::String event_line(int id, const char* ph) {
    return R"({"id":)" + dstd::to_string(id) + R"(,"pid":1,"tid":1,)" +
           R"("name":"read","cat":"POSIX","ph":")" + ph + R"(","ts":)" +
           dstd::to_string(1000 + id) + R"(,"dur":5,"args":{"ret":1}})";
}

// Lines the index counts under the dftracer decoder: 4 X and 2 C, one X of
// them a line the DOM parser refuses for a number too large for a double and an
// invalid escape. It skips metadata, two cropped lines (one of them refused by
// the DOM parser and cut after a nested "}"), the blank line, the
// brackets and the last event, which has no trailing newline. With
// `no_phase_line` the file has an object with no "ph", so schema detection
// picks the path decoder, which counts every object line, metadata included:
// 9 in all.
dstd::String mixed_fixture(bool no_phase_line) {
    dstd::String text = "[\n";
    text += event_line(1, "X") + "\n";
    text += event_line(2, "C") + "\n";
    text += event_line(3, "M") + "\n";
    text += event_line(4, "X") + "\n";
    if (no_phase_line)
        text += R"({"id":5,"name":"no_phase","cat":"POSIX"})"
                "\n";
    text += R"({"id":6,"name":"broken","ph":"X")"
            "\n";
    text += R"({"id":12,"name":"cut","ph":"X","a":1e400,"b":{"c":1})"
            "\n";
    text += "\n";
    text += event_line(7, "C") + "\n";
    text += event_line(8, "M") + "\n";
    text += event_line(9, "X") + "\n";
    text += R"({"id":11,"pid":1,"tid":1,"name":"odd","cat":"POSIX","ph":"X",)"
            R"("ts":1,"dur":1,"args":{"ret":1e400,"s":"a\q"}})"
            "\n";
    text += "]\n";
    text += event_line(10, "X");
    return text;
}

constexpr int DFTRACER_UTILS_FIXTURE_EVENTS = 6;
constexpr int PATH_FIXTURE_EVENTS = 9;

bool dir_has_dftindex(const dstd::String& dir) {
    for (const auto& entry : dftu_utils_test::walk_dir(dir)) {
        if (entry.path.view().filename() == ".dftindex") return true;
    }
    return false;
}

// Extract the last non-empty line from a string and parse it as int.
// Returns -1 on parse failure.
int parse_event_count(const dstd::String& output) {
    dstd::String last;
    dftu_utils_test::LineStream iss(output);
    dstd::String line;
    while (iss.next(line)) {
        auto start = line.find_first_not_of(" \t\r\n");
        if (start != dstd::String::npos) last = line.substr(start);
    }
    const auto end = last.find_last_not_of(" \t\r\n");
    const auto count =
        dstd::from_chars<int>(dstd::StringView(last).substr(0, end + 1));
    return count.has_value() ? *count : -1;
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerEventCount") {
    TEST_CASE("binary exists") {
        auto binary = find_event_count_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_event_count binary not found, skipping. "
                "Set DFTRACER_UTILS_EVENT_COUNT_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("count events single file") {
        auto binary = find_event_count_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_event_count binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        auto output = run_event_count_capture(binary, {env.dir(), "-f"});
        int count = parse_event_count(output);
        CHECK(count == 50);
    }

    TEST_CASE("count events multiple files") {
        auto binary = find_event_count_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_event_count binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        REQUIRE(!create_pfw_gz(env, 10, 0).empty());
        REQUIRE(!create_pfw_gz(env, 20, 1).empty());
        REQUIRE(!create_pfw_gz(env, 30, 2).empty());

        auto output = run_event_count_capture(binary, {env.dir(), "-f"});
        int count = parse_event_count(output);
        CHECK(count == 60);
    }

    TEST_CASE("empty directory returns zero or non-zero") {
        auto binary = find_event_count_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_event_count binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        // No trace files -- binary either exits non-zero or prints 0.
        auto output = run_event_count_capture(binary, {env.dir(), "-f"});
        int count = parse_event_count(output);
        // Either the binary reports 0 events or fails; both are acceptable.
        CHECK((count == 0 || count == -1));
    }

    TEST_CASE("force flag rebuilds index") {
        auto binary = find_event_count_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_event_count binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 25, 0);
        REQUIRE(!f.empty());

        // Both runs stream, since --force ignores any index.
        auto out1 = run_event_count_capture(binary, {env.dir(), "-f"});
        int count1 = parse_event_count(out1);
        CHECK(count1 == 25);

        auto out2 = run_event_count_capture(binary, {env.dir(), "-f"});
        int count2 = parse_event_count(out2);
        CHECK(count2 == 25);
    }

    TEST_CASE("executor threads flag accepted") {
        auto binary = find_event_count_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_event_count binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 15, 0);
        REQUIRE(!f.empty());

        auto output = run_event_count_capture(
            binary, {env.dir(), "-f", "--executor-threads", "2"});
        int count = parse_event_count(output);
        CHECK(count == 15);
    }

    TEST_CASE("custom index dir") {
        auto binary = find_event_count_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_event_count binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 20, 0);
        REQUIRE(!f.empty());

        dstd::String idx_dir = env.dir() + "/idx";
        REQUIRE(dstd::fs::create_directories(idx_dir).has_value());

        auto output = run_event_count_capture(
            binary, {env.dir(), "-f", "--index-dir", idx_dir});
        int count = parse_event_count(output);
        CHECK(count == 20);
    }

    TEST_CASE("cold count matches the index and writes nothing") {
        auto binary = find_event_count_binary();
        auto index_binary = find_index_binary();
        if (binary.empty() || index_binary.empty()) {
            MESSAGE("dftracer binaries not found, skipping.");
            return;
        }

        for (const bool no_phase_line : {false, true}) {
            const int expected = no_phase_line ? PATH_FIXTURE_EVENTS
                                               : DFTRACER_UTILS_FIXTURE_EVENTS;
            dftu_utils_test::ScopedTestDir dir("event_count_mixed");
            dftu_utils_test::write_gz_trace(dir.file("mixed.pfw.gz"),
                                            mixed_fixture(no_phase_line));
            const dstd::String idx_dir = dir.file("idx");

            auto cold = run_event_count_capture(
                binary, {dir.str(), "--index-dir", idx_dir});
            CHECK(parse_event_count(cold) == expected);
            const auto warned = dftu_utils_test::run_process_capture(
                binary, {dir.str(), "--index-dir", idx_dir}, true);
            CHECK(warned.find("2 line(s) could not be read in full") !=
                  dstd::String::npos);
            CHECK(!dstd::fs::exists(idx_dir).value());
            CHECK(!dir_has_dftindex(dir.str()));

            int exit_code = -1;
            dftu_utils_test::run_process_capture(
                index_binary, {dir.str(), "--index-dir", idx_dir}, false,
                &exit_code);
            REQUIRE(exit_code == 0);
            REQUIRE(dstd::fs::exists(idx_dir).value());

            auto warm = run_event_count_capture(
                binary, {dir.str(), "--index-dir", idx_dir});
            CHECK(parse_event_count(warm) == expected);

            auto forced = run_event_count_capture(
                binary, {dir.str(), "--index-dir", idx_dir, "-f"});
            CHECK(parse_event_count(forced) == expected);
        }
    }

    TEST_CASE("cold count without an index dir leaves no index beside inputs") {
        auto binary = find_event_count_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_event_count binary not found, skipping.");
            return;
        }

        dftu_utils_test::ScopedTestDir dir("event_count_cold");
        dftu_utils_test::write_gz_trace(dir.file("mixed.pfw.gz"),
                                        mixed_fixture(false));
        auto out = run_event_count_capture(binary, {dir.str()});
        CHECK(parse_event_count(out) == DFTRACER_UTILS_FIXTURE_EVENTS);
        CHECK(!dir_has_dftindex(dir.str()));
    }

    TEST_CASE("multi-member gzip counts every member") {
        auto binary = find_event_count_binary();
        auto index_binary = find_index_binary();
        if (binary.empty() || index_binary.empty()) {
            MESSAGE("dftracer binaries not found, skipping.");
            return;
        }

        dftu_utils_test::ScopedTestDir dir("event_count_members");
        constexpr int EVENTS = 300;
        const dstd::String plain = dir.file("plain.txt");
        {
            dftu_utils_test::FileOut ofs(plain);
            ofs << "[\n";
            for (int i = 1; i <= EVENTS; ++i) ofs << event_line(i, "X") << "\n";
        }
        const dstd::String gz = dir.file("members.pfw.gz");
        REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                                   4096));
        (void)dstd::fs::remove(plain);
        const dstd::String idx_dir = dir.file("idx");

        auto cold = run_event_count_capture(
            binary, {dir.str(), "--index-dir", idx_dir});
        CHECK(parse_event_count(cold) == EVENTS);

        int exit_code = -1;
        dftu_utils_test::run_process_capture(
            index_binary, {dir.str(), "--index-dir", idx_dir}, false,
            &exit_code);
        REQUIRE(exit_code == 0);
        auto warm = run_event_count_capture(
            binary, {dir.str(), "--index-dir", idx_dir});
        CHECK(parse_event_count(warm) == EVENTS);
    }
}
