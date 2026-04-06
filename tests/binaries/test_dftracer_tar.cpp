#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

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

std::string find_tar_binary() {
    const char* env_path = std::getenv("DFTRACER_TAR_PATH");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;

    std::vector<std::string> candidates = {
        "./dftracer_tar",      "../dftracer_tar",        "../../dftracer_tar",
        "../bin/dftracer_tar", "../../bin/dftracer_tar",
    };
    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

// Run binary, return exit code. stdout+stderr are discarded.
int run_tar(const std::string& binary, const std::vector<std::string>& args) {
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

// Run binary capturing stdout+stderr, return exit code via out-param.
std::string run_tar_capture(const std::string& binary,
                            const std::vector<std::string>& args,
                            int* exit_code = nullptr) {
    int pipefd[2];
    if (::pipe(pipefd) < 0) return "";
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return "";
    }
    if (pid == 0) {
        set_test_library_path(binary);
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        std::vector<const char*> argv;
        argv.push_back(binary.c_str());
        for (const auto& arg : args) argv.push_back(arg.c_str());
        argv.push_back(nullptr);
        ::execv(binary.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    ::close(pipefd[1]);
    std::string output;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0)
        output.append(buf, static_cast<std::size_t>(n));
    ::close(pipefd[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (exit_code) *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return output;
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerTar") {
    TEST_CASE("binary exists") {
        auto binary = find_tar_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_tar binary not found, skipping. "
                "Set DFTRACER_TAR_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("list files") {
        auto binary = find_tar_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_tar binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100,
                                            dft_utils_test::Format::TAR_GZIP);
        REQUIRE(env.is_valid());

        auto tar_gz = env.create_test_tar_gzip_file();
        REQUIRE(!tar_gz.empty());
        REQUIRE(fs::exists(tar_gz));

        int exit_code = 0;
        auto output =
            run_tar_capture(binary, {tar_gz, "--list-files"}, &exit_code);
        CHECK(exit_code == 0);
        // The test archive contains known filenames; at least one must appear.
        CHECK(!output.empty());
        bool has_filename = output.find(".jsonl") != std::string::npos ||
                            output.find(".json") != std::string::npos ||
                            output.find("main") != std::string::npos;
        CHECK(has_filename);
    }

    TEST_CASE("info") {
        auto binary = find_tar_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_tar binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100,
                                            dft_utils_test::Format::TAR_GZIP);
        REQUIRE(env.is_valid());

        auto tar_gz = env.create_test_tar_gzip_file();
        REQUIRE(!tar_gz.empty());
        REQUIRE(fs::exists(tar_gz));

        int exit_code = 0;
        auto output = run_tar_capture(binary, {tar_gz, "--info"}, &exit_code);
        CHECK(exit_code == 0);
        // --info prints "Archive Information:" and the path.
        bool has_info = output.find("Archive") != std::string::npos ||
                        output.find("Format") != std::string::npos ||
                        output.find("Path") != std::string::npos;
        CHECK(has_info);
    }

    TEST_CASE("build index") {
        auto binary = find_tar_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_tar binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100,
                                            dft_utils_test::Format::TAR_GZIP);
        REQUIRE(env.is_valid());

        auto tar_gz = env.create_test_tar_gzip_file();
        REQUIRE(!tar_gz.empty());
        REQUIRE(fs::exists(tar_gz));

        int rc = run_tar(binary, {tar_gz, "--build-only"});
        CHECK(rc == 0);

        std::string db_root = dftracer::utils::utilities::composites::dft::
            internal::determine_index_path(tar_gz, "");
        CHECK(fs::exists(db_root));
    }

    TEST_CASE("force rebuild") {
        auto binary = find_tar_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_tar binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100,
                                            dft_utils_test::Format::TAR_GZIP);
        REQUIRE(env.is_valid());

        auto tar_gz = env.create_test_tar_gzip_file();
        REQUIRE(!tar_gz.empty());
        REQUIRE(fs::exists(tar_gz));

        // First build.
        int rc1 = run_tar(binary, {tar_gz, "--build-only"});
        REQUIRE(rc1 == 0);

        std::string db_root = dftracer::utils::utilities::composites::dft::
            internal::determine_index_path(tar_gz, "");
        REQUIRE(fs::exists(db_root));

        // Force rebuild must also succeed and leave the DB root intact.
        int rc2 = run_tar(binary, {tar_gz, "--build-only", "--force-rebuild"});
        CHECK(rc2 == 0);
        CHECK(fs::exists(db_root));
    }
}
