#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
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

void set_test_library_path(const std::string& binary) {
    const fs::path build_root = fs::path(binary).parent_path().parent_path();
    const std::string lib_path =
        (build_root / "lib").string() + ":" +
        (build_root / "_deps" / "rocksdb-build").string();
    ::setenv("LD_LIBRARY_PATH", lib_path.c_str(), 1);
}

std::string create_pfw_gz(dft_utils_test::TestEnvironment& env, int num_events,
                          int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";

    std::string pfw_path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw.gz";
    fs::rename(trace_gz, pfw_path);
    return pfw_path;
}

std::string find_index_binary() {
    const char* env_path = std::getenv("DFTRACER_INDEX_PATH");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;

    std::vector<std::string> candidates = {
        "./dftracer_index",         "../dftracer_index",
        "../../dftracer_index",     "../bin/dftracer_index",
        "../../bin/dftracer_index",
    };
    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

int run_index(const std::string& binary, const std::vector<std::string>& args) {
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        set_test_library_path(binary);
        std::vector<const char*> argv;
        argv.push_back(binary.c_str());
        for (const auto& arg : args) argv.push_back(arg.c_str());
        argv.push_back(nullptr);
        ::execv(binary.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerIndex") {
    TEST_CASE("binary exists") {
        auto binary = find_index_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_index binary not found, skipping. "
                "Set DFTRACER_INDEX_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("build bloom index") {
        auto binary = find_index_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_index binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        int rc = run_index(binary, {"-d", env.get_dir(), "--force"});
        CHECK(rc == 0);

        CHECK(fs::exists(dftracer::utils::utilities::composites::dft::internal::
                             determine_index_path(f, "")));
    }

    TEST_CASE("build index with custom index-dir") {
        auto binary = find_index_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_index binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        // Place index files in a separate subdirectory.
        std::string idx_dir = env.get_dir() + "/idx";
        fs::create_directories(idx_dir);

        int rc = run_index(
            binary, {"-d", env.get_dir(), "--force", "--index-dir", idx_dir});
        CHECK(rc == 0);

        CHECK(fs::exists(dftracer::utils::utilities::composites::dft::internal::
                             determine_index_path(f, idx_dir)));
    }

    TEST_CASE("build with manifest creates idx") {
        auto binary = find_index_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_index binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        int rc =
            run_index(binary, {"-d", env.get_dir(), "--force", "--manifest"});
        CHECK(rc == 0);

        CHECK(fs::exists(dftracer::utils::utilities::composites::dft::internal::
                             determine_index_path(f, "")));
    }

    TEST_CASE("force rebuild runs twice without error") {
        auto binary = find_index_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_index binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        int rc1 = run_index(binary, {"-d", env.get_dir(), "--force"});
        CHECK(rc1 == 0);
        REQUIRE(fs::exists(dftracer::utils::utilities::composites::dft::
                               internal::determine_index_path(f, "")));

        // Second run with --force must overwrite successfully.
        int rc2 = run_index(binary, {"-d", env.get_dir(), "--force"});
        CHECK(rc2 == 0);
        CHECK(fs::exists(dftracer::utils::utilities::composites::dft::internal::
                             determine_index_path(f, "")));
    }
}
