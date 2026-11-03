// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <spawn.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace dftracer::utils;

extern char** environ;

namespace {
/// Traces must be gzip. Drop-in for the std::ofstream these fixtures used:
/// same `<<` and close(), but the bytes land compressed.
class GzTraceWriter {
   public:
    explicit GzTraceWriter(const dstd::String& path) : path_(path) {}
    ~GzTraceWriter() { close(); }

    template <typename T>
    GzTraceWriter& operator<<(const T& value) {
        buffer_ << value;
        return *this;
    }

    bool is_open() const { return true; }

    void close() {
        if (closed_) return;
        closed_ = true;
        dftu_utils_test::write_gz_trace(path_, buffer_.str());
    }

   private:
    dstd::String path_;
    dftu_utils_test::TextOut buffer_;
    bool closed_ = false;
};
}  // namespace

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

dstd::String find_replay_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_UTILS_REPLAY_PATH",
                                                "dftracer_replay");
}

// This test process links a threaded runtime, so a hand-rolled fork+exec is
// unsafe (the child may run only async-signal-safe code before exec). Use
// posix_spawn, which is built to launch a process safely from a multithreaded
// parent, for every replay invocation.
dstd::Vector<char*> make_argv(const dstd::String& binary,
                              const dstd::Vector<dstd::String>& args) {
    dstd::Vector<char*> argv;
    argv.push_back(const_cast<char*>(binary.c_str()));
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    return argv;
}

dstd::String run_replay_capture(const dstd::String& binary,
                                const dstd::Vector<dstd::String>& args,
                                int* exit_code);

int run_replay(const dstd::String& binary,
               const dstd::Vector<dstd::String>& args) {
    int rc = -1;
    run_replay_capture(binary, args, &rc);
    return rc;
}

