#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <fcntl.h>
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

std::string create_pfw_gz(dft_utils_test::TestEnvironment& env, int num_events,
                          int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";

    std::string pfw_path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw.gz";
    fs::rename(trace_gz, pfw_path);
    return pfw_path;
}

// Use a distinct env var name to avoid collision with the reader unit tests.
std::string find_reader_binary() {
    const char* env_path = std::getenv("DFTRACER_READER_BIN_PATH");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;

    std::vector<std::string> candidates = {
        "./dftracer_reader",         "../dftracer_reader",
        "../../dftracer_reader",     "../bin/dftracer_reader",
        "../../bin/dftracer_reader",
    };
    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

int run_reader(const std::string& binary,
               const std::vector<std::string>& args) {
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
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

std::string run_reader_capture(const std::string& binary,
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
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[1]);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
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

// Return the first non-whitespace character in a string.
char first_nonws(const std::string& s) {
    for (char c : s) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return c;
    }
    return '\0';
}

// Return the last non-whitespace character in a string.
char last_nonws(const std::string& s) {
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (*it != ' ' && *it != '\t' && *it != '\n' && *it != '\r') return *it;
    }
    return '\0';
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerReader") {
    TEST_CASE("binary exists") {
        auto binary = find_reader_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_reader binary not found, skipping. "
                "Set DFTRACER_READER_BIN_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("read entire file") {
        auto binary = find_reader_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_reader binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 20, 0);
        REQUIRE(!f.empty());

        int rc = 0;
        auto output = run_reader_capture(binary, {f, "--mode", "lines"}, &rc);
        CHECK(rc == 0);
        CHECK(!output.empty());
        CHECK(first_nonws(output) == '[');
        CHECK(last_nonws(output) == ']');
    }

    TEST_CASE("read with line range") {
        auto binary = find_reader_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_reader binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 20, 0);
        REQUIRE(!f.empty());

        // Lines 1-6 covers "[" + up to 5 events.
        int rc = run_reader(
            binary, {f, "--mode", "lines", "--start", "1", "--end", "6"});
        CHECK(rc == 0);
    }

    TEST_CASE("check mode validates file integrity") {
        auto binary = find_reader_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_reader binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 20, 0);
        REQUIRE(!f.empty());

        // Build the index first so --check can validate it
        int rc = run_reader(binary, {f, "--mode", "lines"});
        REQUIRE(rc == 0);

        rc = run_reader(binary, {f, "--check"});
        CHECK(rc == 0);
    }

    TEST_CASE("nonexistent file returns non-zero") {
        auto binary = find_reader_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_reader binary not found, skipping.");
            return;
        }

        int rc = run_reader(binary, {"/nonexistent/path/trace.pfw.gz"});
        CHECK(rc != 0);
    }
}
