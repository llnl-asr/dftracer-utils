#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

namespace {

bool under_valgrind() {
#ifdef DFTRACER_UTILS_VALGRIND_MODE
    return true;
#else
    return false;
#endif
}

std::string find_replay_binary() {
    const char* env_path = std::getenv("DFTRACER_REPLAY_PATH");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;

    std::vector<std::string> candidates = {
        "./dftracer_replay",         "../dftracer_replay",
        "../../dftracer_replay",     "../bin/dftracer_replay",
        "../../bin/dftracer_replay",
    };
    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

int run_replay(const std::string& binary,
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

std::string run_replay_capture(const std::string& binary,
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

// Create a plain .pfw trace file with sample events.
// The replay binary handles uncompressed .pfw without needing an index.
void create_sample_trace(const std::string& path, int num_events = 10) {
    std::ofstream file(path);
    file << "[\n";
    for (int i = 0; i < num_events; i++) {
        file << R"({"id":)" << i
             << R"(,"name":"read","cat":"POSIX","pid":12345,"tid":12345,)";
        file << R"("ts":)" << (1000000 + i * 1000) << R"(,"dur":)"
             << (100 + i * 10);
        file << R"(,"ph":"X","args":{"fhash":"hash)" << i << R"(","size":)"
             << (1024 * (i + 1));
        file << R"(,"level":1}})";
        if (i < num_events - 1) file << ",";
        file << "\n";
    }
    file << "]";
    file.close();
}

void create_multi_category_trace(const std::string& path) {
    std::ofstream file(path);
    file << "[\n";
    file
        << R"({"id":1,"name":"read","cat":"POSIX","pid":12345,"tid":12345,"ts":1000000,"dur":1500,"ph":"X","args":{"size":1024}})"
        << ",\n";
    file
        << R"({"id":2,"name":"fopen","cat":"STDIO","pid":12345,"tid":12345,"ts":1002000,"dur":500,"ph":"X","args":{}})"
        << ",\n";
    file
        << R"({"id":3,"name":"write","cat":"POSIX","pid":12345,"tid":12345,"ts":1003000,"dur":2000,"ph":"X","args":{"size":2048}})"
        << ",\n";
    file
        << R"({"id":4,"name":"fread","cat":"STDIO","pid":12345,"tid":12345,"ts":1006000,"dur":800,"ph":"X","args":{"size":512}})"
        << ",\n";
    file
        << R"({"id":5,"name":"open","cat":"POSIX","pid":12345,"tid":12345,"ts":1007500,"dur":300,"ph":"X","args":{}})"
        << "\n";
    file << "]";
    file.close();
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerReplay") {
    TEST_CASE("binary exists") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_replay binary not found, skipping. "
                "Set DFTRACER_REPLAY_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("help flag") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        int rc = 0;
        auto output = run_replay_capture(binary, {"--help"}, &rc);
        CHECK(rc == 0);
        CHECK(output.find("DFTracer replay utility") != std::string::npos);
        CHECK(output.find("--dftracer-mode") != std::string::npos);
        CHECK(output.find("--dry-run") != std::string::npos);
    }

    TEST_CASE("version flag") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        int rc = 0;
        auto output = run_replay_capture(binary, {"--version"}, &rc);
        CHECK(rc == 0);
        CHECK(!output.empty());
    }

    TEST_CASE("dry run mode") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir = dft_utils_test::make_unique_test_path("replay_dry");
        fs::create_directories(temp_dir);
        std::string trace_file = (temp_dir / "test_trace.pfw").string();
        create_sample_trace(trace_file, 5);

        int rc = 0;
        auto output =
            run_replay_capture(binary, {"--dry-run", trace_file}, &rc);
        CHECK(rc == 0);
        CHECK(output.find("Replay Summary") != std::string::npos);
        CHECK(output.find("Executed:") != std::string::npos);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("dftracer mode with no-sleep") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_nosleep");
        fs::create_directories(temp_dir);
        std::string trace_file = (temp_dir / "test_trace.pfw").string();
        create_sample_trace(trace_file, 5);

        int rc = 0;
        auto output = run_replay_capture(
            binary, {"--dftracer-mode", "--no-sleep", trace_file}, &rc);
        CHECK(rc == 0);
        CHECK(output.find("Replay Summary") != std::string::npos);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("dftracer mode with no-timing") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_notiming");
        fs::create_directories(temp_dir);
        std::string trace_file = (temp_dir / "test_trace.pfw").string();
        create_sample_trace(trace_file, 5);

        int rc = 0;
        auto output = run_replay_capture(
            binary, {"--dftracer-mode", "--no-timing", trace_file}, &rc);
        CHECK(rc == 0);
        CHECK(output.find("Executed:") != std::string::npos);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("filter by category") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_filter_cat");
        fs::create_directories(temp_dir);
        std::string trace_file = (temp_dir / "multi_category.pfw").string();
        create_multi_category_trace(trace_file);

        int rc = 0;
        auto output = run_replay_capture(
            binary, {"--dry-run", "--filter-category", "POSIX", trace_file},
            &rc);
        CHECK(rc == 0);
        CHECK(output.find("Executed:") != std::string::npos);
        CHECK(output.find("Filtered:") != std::string::npos);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("filter by multiple categories") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_filter_multi");
        fs::create_directories(temp_dir);
        std::string trace_file = (temp_dir / "multi_category.pfw").string();
        create_multi_category_trace(trace_file);

        // Exit code may vary but should run.
        run_replay(binary, {"--dry-run", "--filter-category", "POSIX,STDIO",
                            trace_file});

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("filter by function name") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_filter_func");
        fs::create_directories(temp_dir);
        std::string trace_file = (temp_dir / "multi_category.pfw").string();
        create_multi_category_trace(trace_file);

        int rc = 0;
        auto output = run_replay_capture(
            binary,
            {"--dry-run", "--filter-function", "read,write", trace_file}, &rc);
        CHECK(output.find("Replay Summary") != std::string::npos);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("max events limit") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_max_events");
        fs::create_directories(temp_dir);
        std::string trace_file = (temp_dir / "test_trace.pfw").string();
        create_sample_trace(trace_file, 20);

        int rc = 0;
        auto output = run_replay_capture(
            binary, {"--dry-run", "--max-events", "10", trace_file}, &rc);
        CHECK(rc == 0);
        CHECK(output.find("Executed:") != std::string::npos);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("50% deterministic sampling") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_sample50");
        fs::create_directories(temp_dir);
        std::string trace_file = (temp_dir / "large_trace.pfw").string();
        create_sample_trace(trace_file, 100);

        int rc = 0;
        auto output = run_replay_capture(binary,
                                         {"--dry-run", "--sample-rate", "0.5",
                                          "--sample-seed", "42", trace_file},
                                         &rc);
        CHECK(rc == 0);
        CHECK(output.find("Replay Summary") != std::string::npos);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("25% sampling") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_sample25");
        fs::create_directories(temp_dir);
        std::string trace_file = (temp_dir / "large_trace.pfw").string();
        create_sample_trace(trace_file, 100);

        int rc = 0;
        run_replay_capture(binary,
                           {"--dry-run", "--sample-rate", "0.25",
                            "--sample-seed", "42", trace_file},
                           &rc);
        CHECK(rc == 0);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("invalid sampling rate") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_sample_bad");
        fs::create_directories(temp_dir);
        std::string trace_file = (temp_dir / "test_trace.pfw").string();
        create_sample_trace(trace_file, 10);

        // Should handle gracefully or fail with error.
        // The exact behavior depends on argparse validation.
        run_replay(binary, {"--dry-run", "--sample-rate", "1.5", trace_file});

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("timing scale factor") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_timing");
        fs::create_directories(temp_dir);
        std::string trace_file = (temp_dir / "perf_trace.pfw").string();
        create_sample_trace(trace_file, 10);

        int rc =
            run_replay(binary, {"--dftracer-mode", "--no-sleep", trace_file});
        CHECK(rc == 0);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("benchmark mode") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_bench");
        fs::create_directories(temp_dir);
        std::string trace_file = (temp_dir / "perf_trace.pfw").string();
        create_sample_trace(trace_file, 50);

        auto start = std::chrono::steady_clock::now();
        int rc = run_replay(binary, {"--dftracer-mode", "--no-sleep",
                                     "--no-timing", trace_file});
        auto end = std::chrono::steady_clock::now();

        CHECK(rc == 0);

        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        if (!under_valgrind()) CHECK(duration.count() < 5000);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("verbose mode") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_verbose");
        fs::create_directories(temp_dir);
        std::string trace_file = (temp_dir / "stats_trace.pfw").string();
        create_sample_trace(trace_file, 5);

        int rc = 0;
        auto output = run_replay_capture(
            binary, {"--dry-run", "--verbose", trace_file}, &rc);
        CHECK(rc == 0);
        CHECK(output.find("Total events:") != std::string::npos);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("statistics output") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_stats");
        fs::create_directories(temp_dir);
        std::string trace_file = (temp_dir / "stats_trace.pfw").string();
        create_multi_category_trace(trace_file);

        int rc = 0;
        auto output =
            run_replay_capture(binary, {"--dry-run", trace_file}, &rc);
        CHECK(output.find("Replay Summary") != std::string::npos);
        CHECK(output.find("Executed:") != std::string::npos);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("per-function statistics") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_perfunc");
        fs::create_directories(temp_dir);
        std::string trace_file = (temp_dir / "stats_trace.pfw").string();
        create_multi_category_trace(trace_file);

        int rc = 0;
        auto output = run_replay_capture(
            binary, {"--dry-run", "--verbose", trace_file}, &rc);
        CHECK(!output.empty());

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("nonexistent trace file") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        int rc =
            run_replay(binary, {"--dry-run", "/nonexistent/path/trace.pfw"});
        CHECK(rc != 0);
    }

    TEST_CASE("invalid trace format") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_bad_fmt");
        fs::create_directories(temp_dir);
        std::string bad_trace = (temp_dir / "bad_trace.pfw").string();

        std::ofstream file(bad_trace);
        file << "this is not valid JSON";
        file.close();

        // Should handle parse error gracefully.
        // May exit with error or skip invalid entries.
        run_replay(binary, {"--dry-run", bad_trace});

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("empty trace file") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_empty");
        fs::create_directories(temp_dir);
        std::string empty_trace = (temp_dir / "empty_trace.pfw").string();

        std::ofstream file(empty_trace);
        file << "[]";
        file.close();

        int rc = 0;
        auto output =
            run_replay_capture(binary, {"--dry-run", empty_trace}, &rc);
        CHECK(output.find("Replay Summary") != std::string::npos);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("multiple trace files") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_multi");
        fs::create_directories(temp_dir);
        std::string trace1 = (temp_dir / "trace1.pfw").string();
        std::string trace2 = (temp_dir / "trace2.pfw").string();

        create_sample_trace(trace1, 5);
        create_sample_trace(trace2, 5);

        int rc = 0;
        auto output =
            run_replay_capture(binary, {"--dry-run", trace1, trace2}, &rc);
        CHECK(rc == 0);
        CHECK(output.find("Replay Summary") != std::string::npos);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("directory with trace files") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir = dft_utils_test::make_unique_test_path("replay_dir");
        fs::create_directories(temp_dir);
        fs::path trace_dir = temp_dir / "traces";
        fs::create_directories(trace_dir);

        create_sample_trace((trace_dir / "trace1.pfw").string(), 3);
        create_sample_trace((trace_dir / "trace2.pfw").string(), 3);
        create_sample_trace((trace_dir / "trace3.pfw").string(), 3);

        int rc = run_replay(binary, {"--dry-run", trace_dir.string()});
        CHECK(rc == 0);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("call tree mode with real traces") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        std::string trace_dir = "trace_short/cosmoflow_h100/nodes-1";
        if (!fs::is_directory(trace_dir)) {
            MESSAGE("Skipping call tree test - directory not found: ",
                    trace_dir);
            return;
        }

        int rc = run_replay(binary, {"--dry-run", "--use-call-tree",
                                     "--max-events", "10", trace_dir});
        CHECK(rc == 0);
    }

