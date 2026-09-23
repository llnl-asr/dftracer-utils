#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdlib>
#include <string>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

namespace {

std::string find_gen_fake_trace_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_GEN_FAKE_TRACE_PATH",
                                                "dftracer_gen_fake_trace");
}

int run_gen_fake_trace(const std::string& binary,
                       const std::vector<std::string>& args) {
    return dftu_utils_test::run_process(binary, args);
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerGenFakeTrace") {
    TEST_CASE("binary exists") {
        auto binary = find_gen_fake_trace_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_gen_fake_trace binary not found, skipping. "
                "Set DFTRACER_GEN_FAKE_TRACE_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("generate single rank") {
        auto binary = find_gen_fake_trace_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_gen_fake_trace binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        std::string out_dir = env.get_dir() + "/output";
        int rc = run_gen_fake_trace(
            binary, {"-o", out_dir, "-p", "1", "-e", "1", "-s", "1",
                     "--num-train-files", "1", "--num-val-files", "1"});
        CHECK(rc == 0);

        std::string rank0 = out_dir + "/rank_0.pfw.gz";
        REQUIRE(fs::exists(rank0));
        CHECK(fs::file_size(rank0) > 0);
    }

    TEST_CASE("generate multiple ranks") {
        auto binary = find_gen_fake_trace_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_gen_fake_trace binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        std::string out_dir = env.get_dir() + "/output";
        int rc = run_gen_fake_trace(
            binary, {"-o", out_dir, "-p", "3", "-e", "1", "-s", "1",
                     "--num-train-files", "1", "--num-val-files", "1"});
        CHECK(rc == 0);

        for (int rank = 0; rank < 3; ++rank) {
            std::string path =
                out_dir + "/rank_" + std::to_string(rank) + ".pfw.gz";
            CHECK(fs::exists(path));
            CHECK(fs::file_size(path) > 0);
        }
    }

    TEST_CASE("output is valid gzip with JSON content") {
        auto binary = find_gen_fake_trace_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_gen_fake_trace binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        std::string out_dir = env.get_dir() + "/output";
        int rc = run_gen_fake_trace(
            binary, {"-o", out_dir, "-p", "1", "-e", "1", "-s", "1",
                     "--num-train-files", "1", "--num-val-files", "1"});
        CHECK(rc == 0);

        std::string rank0 = out_dir + "/rank_0.pfw.gz";
        REQUIRE(fs::exists(rank0));

        // First line is the opening JSON array bracket.
        auto first = dftu_utils_test::gz_first_line(rank0);
        REQUIRE(!first.empty());
        CHECK(first.front() == '[');
    }

    TEST_CASE("deterministic output with fixed seed") {
        auto binary = find_gen_fake_trace_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_gen_fake_trace binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        std::string out1 = env.get_dir() + "/run1";
        std::string out2 = env.get_dir() + "/run2";

        std::vector<std::string> common_args = {"-p",
                                                "1",
                                                "-e",
                                                "1",
                                                "-s",
                                                "2",
                                                "--num-train-files",
                                                "1",
                                                "--num-val-files",
                                                "1",
                                                "--seed",
                                                "42"};

        auto args1 = common_args;
        args1.insert(args1.begin(), {"-o", out1});
        int rc1 = run_gen_fake_trace(binary, args1);
        CHECK(rc1 == 0);

        auto args2 = common_args;
        args2.insert(args2.begin(), {"-o", out2});
        int rc2 = run_gen_fake_trace(binary, args2);
        CHECK(rc2 == 0);

        std::string f1 = out1 + "/rank_0.pfw.gz";
        std::string f2 = out2 + "/rank_0.pfw.gz";
        REQUIRE(fs::exists(f1));
        REQUIRE(fs::exists(f2));

        // Same seed + same parameters must produce identical file sizes.
        CHECK(fs::file_size(f1) == fs::file_size(f2));
    }

    TEST_CASE("verify mode succeeds") {
        auto binary = find_gen_fake_trace_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_gen_fake_trace binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        std::string out_dir = env.get_dir() + "/output";
        // Use minimal parameters to keep the test fast.
        int rc =
            run_gen_fake_trace(binary, {"-o", out_dir, "-p", "1", "-e", "1",
                                        "-s", "1", "--num-train-files", "1",
                                        "--num-val-files", "1", "--verify"});
        CHECK(rc == 0);
    }

    TEST_CASE("missing required output dir argument fails") {
        auto binary = find_gen_fake_trace_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_gen_fake_trace binary not found, skipping.");
            return;
        }

        // Omit -o; binary must exit non-zero.
        int rc = run_gen_fake_trace(binary, {"-p", "1", "-e", "1", "-s", "1"});
        CHECK(rc != 0);
    }
}
