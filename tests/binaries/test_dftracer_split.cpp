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

using namespace dftracer::utils;

// ============================================================================
// Helpers
// ============================================================================

namespace {

// Inputs live in their own folder: split reads every subfolder of -d, so an
// output folder inside it would be read back as input by the next run.
dstd::String input_dir(dftu_utils_test::TestEnvironment& env) {
    const dstd::String dir = env.get_dir() + "/in";
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    return dir;
}

dstd::String create_pfw_gz(dftu_utils_test::TestEnvironment& env,
                           int num_events, int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";

    dstd::String pfw_path =
        input_dir(env) + "/trace_" + dstd::to_string(id) + ".pfw.gz";
    REQUIRE(dstd::fs::rename(trace_gz, pfw_path).has_value());
    return pfw_path;
}

dstd::String find_split_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_UTILS_SPLIT_PATH",
                                                "dftracer_split");
}

int run_split(const dstd::String& binary,
              const dstd::Vector<dstd::String>& args) {
    return dftu_utils_test::run_process(binary, args);
}

// Count non-empty lines across all .pfw.gz files in a directory by
// decompressing each with zlib.
int count_gz_lines_in_dir(const dstd::String& dir) {
    int total = 0;
    for (const auto& entry : dftu_utils_test::list_dir(dir)) {
        if (!(entry.kind == dstd::fs::FileKind::File)) continue;
        const dstd::String p(entry.path.str());
        if (p.size() < 7 || p.substr(p.size() - 7) != ".pfw.gz") continue;

        gzFile gz = gzopen(p.c_str(), "rb");
        if (!gz) continue;

        char buf[4096];
        while (gzgets(gz, buf, sizeof(buf)) != nullptr) {
            dstd::String line(buf);
            // strip trailing newline
            while (!line.empty() &&
                   (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }
            if (!line.empty()) ++total;
        }
        gzclose(gz);
    }
    return total;
}

// Count .pfw.gz files in a directory.
int count_gz_files(const dstd::String& dir) {
    int n = 0;
    for (const auto& entry : dftu_utils_test::list_dir(dir)) {
        if (!(entry.kind == dstd::fs::FileKind::File)) continue;
        const dstd::String p(entry.path.str());
        if (p.size() >= 7 && p.substr(p.size() - 7) == ".pfw.gz") ++n;
    }
    return n;
}

// Count gzip members across all .pfw.gz output files (each member starts with
// the RFC 1952 magic 1F 8B 08).
int count_gzip_members(const dstd::String& dir) {
    int members = 0;
    for (const auto& entry : dftu_utils_test::list_dir(dir)) {
        if (!(entry.kind == dstd::fs::FileKind::File)) continue;
        const dstd::String p(entry.path.str());
        if (p.size() < 7 || p.substr(p.size() - 7) != ".pfw.gz") continue;
        const dstd::String buf = dftu_utils_test::read_text(p);
        for (std::size_t i = 0; i + 2 < buf.size(); ++i)
            if (static_cast<unsigned char>(buf[i]) == 0x1f &&
                static_cast<unsigned char>(buf[i + 1]) == 0x8b &&
                static_cast<unsigned char>(buf[i + 2]) == 0x08)
                ++members;
    }
    return members;
}

// True when the first line of every .pfw.gz file in the directory is an event.
bool all_files_start_with_event(const dstd::String& dir) {
    for (const auto& entry : dftu_utils_test::list_dir(dir)) {
        if (!(entry.kind == dstd::fs::FileKind::File)) continue;
        const dstd::String p(entry.path.str());
        if (p.size() < 7 || p.substr(p.size() - 7) != ".pfw.gz") continue;

        gzFile gz = gzopen(p.c_str(), "rb");
        if (!gz) return false;
        char buf[16] = {0};
        const bool got = gzgets(gz, buf, sizeof(buf)) != nullptr;
        gzclose(gz);
        if (!got || buf[0] != '{') return false;
    }
    return true;
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerSplit") {
    TEST_CASE("binary exists") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_split binary not found, skipping. "
                "Set DFTRACER_UTILS_SPLIT_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("basic split produces output files") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        dstd::String out_dir = env.get_dir() + "/split_out";
        int rc = run_split(
            binary, {input_dir(env), "-o", out_dir, "--disable-watchdog"});
        CHECK(rc == 0);
        REQUIRE(dstd::fs::exists(out_dir).value());
        CHECK(count_gz_files(out_dir) > 0);
        CHECK(all_files_start_with_event(out_dir));
    }

    TEST_CASE("checkpoint-size controls multi-member output") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        // Large enough to exceed a 1 MB uncompressed member.
        auto f = create_pfw_gz(env, 40000, 0);
        REQUIRE(!f.empty());

        // Small checkpoint (member) size over several MB of events -> many
        // members. checkpoint-size is the gzip member size.
        dstd::String mm = env.get_dir() + "/split_mm";
        int rc =
            run_split(binary, {input_dir(env), "-o", mm, "--checkpoint-size",
                               "1048576", "--disable-watchdog"});
        CHECK(rc == 0);
        REQUIRE(dstd::fs::exists(mm).value());
        CHECK(count_gzip_members(mm) >= 2);

        // A checkpoint larger than the data -> one member per output file.
        dstd::String sm = env.get_dir() + "/split_sm";
        rc = run_split(binary, {input_dir(env), "-o", sm, "--checkpoint-size",
                                "1073741824", "--disable-watchdog"});
        CHECK(rc == 0);
        REQUIRE(dstd::fs::exists(sm).value());
        CHECK(count_gzip_members(sm) == count_gz_files(sm));
    }

    TEST_CASE("a small chunk size rolls the output over to more files") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 40000, 0).empty());

        dstd::String one = env.get_dir() + "/split_one";
        int rc = run_split(binary,
                           {input_dir(env), "-o", one, "--disable-watchdog"});
        CHECK(rc == 0);
        REQUIRE(dstd::fs::exists(one).value());

        // 64 KB of gzip is a few MB of events; a chunk ends at a member end
        // past that, so 256 KB members give several chunk files.
        dstd::String many = env.get_dir() + "/split_many";
        rc = run_split(binary,
                       {input_dir(env), "-o", many, "--chunk-size", "64KB",
                        "--checkpoint-size", "262144", "--disable-watchdog"});
        CHECK(rc == 0);
        REQUIRE(dstd::fs::exists(many).value());

        CHECK(count_gz_files(many) > count_gz_files(one));
        CHECK(count_gz_lines_in_dir(many) == count_gz_lines_in_dir(one));
    }

    TEST_CASE("a chunk size without a unit is refused") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 1000, 0).empty());

        dftu_utils_test::set_test_library_path(binary);
        const dstd::String out = env.get_dir() + "/split_bare";
        int rc = -1;
        const dstd::String text = dftu_utils_test::run_process_capture(
            binary,
            {input_dir(env), "-o", out, "--chunk-size", "4",
             "--disable-watchdog"},
            true, &rc);
        INFO(text);
        CHECK(rc != 0);
        CHECK(text.find("--chunk-size") != dstd::String::npos);
        CHECK(text.find("a unit is required") != dstd::String::npos);
        CHECK(!dstd::fs::exists(out).value());
    }

    TEST_CASE("a tiny memory budget gives the same output") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        for (int id = 0; id < 4; ++id) {
            REQUIRE(!create_pfw_gz(env, 5000, id).empty());
        }

        dstd::String open_dir = env.get_dir() + "/split_open";
        int rc = run_split(binary, {input_dir(env), "-o", open_dir, "--verify",
                                    "--disable-watchdog"});
        CHECK(rc == 0);
        REQUIRE(dstd::fs::exists(open_dir).value());

        dstd::String tight_dir = env.get_dir() + "/split_tight";
        rc = run_split(
            binary, {input_dir(env), "-o", tight_dir, "--memory-budget", "1MB",
                     "--verify", "--disable-watchdog"});
        CHECK(rc == 0);
        REQUIRE(dstd::fs::exists(tight_dir).value());

        CHECK(count_gz_files(tight_dir) == count_gz_files(open_dir));
        CHECK(count_gz_lines_in_dir(tight_dir) ==
              count_gz_lines_in_dir(open_dir));
    }

    TEST_CASE("split with verify flag") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        REQUIRE(!create_pfw_gz(env, 50, 0).empty());
        REQUIRE(!create_pfw_gz(env, 50, 1).empty());

        dstd::String out_dir = env.get_dir() + "/split_verify";
        int rc = run_split(binary, {input_dir(env), "-o", out_dir, "--verify",
                                    "--disable-watchdog"});
        CHECK(rc == 0);
    }

    TEST_CASE("split preserves event lines") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        const int num_events = 80;
        auto f = create_pfw_gz(env, num_events, 0);
        REQUIRE(!f.empty());

        dstd::String out_dir = env.get_dir() + "/split_lines";
        int rc = run_split(
            binary, {input_dir(env), "-o", out_dir, "--disable-watchdog"});
        CHECK(rc == 0);
        REQUIRE(dstd::fs::exists(out_dir).value());

        int total_lines = count_gz_lines_in_dir(out_dir);
        // Split output contains all events plus possible metadata lines.
        // Must have at least as many lines as events.
        CHECK(total_lines >= num_events);
    }

    TEST_CASE("split multiple input files") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        REQUIRE(!create_pfw_gz(env, 30, 0).empty());
        REQUIRE(!create_pfw_gz(env, 30, 1).empty());

        dstd::String out_dir = env.get_dir() + "/split_multi";
        int rc = run_split(
            binary, {input_dir(env), "-o", out_dir, "--disable-watchdog"});
        CHECK(rc == 0);
        REQUIRE(dstd::fs::exists(out_dir).value());
        CHECK(count_gz_files(out_dir) > 0);
    }

    TEST_CASE("a rerun does not read its own output back") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 2000, 0).empty());

        const dstd::String out = input_dir(env) + "/chunks";
        REQUIRE(run_split(binary, {input_dir(env), "-o", out,
                                   "--disable-watchdog"}) == 0);
        const auto first = count_gz_lines_in_dir(out);
        REQUIRE(first > 0);
        (void)dstd::fs::remove_all(out);
        REQUIRE(run_split(binary, {input_dir(env), "-o", out,
                                   "--disable-watchdog"}) == 0);
        REQUIRE(dstd::fs::create_directories(out + "/stale").has_value());
        REQUIRE(run_split(binary, {input_dir(env), "-o", out,
                                   "--disable-watchdog"}) == 0);
        CHECK(count_gz_lines_in_dir(out) == first);
    }

    TEST_CASE("empty directory returns non-zero") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        dstd::String out_dir = env.get_dir() + "/split_empty";
        int rc = run_split(
            binary, {input_dir(env), "-o", out_dir, "--disable-watchdog"});
        CHECK(rc != 0);
    }

    TEST_CASE("a plain trace is not split and returns non-zero") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        {
            dftu_utils_test::FileOut plain(input_dir(env) + "/plain.pfw");
            plain << "{\"name\":\"read\",\"cat\":\"POSIX\",\"ph\":\"X\"}\n";
        }

        dstd::String out_dir = env.get_dir() + "/split_plain";
        int rc = run_split(
            binary, {input_dir(env), "-o", out_dir, "--disable-watchdog"});
        CHECK(rc != 0);
        CHECK(count_gz_files(out_dir) == 0);
    }

    TEST_CASE("a plain trace error names the file and says it is not gzip") {
        auto binary = find_split_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_split binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        {
            dftu_utils_test::FileOut plain(input_dir(env) + "/plain.pfw");
            plain << "{\"name\":\"read\",\"cat\":\"POSIX\",\"ph\":\"X\"}\n";
        }

        dftu_utils_test::set_test_library_path(binary);
        int rc = 0;
        const dstd::String out = dftu_utils_test::run_process_capture(
            binary,
            {input_dir(env), "-o", env.get_dir() + "/split_plain",
             "--disable-watchdog"},
            true, &rc);
        CHECK(rc != 0);
        CHECK(out.find("plain.pfw") != dstd::String::npos);
        CHECK(out.find("is not gzip") != dstd::String::npos);
        CHECK(out.find("0 chunk files") == dstd::String::npos);
    }
}