    TEST_CASE("hierarchical replay with call tree") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        std::string trace_dir = "trace_short/cosmoflow_h100/nodes-1";
        if (!fs::is_directory(trace_dir)) {
            MESSAGE("Skipping hierarchical replay test - directory not found: ",
                    trace_dir);
            return;
        }

        int rc =
            run_replay(binary, {"--dftracer-mode", "--use-call-tree",
                                "--no-sleep", "--max-events", "10", trace_dir});
        CHECK(rc == 0);
    }

    TEST_CASE("BERT trace replay") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        std::string trace_file = "trace_short/bert_v100-1.pfw";
        if (!fs::exists(trace_file)) {
            MESSAGE("Skipping BERT trace test - file not found: ", trace_file);
            return;
        }

        int rc = 0;
        auto output = run_replay_capture(binary,
                                         {"--dftracer-mode", "--no-sleep",
                                          "--max-events", "100", trace_file},
                                         &rc);
        CHECK(rc == 0);
        CHECK(output.find("Total events:") != std::string::npos);
    }

    TEST_CASE("CosmoFlow A100 trace replay") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        std::string trace_dir = "trace_short/cosmoflow_a100";
        if (!fs::is_directory(trace_dir)) {
            MESSAGE("Skipping CosmoFlow A100 test - directory not found: ",
                    trace_dir);
            return;
        }

        int rc =
            run_replay(binary, {"--dry-run", "--max-events", "50", trace_dir});
        CHECK(rc == 0);
    }

    TEST_CASE("CosmoFlow H100 trace replay") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        std::string trace_dir = "trace_short/cosmoflow_h100";
        if (!fs::is_directory(trace_dir)) {
            MESSAGE("Skipping CosmoFlow H100 test - directory not found: ",
                    trace_dir);
            return;
        }

        int rc = run_replay(
            binary, {"--dftracer-mode", "--no-sleep", "--sample-rate", "0.1",
                     "--sample-seed", "42", trace_dir});
        CHECK(rc == 0);
    }

    TEST_CASE("large trace file stress test") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_stress");
        fs::create_directories(temp_dir);
        std::string large_trace = (temp_dir / "large_trace.pfw").string();
        create_sample_trace(large_trace, 1000);

        auto start = std::chrono::steady_clock::now();
        int rc = run_replay(binary, {"--dry-run", large_trace});
        auto end = std::chrono::steady_clock::now();

        CHECK(rc == 0);

        auto duration =
            std::chrono::duration_cast<std::chrono::seconds>(end - start);
        if (!under_valgrind()) CHECK(duration.count() < 10);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    TEST_CASE("high sampling rate with large file") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        fs::path temp_dir =
            dft_utils_test::make_unique_test_path("replay_stress2");
        fs::create_directories(temp_dir);
        std::string large_trace = (temp_dir / "large_trace2.pfw").string();
        create_sample_trace(large_trace, 500);

        int rc = run_replay(
            binary, {"--dftracer-mode", "--no-sleep", "--sample-rate", "0.9",
                     "--sample-seed", "42", large_trace});
        CHECK(rc == 0);

        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
}
