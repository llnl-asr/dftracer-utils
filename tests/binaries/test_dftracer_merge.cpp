#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdlib>
#include <fstream>
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

std::string find_merge_binary() {
    const char* env_path = std::getenv("DFTRACER_MERGE_PATH");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;

    std::vector<std::string> candidates = {
        "./dftracer_merge",         "../dftracer_merge",
        "../../dftracer_merge",     "../bin/dftracer_merge",
        "../../bin/dftracer_merge",
    };
    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

int run_merge(const std::string& binary, const std::vector<std::string>& args) {
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

// Count non-empty lines in a plain text file.
int count_lines(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return -1;
    int n = 0;
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty()) ++n;
    }
    return n;
}

// Read first non-empty line from a plain text file.
std::string first_line(const std::string& path) {
    std::ifstream ifs(path);
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty()) return line;
    }
    return "";
}

// Read last non-empty line from a plain text file.
std::string last_line(const std::string& path) {
    std::ifstream ifs(path);
    std::string line, last;
    while (std::getline(ifs, line)) {
        if (!line.empty()) last = line;
    }
    return last;
}

// Decompress a gzip file and return the first non-empty line.
std::string gz_first_line(const std::string& gz_path) {
    gzFile gz = gzopen(gz_path.c_str(), "rb");
    if (!gz) return "";

    char buf[4096];
    std::string result;
    while (gzgets(gz, buf, sizeof(buf)) != nullptr) {
        std::string line(buf);
        // strip trailing newline
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        if (!line.empty()) {
            result = line;
            break;
        }
    }
    gzclose(gz);
    return result;
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerMerge") {
    TEST_CASE("binary exists") {
        auto binary = find_merge_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_merge binary not found, skipping. "
                "Set DFTRACER_MERGE_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("basic merge - uncompressed output") {
        auto binary = find_merge_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_merge binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        const int events_per_file = 10;
        const int num_files = 3;
        for (int i = 0; i < num_files; ++i) {
            auto f = create_pfw_gz(env, events_per_file, i);
            REQUIRE(!f.empty());
        }

        std::string output = env.get_dir() + "/merged.pfw";
        int rc = run_merge(binary, {"-d", env.get_dir(), "-o", output, "-f",
                                    "--disable-watchdog"});
        CHECK(rc == 0);
        REQUIRE(fs::exists(output));

        CHECK(first_line(output) == "[");
        CHECK(last_line(output) == "]");

        // [ + events + ] = num_files * events_per_file + 2
        int expected_lines = num_files * events_per_file + 2;
        CHECK(count_lines(output) == expected_lines);
    }

    TEST_CASE("compressed merge") {
        auto binary = find_merge_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_merge binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        for (int i = 0; i < 2; ++i) {
            auto f = create_pfw_gz(env, 5, i);
            REQUIRE(!f.empty());
        }

        std::string output = env.get_dir() + "/merged_c.pfw";
        std::string output_gz = output + ".gz";
        int rc = run_merge(binary, {"-d", env.get_dir(), "-o", output, "-f",
                                    "--compress", "--disable-watchdog"});
        CHECK(rc == 0);
        REQUIRE(fs::exists(output_gz));
        CHECK(!fs::exists(output));  // plain file should not exist

        CHECK(gz_first_line(output_gz) == "[");
    }

    TEST_CASE("verified merge") {
        auto binary = find_merge_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_merge binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        for (int i = 0; i < 2; ++i) {
            auto f = create_pfw_gz(env, 8, i);
            REQUIRE(!f.empty());
        }

        std::string output = env.get_dir() + "/merged_v.pfw";
        int rc = run_merge(binary, {"-d", env.get_dir(), "-o", output, "-f",
                                    "--verify", "--disable-watchdog"});
        CHECK(rc == 0);
    }

    TEST_CASE("empty directory returns non-zero") {
        auto binary = find_merge_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_merge binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        // No .pfw.gz files created -- directory is empty of trace files.
        std::string output = env.get_dir() + "/merged_empty.pfw";
        int rc = run_merge(binary, {"-d", env.get_dir(), "-o", output, "-f",
                                    "--disable-watchdog"});
        CHECK(rc != 0);
    }

    TEST_CASE("gzip-only mode skips plain .pfw files") {
        auto binary = find_merge_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_merge binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        // Create one .pfw.gz file (should be processed).
        auto gz = create_pfw_gz(env, 5, 0);
        REQUIRE(!gz.empty());

        // Create a plain .pfw file (should be ignored with -g).
        std::string plain = env.get_dir() + "/plain.pfw";
        {
            std::ofstream ofs(plain);
            REQUIRE(ofs.is_open());
            // Write a minimal valid DFTracer event so the file is non-trivial.
            ofs << R"({"id":1,"pid":1,"tid":1,"name":"read","cat":"IO",)"
                << R"("ph":"C","ts":1000000,"dur":100,"args":{"ret":1024}})"
                << "\n";
        }

        std::string output = env.get_dir() + "/merged_g.pfw";
        // -g: only .pfw.gz files; the plain .pfw should be ignored.
        int rc = run_merge(binary, {"-d", env.get_dir(), "-o", output, "-f",
                                    "-g", "--disable-watchdog"});
        // Should succeed because there is at least one .pfw.gz file.
        CHECK(rc == 0);
        REQUIRE(fs::exists(output));

        // Output should contain only the 5 events from the .pfw.gz file.
        CHECK(count_lines(output) == 5 + 2);  // [ + 5 events + ]
    }
}
