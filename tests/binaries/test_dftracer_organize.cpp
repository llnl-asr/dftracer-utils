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

std::string find_organize_binary() {
    const char* env_path = std::getenv("DFTRACER_ORGANIZE_PATH");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;

    std::vector<std::string> candidates = {
        "./dftracer_organize",         "../dftracer_organize",
        "../../dftracer_organize",     "../bin/dftracer_organize",
        "../../bin/dftracer_organize",
    };
    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

std::string find_reconstruct_binary() {
    const char* env_path = std::getenv("DFTRACER_RECONSTRUCT_PATH");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;

    std::vector<std::string> candidates = {
        "./dftracer_reconstruct",         "../dftracer_reconstruct",
        "../../dftracer_reconstruct",     "../bin/dftracer_reconstruct",
        "../../bin/dftracer_reconstruct",
    };
    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

int run_binary(const std::string& binary,
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

// Count non-empty lines in a gzip file.
int count_gz_lines(const std::string& gz_path) {
    gzFile gz = gzopen(gz_path.c_str(), "rb");
    if (!gz) return -1;
    int count = 0;
    char buf[8192];
    while (gzgets(gz, buf, sizeof(buf)) != nullptr) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        if (!line.empty()) ++count;
    }
    gzclose(gz);
    return count;
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

// Check whether any file matching a glob-like suffix exists in a directory.
bool any_file_with_suffix(const std::string& dir, const std::string& suffix) {
    if (!fs::exists(dir)) return false;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            const auto name = entry.path().filename().string();
            if (name.size() >= suffix.size() &&
                name.substr(name.size() - suffix.size()) == suffix) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerOrganize") {
    TEST_CASE("organize binary exists") {
        auto binary = find_organize_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_organize binary not found, skipping. "
                "Set DFTRACER_ORGANIZE_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("reconstruct binary exists") {
        auto binary = find_reconstruct_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_reconstruct binary not found, skipping. "
                "Set DFTRACER_RECONSTRUCT_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("basic organize") {
        auto binary = find_organize_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_organize binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        std::string out_dir = env.get_dir() + "/organized";
        fs::create_directories(out_dir);

        // Test data uses "cat":"IO" for all events.
        int rc = run_binary(binary, {"-d", env.get_dir(), "-o", out_dir,
                                     "--groups", R"(io:cat == "IO")"});
        CHECK(rc == 0);

        // At least one output file must exist (io.pfw.gz or io.pfw).
        bool has_output = fs::exists(out_dir + "/io.pfw.gz") ||
                          fs::exists(out_dir + "/io.pfw");
        CHECK(has_output);
    }

    TEST_CASE("organize creates midx sidecar") {
        auto binary = find_organize_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_organize binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        std::string out_dir = env.get_dir() + "/organized_midx";
        fs::create_directories(out_dir);

        int rc = run_binary(binary, {"-d", env.get_dir(), "-o", out_dir,
                                     "--groups", R"(io:cat == "IO")"});
        CHECK(rc == 0);

        // The organizer builds .pidx sidecars in the output directory.
        CHECK(any_file_with_suffix(out_dir, ".pidx"));
    }

    TEST_CASE("reconstruct from organized") {
        auto org_binary = find_organize_binary();
        auto rec_binary = find_reconstruct_binary();
        if (org_binary.empty() || rec_binary.empty()) {
            MESSAGE(
                "dftracer_organize or dftracer_reconstruct binary not found, "
                "skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        std::string org_dir = env.get_dir() + "/org_for_recon";
        std::string rec_dir = env.get_dir() + "/reconstructed";
        fs::create_directories(org_dir);
        fs::create_directories(rec_dir);

        int rc_org = run_binary(org_binary, {"-d", env.get_dir(), "-o", org_dir,
                                             "--groups", R"(io:cat == "IO")"});
        REQUIRE(rc_org == 0);

        // Reconstruct needs the .pidx sidecars in the organized dir.
        int rc_rec = run_binary(
            rec_binary, {"-d", org_dir, "-o", rec_dir, "--no-compress"});
        CHECK(rc_rec == 0);

        // At least one reconstructed file must exist.
        bool has_output = false;
        if (fs::exists(rec_dir)) {
            for (const auto& entry : fs::directory_iterator(rec_dir)) {
                if (entry.is_regular_file() &&
                    entry.path().extension() == ".pfw") {
                    has_output = true;
                    break;
                }
            }
        }
        CHECK(has_output);
    }

    TEST_CASE("round-trip preserves event count") {
        auto org_binary = find_organize_binary();
        auto rec_binary = find_reconstruct_binary();
        if (org_binary.empty() || rec_binary.empty()) {
            MESSAGE(
                "dftracer_organize or dftracer_reconstruct binary not found, "
                "skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        const int num_events = 80;
        auto f = create_pfw_gz(env, num_events, 0);
        REQUIRE(!f.empty());

        int original_lines = count_gz_lines(f);
        REQUIRE(original_lines > 0);
        // Subtract array delimiters ([ and ]) -- organizer strips them.
        int original_events = original_lines - 2;

        std::string org_dir = env.get_dir() + "/org_roundtrip";
        std::string rec_dir = env.get_dir() + "/rec_roundtrip";
        fs::create_directories(org_dir);
        fs::create_directories(rec_dir);

        int rc_org = run_binary(org_binary, {"-d", env.get_dir(), "-o", org_dir,
                                             "--groups", R"(io:cat == "IO")"});
        REQUIRE(rc_org == 0);

        int rc_rec = run_binary(
            rec_binary, {"-d", org_dir, "-o", rec_dir, "--no-compress"});
        REQUIRE(rc_rec == 0);

        // Sum lines across all reconstructed .pfw files.
        int reconstructed_lines = 0;
        if (fs::exists(rec_dir)) {
            for (const auto& entry : fs::directory_iterator(rec_dir)) {
                if (entry.is_regular_file() &&
                    entry.path().extension() == ".pfw") {
                    int n = count_lines(entry.path().string());
                    if (n > 0) reconstructed_lines += n;
                }
            }
        }

        // All events routed to "io" group should be recoverable.
        CHECK(reconstructed_lines == original_events);
    }
}
