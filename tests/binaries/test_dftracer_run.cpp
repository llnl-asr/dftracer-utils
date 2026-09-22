#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cstdlib>
#include <regex>
#include <string>
#include <vector>

namespace {

std::string env_or_probe(const char* var, const std::string& name) {
    const char* p = std::getenv(var);
    if (p != nullptr && ::access(p, X_OK) == 0) return p;
    for (const std::string& c : {"./" + name, "../" + name, "../../" + name}) {
        if (::access(c.c_str(), X_OK) == 0) return c;
    }
    return "";
}

std::string create_pfw_gz(dftu_utils_test::TestEnvironment& env,
                          int num_events) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";
    std::string pfw_path = env.get_dir() + "/trace.pfw.gz";
    fs::rename(trace_gz, pfw_path);
    return pfw_path;
}

struct RunResult {
    int exit_code = -1;
    std::string err;
};

// Run the binary and capture its stderr, where host->log and the run summary
// land. Everything the child touches after fork is precomputed so it calls
// only async-signal-safe functions.
RunResult run_capture_stderr(const std::string& binary,
                             const std::vector<std::string>& args) {
    int pipefd[2];
    if (::pipe(pipefd) < 0) return {};

    std::vector<const char*> argv;
    argv.push_back(binary.c_str());
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return {};
    }
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        ::execv(binary.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    ::close(pipefd[1]);

    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    ::close(pipefd[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    RunResult r;
    r.err = std::move(out);
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

long first_capture(const std::string& text, const std::string& pattern) {
    std::smatch m;
    if (std::regex_search(text, m, std::regex(pattern))) return std::stol(m[1]);
    return -1;
}

}  // namespace

TEST_SUITE("DFTracerRun") {
    TEST_CASE("event_counter plugin (C ABI) counts every scanned event") {
        std::string run = env_or_probe("DFTRACER_RUN_PATH", "dftracer_run");
        std::string plugin = env_or_probe("DFTRACER_EVENT_COUNTER_PLUGIN_PATH",
                                          "event_counter.so");
        if (run.empty() || plugin.empty()) {
            MESSAGE(
                "dftracer_run or event_counter plugin not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(50);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 50).empty());

        RunResult r =
            run_capture_stderr(run, {"-d", env.get_dir(), "--plugin", plugin});
        CHECK(r.exit_code == 0);
        long events = first_capture(r.err, "events=([0-9]+)");
        long with_dur = first_capture(r.err, "with_dur=([0-9]+)");
        CHECK(events == 50);
        CHECK(with_dur == 50);
        MESSAGE("event_counter: " << r.err);
    }

    TEST_CASE("duration_histogram plugin (C++ ABI) bins every event") {
        std::string run = env_or_probe("DFTRACER_RUN_PATH", "dftracer_run");
        std::string plugin = env_or_probe(
            "DFTRACER_DURATION_HISTOGRAM_PLUGIN_PATH", "duration_histogram.so");
        if (run.empty() || plugin.empty()) {
            MESSAGE("dftracer_run or duration_histogram plugin not found.");
            return;
        }

        dftu_utils_test::TestEnvironment env(40);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 40).empty());

        RunResult r =
            run_capture_stderr(run, {"-d", env.get_dir(), "--plugin", plugin});
        CHECK(r.exit_code == 0);

        long total = 0;
        int lines = 0;
        std::regex bin("2\\^[0-9]+ us: ([0-9]+)");
        for (std::sregex_iterator it(r.err.begin(), r.err.end(), bin), end;
             it != end; ++it) {
            total += std::stol((*it)[1]);
            ++lines;
        }
        CHECK(lines >= 1);
        CHECK(total == 40);  // every generated event carries a duration
        MESSAGE("duration_histogram: bins=" << lines << " total=" << total);
    }

    TEST_CASE("query_filter plugin (C++ SDK, F builder, no query-lib link)") {
        std::string run = env_or_probe("DFTRACER_RUN_PATH", "dftracer_run");
        std::string plugin = env_or_probe("DFTRACER_QUERY_FILTER_PLUGIN_PATH",
                                          "query_filter.so");
        if (run.empty() || plugin.empty()) {
            MESSAGE("dftracer_run or query_filter plugin not found, skipping.");
            return;
        }

        // Generator durations are 100 + i*10 for i in 1..N, so `dur > 300`
        // matches i > 20. With N=50 that is 30 of 50 events: proof the plugin's
        // F("dur") > 300 predicate was rendered, parsed host-side, and applied.
        dftu_utils_test::TestEnvironment env(50);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 50).empty());

        RunResult r =
            run_capture_stderr(run, {"-d", env.get_dir(), "--plugin", plugin});
        CHECK(r.exit_code == 0);
        long matched = first_capture(r.err, "matched=([0-9]+)");
        long total = first_capture(r.err, "total=([0-9]+)");
        CHECK(total == 50);
        CHECK(matched == 30);
        MESSAGE("query_filter: " << r.err);
    }

    TEST_CASE("consumer reads the producer's per-batch value over a port") {
        std::string run = env_or_probe("DFTRACER_RUN_PATH", "dftracer_run");
        std::string producer = env_or_probe(
            "DFTRACER_PERBATCH_PRODUCER_PLUGIN_PATH", "perbatch_producer.so");
        std::string consumer = env_or_probe(
            "DFTRACER_PERBATCH_CONSUMER_PLUGIN_PATH", "perbatch_consumer.so");
        if (run.empty() || producer.empty() || consumer.empty()) {
            MESSAGE("dftracer_run or perbatch plugins not found, skipping.");
            return;
        }

        const int N = 30;
        dftu_utils_test::TestEnvironment env(N);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, N).empty());

        // Consumer first on the command line: the fold order comes from the
        // declared provides/consumes, so the producer still runs first.
        RunResult r = run_capture_stderr(run, {"-d", env.get_dir(), "--plugin",
                                               consumer, "--plugin", producer});
        CHECK(r.exit_code == 0);
        CHECK(r.err.find("perbatch_consumer: WIRED") != std::string::npos);
        // Every generated event carries a duration, so the producer's summed
        // per-batch counts equal the event total; a value only the producer's
        // publishes could have supplied.
        long total = first_capture(r.err, "total=([0-9]+)");
        CHECK(total == N);
        MESSAGE("perbatch wired: " << r.err);
    }

    TEST_CASE("consumer alone fails before scanning (no producer)") {
        std::string run = env_or_probe("DFTRACER_RUN_PATH", "dftracer_run");
        std::string consumer = env_or_probe(
            "DFTRACER_PERBATCH_CONSUMER_PLUGIN_PATH", "perbatch_consumer.so");
        if (run.empty() || consumer.empty()) {
            MESSAGE("dftracer_run or perbatch_consumer plugin not found.");
            return;
        }

        dftu_utils_test::TestEnvironment env(20);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 20).empty());

        RunResult r = run_capture_stderr(
            run, {"-d", env.get_dir(), "--plugin", consumer});
        CHECK(r.exit_code != 0);
        CHECK(r.err.find("com.example.perbatch_dur") != std::string::npos);
        CHECK(r.err.find("no loaded plugin provides") != std::string::npos);
        MESSAGE("perbatch unmet: " << r.err);
    }

    TEST_CASE("consumer reads producer's cross-worker-merged accumulator") {
        std::string run = env_or_probe("DFTRACER_RUN_PATH", "dftracer_run");
        std::string producer = env_or_probe(
            "DFTRACER_DUR_STATS_PRODUCER_PLUGIN_PATH", "dur_stats_producer.so");
        std::string consumer = env_or_probe(
            "DFTRACER_DUR_STATS_CONSUMER_PLUGIN_PATH", "dur_stats_consumer.so");
        if (run.empty() || producer.empty() || consumer.empty()) {
            MESSAGE("dftracer_run or dur_stats plugins not found, skipping.");
            return;
        }

        const int N = 60;
        dftu_utils_test::TestEnvironment env(N);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, N).empty());

        // Consumer first on the command line: agg_result still finds the
        // producer's merged accumulator because the fold order is derived.
        RunResult r = run_capture_stderr(run, {"-d", env.get_dir(), "--plugin",
                                               consumer, "--plugin", producer});
        CHECK(r.exit_code == 0);
        CHECK(r.err.find("dur_stats_consumer: WIRED") != std::string::npos);
        // The merged count equals the whole-scan event count only if every
        // worker slice's accumulator was folded into one; a per-slice count
        // could not reach it. The max is the largest duration, so it is
        // positive.
        long count = first_capture(r.err, "count=([0-9]+)");
        CHECK(count == N);
        std::smatch m;
        REQUIRE(std::regex_search(r.err, m, std::regex("max=([0-9.]+)")));
        CHECK(std::stod(m[1]) > 0.0);
        MESSAGE("dur_stats wired: " << r.err);
    }

    TEST_CASE("dur_stats consumer alone fails before scanning") {
        std::string run = env_or_probe("DFTRACER_RUN_PATH", "dftracer_run");
        std::string consumer = env_or_probe(
            "DFTRACER_DUR_STATS_CONSUMER_PLUGIN_PATH", "dur_stats_consumer.so");
        if (run.empty() || consumer.empty()) {
            MESSAGE("dftracer_run or dur_stats_consumer plugin not found.");
            return;
        }

        dftu_utils_test::TestEnvironment env(20);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 20).empty());

        RunResult r = run_capture_stderr(
            run, {"-d", env.get_dir(), "--plugin", consumer});
        CHECK(r.exit_code != 0);
        CHECK(r.err.find("com.example.dur_stats") != std::string::npos);
        CHECK(r.err.find("no loaded plugin provides") != std::string::npos);
        MESSAGE("dur_stats unmet: " << r.err);
    }

    TEST_CASE("--describe prints provides/consumes without a trace") {
        std::string run = env_or_probe("DFTRACER_RUN_PATH", "dftracer_run");
        std::string producer = env_or_probe(
            "DFTRACER_DUR_STATS_PRODUCER_PLUGIN_PATH", "dur_stats_producer.so");
        std::string consumer = env_or_probe(
            "DFTRACER_DUR_STATS_CONSUMER_PLUGIN_PATH", "dur_stats_consumer.so");
        if (run.empty() || producer.empty() || consumer.empty()) {
            MESSAGE("dftracer_run or dur_stats plugins not found, skipping.");
            return;
        }

        // No -d/--files at all: --describe must not require a trace, an
        // index, or a directory argument.
        RunResult r = run_capture_stderr(
            run, {"--describe", "--plugin", producer, "--plugin", consumer});
        CHECK(r.exit_code == 0);
        CHECK(r.err.find("plugin: " + producer) != std::string::npos);
        CHECK(r.err.find("plugin: " + consumer) != std::string::npos);
        CHECK(r.err.find("provides: com.example.dur_stats") !=
              std::string::npos);
        CHECK(r.err.find("consumes: com.example.dur_stats") !=
              std::string::npos);
        // Neither dur_stats plugin registers an op, so the line is present
        // and reads none rather than being omitted.
        CHECK(r.err.find("ops: (none)") != std::string::npos);
        // Neither declares its config keys, so describe says so rather than
        // implying an empty but declared set.
        CHECK(r.err.find("config: (undeclared)") != std::string::npos);
        MESSAGE("describe: " << r.err);
    }

    TEST_CASE("--describe on a plugin that fails to load reports the error") {
        std::string run = env_or_probe("DFTRACER_RUN_PATH", "dftracer_run");
        if (run.empty()) {
            MESSAGE("dftracer_run not found, skipping.");
            return;
        }

        RunResult r = run_capture_stderr(
            run, {"--describe", "--plugin", "/no/such/plugin.so"});
        CHECK(r.exit_code != 0);
        CHECK(!r.err.empty());
    }

    TEST_CASE("missing plugin path fails cleanly") {
        std::string run = env_or_probe("DFTRACER_RUN_PATH", "dftracer_run");
        if (run.empty()) {
            MESSAGE("dftracer_run not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(10);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 10).empty());

        RunResult r = run_capture_stderr(
            run, {"-d", env.get_dir(), "--plugin", "/no/such/plugin.so"});
        CHECK(r.exit_code != 0);
    }
}