dstd::String run_replay_capture(const dstd::String& binary,
                                const dstd::Vector<dstd::String>& args,
                                int* exit_code) {
    int pipefd[2];
    if (::pipe(pipefd) < 0) return "";

    auto argv = make_argv(binary, args);

    posix_spawn_file_actions_t fa;
    ::posix_spawn_file_actions_init(&fa);
    ::posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    ::posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
    ::posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    ::posix_spawn_file_actions_addclose(&fa, pipefd[1]);

    pid_t pid = 0;
    int spawn_rc =
        ::posix_spawn(&pid, binary.c_str(), &fa, nullptr, argv.data(), environ);
    ::posix_spawn_file_actions_destroy(&fa);
    ::close(pipefd[1]);
    if (spawn_rc != 0) {
        ::close(pipefd[0]);
        if (exit_code) *exit_code = -1;
        return "";
    }
    dstd::String output;
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
void create_sample_trace(const dstd::String& path, int num_events = 10) {
    GzTraceWriter file(path);
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

void create_multi_category_trace(const dstd::String& path) {
    GzTraceWriter file(path);
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
                "Set DFTRACER_UTILS_REPLAY_PATH env to specify location.");
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
        CHECK(output.find("DFTracer replay utility") != dstd::String::npos);
        CHECK(output.find("--dftracer-mode") != dstd::String::npos);
        CHECK(output.find("--dry-run") != dstd::String::npos);
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

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_dry");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "test_trace.pfw.gz").str());
        create_sample_trace(trace_file, 5);

        int rc = 0;
        auto output =
            run_replay_capture(binary, {"--dry-run", trace_file}, &rc);
        CHECK(rc == 0);
        CHECK(output.find("Replay Summary") != dstd::String::npos);
        CHECK(output.find("Executed:") != dstd::String::npos);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("dftracer mode with no-sleep") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_nosleep");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "test_trace.pfw.gz").str());
        create_sample_trace(trace_file, 5);

        int rc = 0;
        auto output = run_replay_capture(
            binary, {"--dftracer-mode", "--no-sleep", trace_file}, &rc);
        CHECK(rc == 0);
        CHECK(output.find("Replay Summary") != dstd::String::npos);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("dftracer mode with no-timing") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_notiming");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "test_trace.pfw.gz").str());
        create_sample_trace(trace_file, 5);

        int rc = 0;
        auto output = run_replay_capture(
            binary, {"--dftracer-mode", "--no-timing", trace_file}, &rc);
        CHECK(rc == 0);
        CHECK(output.find("Executed:") != dstd::String::npos);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("a bad pid or tid token is an error") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_bad_id");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "test_trace.pfw.gz").str());
        create_sample_trace(trace_file, 10);

        for (const char* flag : {"--filter-pid", "--exclude-pid",
                                 "--filter-tid", "--exclude-tid"}) {
            int rc = 0;
            auto output = run_replay_capture(
                binary, {"--dry-run", flag, "12,abc", trace_file}, &rc);
            CHECK(rc == 1);
            CHECK(output.find("abc") != dstd::String::npos);
        }

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("filter by category") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_filter_cat");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "multi_category.pfw.gz").str());
        create_multi_category_trace(trace_file);

        int rc = 0;
        auto output = run_replay_capture(
            binary, {"--dry-run", "--filter-category", "POSIX", trace_file},
            &rc);
        CHECK(rc == 0);
        CHECK(output.find("Executed:") != dstd::String::npos);
        CHECK(output.find("Filtered:") != dstd::String::npos);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("filter by multiple categories") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_filter_multi");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "multi_category.pfw.gz").str());
        create_multi_category_trace(trace_file);

        // Exit code may vary but should run.
        run_replay(binary, {"--dry-run", "--filter-category", "POSIX,STDIO",
                            trace_file});

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("filter by function name") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_filter_func");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "multi_category.pfw.gz").str());
        create_multi_category_trace(trace_file);

        int rc = 0;
        auto output = run_replay_capture(
            binary,
            {"--dry-run", "--filter-function", "read,write", trace_file}, &rc);
        CHECK(output.find("Replay Summary") != dstd::String::npos);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("max events limit") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_max_events");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "test_trace.pfw.gz").str());
        create_sample_trace(trace_file, 20);

        int rc = 0;
        auto output = run_replay_capture(
            binary, {"--dry-run", "--max-events", "10", trace_file}, &rc);
        CHECK(rc == 0);
        CHECK(output.find("Executed:") != dstd::String::npos);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("50% deterministic sampling") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_sample50");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "large_trace.pfw.gz").str());
        create_sample_trace(trace_file, 100);

        int rc = 0;
        auto output = run_replay_capture(binary,
                                         {"--dry-run", "--sample-rate", "0.5",
                                          "--sample-seed", "42", trace_file},
                                         &rc);
        CHECK(rc == 0);
        CHECK(output.find("Replay Summary") != dstd::String::npos);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("25% sampling") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_sample25");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "large_trace.pfw.gz").str());
        create_sample_trace(trace_file, 100);

        int rc = 0;
        run_replay_capture(binary,
                           {"--dry-run", "--sample-rate", "0.25",
                            "--sample-seed", "42", trace_file},
                           &rc);
        CHECK(rc == 0);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("invalid sampling rate") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_sample_bad");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "test_trace.pfw.gz").str());
        create_sample_trace(trace_file, 10);

        CHECK(run_replay(binary, {"--dry-run", "--sample-rate", "1.5",
                                  trace_file}) == 1);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("timing scale factor") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_timing");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "perf_trace.pfw.gz").str());
        create_sample_trace(trace_file, 10);

        int rc =
            run_replay(binary, {"--dftracer-mode", "--no-sleep", trace_file});
        CHECK(rc == 0);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("benchmark mode") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_bench");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "perf_trace.pfw.gz").str());
        create_sample_trace(trace_file, 50);

        auto start = std::chrono::steady_clock::now();
        int rc = run_replay(binary, {"--dftracer-mode", "--no-sleep",
                                     "--no-timing", trace_file});
        auto end = std::chrono::steady_clock::now();

        CHECK(rc == 0);

        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        if (!under_valgrind()) CHECK(duration.count() < 5000);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("summary reports totals") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_verbose");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "stats_trace.pfw.gz").str());
        create_sample_trace(trace_file, 5);

        int rc = 0;
        auto output =
            run_replay_capture(binary, {"--dry-run", trace_file}, &rc);
        CHECK(rc == 0);
        CHECK(output.find("Total events:") != dstd::String::npos);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("statistics output") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_stats");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "stats_trace.pfw.gz").str());
        create_multi_category_trace(trace_file);

        int rc = 0;
        auto output =
            run_replay_capture(binary, {"--dry-run", trace_file}, &rc);
        CHECK(output.find("Replay Summary") != dstd::String::npos);
        CHECK(output.find("Executed:") != dstd::String::npos);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("per-function statistics") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_perfunc");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace_file =
            dstd::String((temp_dir / "stats_trace.pfw.gz").str());
        create_multi_category_trace(trace_file);

        int rc = 0;
        auto output =
            run_replay_capture(binary, {"--dry-run", trace_file}, &rc);
        CHECK(!output.empty());

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("nonexistent trace file") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        int rc =
            run_replay(binary, {"--dry-run", "/nonexistent/path/trace.pfw"});
        CHECK(rc == 1);
    }

    TEST_CASE("invalid trace format") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_bad_fmt");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String bad_trace =
            dstd::String((temp_dir / "bad_trace.pfw.gz").str());

        dftu_utils_test::FileOut file(bad_trace);
        file << "this is not valid JSON";
        file.close();

        CHECK(run_replay(binary, {"--dry-run", bad_trace}) == 1);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("empty trace file") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_empty");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String empty_trace =
            dstd::String((temp_dir / "empty_trace.pfw.gz").str());

        // Must be a valid (empty) gzip, not plain text in a .gz name.
        GzTraceWriter file(empty_trace);
        file << "[]";
        file.close();

        int rc = 0;
        auto output =
            run_replay_capture(binary, {"--dry-run", empty_trace}, &rc);
        CHECK(output.find("Replay Summary") != dstd::String::npos);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("multiple trace files") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_multi");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String trace1 = dstd::String((temp_dir / "trace1.pfw").str());
        dstd::String trace2 = dstd::String((temp_dir / "trace2.pfw").str());

        create_sample_trace(trace1, 5);
        create_sample_trace(trace2, 5);

        int rc = 0;
        auto output =
            run_replay_capture(binary, {"--dry-run", trace1, trace2}, &rc);
        CHECK(rc == 0);
        CHECK(output.find("Replay Summary") != dstd::String::npos);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("the thread count does not change the result") {
        auto binary = find_replay_binary();
        REQUIRE_FALSE(binary.empty());

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_threads");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::Vector<dstd::String> args = {"--dry-run", "--no-timing"};
        std::size_t expected = 0;
        for (int i = 0; i < 6; ++i) {
            const dstd::String path = dstd::String(
                (temp_dir / ("trace" + dstd::to_string(i) + ".pfw")).str());
            create_sample_trace(path, 3 + i);
            args.push_back(path);
            expected += static_cast<std::size_t>(3 + i);
        }

        auto count_after = [](const dstd::String& out,
                              const dstd::String& key) {
            const auto at = out.find(key);
            REQUIRE(at != dstd::String::npos);
            return dftu_utils_test::to_number<unsigned long long>(
                out.substr(at + key.size()));
        };
        for (const char* threads : {"1", "4"}) {
            auto run_args = args;
            run_args.insert(run_args.begin(), {"--executor-threads", threads});
            int rc = 0;
            auto output = run_replay_capture(binary, run_args, &rc);
            INFO("threads " << threads << ": " << output);
            CHECK(rc == 0);
            CHECK(count_after(output, "Total events: ") == expected);
            CHECK(count_after(output, "Executed: ") == expected);
        }

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("directory with trace files") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_dir");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::Path trace_dir = temp_dir / "traces";
        REQUIRE(dstd::fs::create_directories(trace_dir).has_value());

        create_sample_trace(dstd::String((trace_dir / "trace1.pfw").str()), 3);
        create_sample_trace(dstd::String((trace_dir / "trace2.pfw").str()), 3);
        create_sample_trace(dstd::String((trace_dir / "trace3.pfw").str()), 3);

        int rc =
            run_replay(binary, {"--dry-run", dstd::String(trace_dir.str())});
        CHECK(rc == 0);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("call tree mode with real traces") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::String trace_dir = "trace_short/cosmoflow_h100/nodes-1";
        if (!dstd::fs::is_directory(trace_dir).value()) {
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

        dstd::String trace_dir = "trace_short/cosmoflow_h100/nodes-1";
        if (!dstd::fs::is_directory(trace_dir).value()) {
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

        dstd::String trace_file = "trace_short/bert_v100-1.pfw";
        if (!dstd::fs::exists(trace_file).value()) {
            MESSAGE("Skipping BERT trace test - file not found: ", trace_file);
            return;
        }

        int rc = 0;
        auto output = run_replay_capture(binary,
                                         {"--dftracer-mode", "--no-sleep",
                                          "--max-events", "100", trace_file},
                                         &rc);
        CHECK(rc == 0);
        CHECK(output.find("Total events:") != dstd::String::npos);
    }

    TEST_CASE("CosmoFlow A100 trace replay") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::String trace_dir = "trace_short/cosmoflow_a100";
        if (!dstd::fs::is_directory(trace_dir).value()) {
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

        dstd::String trace_dir = "trace_short/cosmoflow_h100";
        if (!dstd::fs::is_directory(trace_dir).value()) {
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

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_stress");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String large_trace =
            dstd::String((temp_dir / "large_trace.pfw.gz").str());
        create_sample_trace(large_trace, 1000);

        auto start = std::chrono::steady_clock::now();
        int rc = run_replay(binary, {"--dry-run", large_trace});
        auto end = std::chrono::steady_clock::now();

        CHECK(rc == 0);

        auto duration =
            std::chrono::duration_cast<std::chrono::seconds>(end - start);
        if (!under_valgrind()) CHECK(duration.count() < 10);

        (void)dstd::fs::remove_all(temp_dir);
    }

    TEST_CASE("high sampling rate with large file") {
        auto binary = find_replay_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_replay binary not found, skipping.");
            return;
        }

        dstd::Path temp_dir =
            dftu_utils_test::make_unique_test_path("replay_stress2");
        REQUIRE(dstd::fs::create_directories(temp_dir).has_value());
        dstd::String large_trace =
            dstd::String((temp_dir / "large_trace2.pfw").str());
        create_sample_trace(large_trace, 500);

        int rc = run_replay(
            binary, {"--dftracer-mode", "--no-sleep", "--sample-rate", "0.9",
                     "--sample-seed", "42", large_trace});
        CHECK(rc == 0);

        (void)dstd::fs::remove_all(temp_dir);
    }
}
