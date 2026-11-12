// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cstdlib>
#include <regex>
#include <string>

using namespace dftracer::utils;

namespace {

dstd::String env_or_probe(const char* var, const dstd::String& name) {
    const char* p = std::getenv(var);
    if (p != nullptr && ::access(p, X_OK) == 0) return p;
    for (const dstd::String& c : {"./" + name, "../" + name, "../../" + name}) {
        if (::access(c.c_str(), X_OK) == 0) return c;
    }
    return "";
}

dstd::String create_pfw_gz(dftu_utils_test::TestEnvironment& env,
                           int event_count) {
    auto trace_gz = env.create_dft_test_gzip_file(event_count);
    if (trace_gz.empty()) return "";
    dstd::String pfw_path = env.dir() + "/trace.pfw.gz";
    REQUIRE(dstd::fs::rename(trace_gz, pfw_path).has_value());
    return pfw_path;
}

struct RunResult {
    int exit_code = -1;
    dstd::String err;
};

// Run the binary and capture its stderr, where host->log and the run summary
// land. Everything the child touches after fork is precomputed so it calls
// only async-signal-safe functions.
RunResult run_capture_stderr(const dstd::String& binary,
                             const dstd::Vector<dstd::String>& args) {
    int pipefd[2];
    if (::pipe(pipefd) < 0) return {};

    dstd::Vector<const char*> argv;
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

    dstd::String out;
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

long first_capture(const dstd::String& text, const dstd::String& pattern) {
    std::match_results<dstd::String::const_iterator> m;
    if (std::regex_search(text, m, std::regex(pattern.c_str())))
        return dftu_utils_test::to_number<long>(
            dstd::String(m[1].first, m[1].second));
    return -1;
}

}  // namespace

TEST_SUITE("DFTracerRun") {
    TEST_CASE("event_counter plugin (C ABI) counts every scanned event") {
        dstd::String run =
            env_or_probe("DFTRACER_UTILS_RUN_PATH", "dftracer_run");
        dstd::String plugin = env_or_probe(
            "DFTRACER_UTILS_EVENT_COUNTER_PLUGIN_PATH", "event_counter.so");
        if (run.empty() || plugin.empty()) {
            MESSAGE(
                "dftracer_run or event_counter plugin not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(50);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 50).empty());

        RunResult r = run_capture_stderr(run, {env.dir(), "--plugin", plugin});
        CHECK(r.exit_code == 0);
        long events = first_capture(r.err, "events=([0-9]+)");
        long with_dur = first_capture(r.err, "with_dur=([0-9]+)");
        CHECK(events == 50);
        CHECK(with_dur == 50);
        MESSAGE("event_counter: " << r.err);
    }

    TEST_CASE("duration_histogram plugin (C++ ABI) bins every event") {
        dstd::String run =
            env_or_probe("DFTRACER_UTILS_RUN_PATH", "dftracer_run");
        dstd::String plugin =
            env_or_probe("DFTRACER_UTILS_DURATION_HISTOGRAM_PLUGIN_PATH",
                         "duration_histogram.so");
        if (run.empty() || plugin.empty()) {
            MESSAGE("dftracer_run or duration_histogram plugin not found.");
            return;
        }

        dftu_utils_test::TestEnvironment env(40);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 40).empty());

        RunResult r = run_capture_stderr(run, {env.dir(), "--plugin", plugin});
        CHECK(r.exit_code == 0);

        long total = 0;
        int lines = 0;
        std::regex bin("2\\^[0-9]+ us: ([0-9]+)");
        for (std::regex_iterator<dstd::String::const_iterator>
                 it(r.err.cbegin(), r.err.cend(), bin),
             end;
             it != end; ++it) {
            total += dftu_utils_test::to_number<long>(
                dstd::String((*it)[1].first, (*it)[1].second));
            ++lines;
        }
        CHECK(lines >= 1);
        CHECK(total == 40);  // every generated event carries a duration
        MESSAGE("duration_histogram: bins=" << lines << " total=" << total);
    }

    TEST_CASE("query_filter plugin (C++ SDK, F builder, no query-lib link)") {
        dstd::String run =
            env_or_probe("DFTRACER_UTILS_RUN_PATH", "dftracer_run");
        dstd::String plugin = env_or_probe(
            "DFTRACER_UTILS_QUERY_FILTER_PLUGIN_PATH", "query_filter.so");
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

        RunResult r = run_capture_stderr(run, {env.dir(), "--plugin", plugin});
        CHECK(r.exit_code == 0);
        long matched = first_capture(r.err, "matched=([0-9]+)");
        long total = first_capture(r.err, "total=([0-9]+)");
        CHECK(total == 50);
        CHECK(matched == 30);
        MESSAGE("query_filter: " << r.err);
    }

    TEST_CASE("consumer reads the producer's per-batch value over a port") {
        dstd::String run =
            env_or_probe("DFTRACER_UTILS_RUN_PATH", "dftracer_run");
        dstd::String producer =
            env_or_probe("DFTRACER_UTILS_PERBATCH_PRODUCER_PLUGIN_PATH",
                         "perbatch_producer.so");
        dstd::String consumer =
            env_or_probe("DFTRACER_UTILS_PERBATCH_CONSUMER_PLUGIN_PATH",
                         "perbatch_consumer.so");
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
        RunResult r = run_capture_stderr(
            run, {env.dir(), "--plugin", consumer, "--plugin", producer});
        CHECK(r.exit_code == 0);
        CHECK(r.err.find("perbatch_consumer: WIRED") != dstd::String::npos);
        // Every generated event carries a duration, so the producer's summed
        // per-batch counts equal the event total; a value only the producer's
        // publishes could have supplied.
        long total = first_capture(r.err, "total=([0-9]+)");
        CHECK(total == N);
        MESSAGE("perbatch wired: " << r.err);
    }

    TEST_CASE("consumer alone fails before scanning (no producer)") {
        dstd::String run =
            env_or_probe("DFTRACER_UTILS_RUN_PATH", "dftracer_run");
        dstd::String consumer =
            env_or_probe("DFTRACER_UTILS_PERBATCH_CONSUMER_PLUGIN_PATH",
                         "perbatch_consumer.so");
        if (run.empty() || consumer.empty()) {
            MESSAGE("dftracer_run or perbatch_consumer plugin not found.");
            return;
        }

        dftu_utils_test::TestEnvironment env(20);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 20).empty());

        RunResult r =
            run_capture_stderr(run, {env.dir(), "--plugin", consumer});
        CHECK(r.exit_code != 0);
        CHECK(r.err.find("com.example.perbatch_dur") != dstd::String::npos);
        CHECK(r.err.find("no loaded plugin provides") != dstd::String::npos);
        MESSAGE("perbatch unmet: " << r.err);
    }

    TEST_CASE("consumer reads producer's cross-worker-merged accumulator") {
        dstd::String run =
            env_or_probe("DFTRACER_UTILS_RUN_PATH", "dftracer_run");
        dstd::String producer =
            env_or_probe("DFTRACER_UTILS_DUR_STATS_PRODUCER_PLUGIN_PATH",
                         "dur_stats_producer.so");
        dstd::String consumer =
            env_or_probe("DFTRACER_UTILS_DUR_STATS_CONSUMER_PLUGIN_PATH",
                         "dur_stats_consumer.so");
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
        RunResult r = run_capture_stderr(
            run, {env.dir(), "--plugin", consumer, "--plugin", producer});
        CHECK(r.exit_code == 0);
        CHECK(r.err.find("dur_stats_consumer: WIRED") != dstd::String::npos);
        // The merged count equals the whole-scan event count only if every
        // worker slice's accumulator was folded into one; a per-slice count
        // could not reach it. The max is the largest duration, so it is
        // positive.
        long count = first_capture(r.err, "count=([0-9]+)");
        CHECK(count == N);
        std::match_results<dstd::String::const_iterator> m;
        REQUIRE(std::regex_search(r.err, m, std::regex("max=([0-9.]+)")));
        CHECK(dftu_utils_test::to_number<double>(
                  dstd::String(m[1].first, m[1].second)) > 0.0);
        MESSAGE("dur_stats wired: " << r.err);
    }

    TEST_CASE("dur_stats consumer alone fails before scanning") {
        dstd::String run =
            env_or_probe("DFTRACER_UTILS_RUN_PATH", "dftracer_run");
        dstd::String consumer =
            env_or_probe("DFTRACER_UTILS_DUR_STATS_CONSUMER_PLUGIN_PATH",
                         "dur_stats_consumer.so");
        if (run.empty() || consumer.empty()) {
            MESSAGE("dftracer_run or dur_stats_consumer plugin not found.");
            return;
        }

        dftu_utils_test::TestEnvironment env(20);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 20).empty());

        RunResult r =
            run_capture_stderr(run, {env.dir(), "--plugin", consumer});
        CHECK(r.exit_code != 0);
        CHECK(r.err.find("com.example.dur_stats") != dstd::String::npos);
        CHECK(r.err.find("no loaded plugin provides") != dstd::String::npos);
        MESSAGE("dur_stats unmet: " << r.err);
    }

    TEST_CASE("--describe prints provides/consumes without a trace") {
        dstd::String run =
            env_or_probe("DFTRACER_UTILS_RUN_PATH", "dftracer_run");
        dstd::String producer =
            env_or_probe("DFTRACER_UTILS_DUR_STATS_PRODUCER_PLUGIN_PATH",
                         "dur_stats_producer.so");
        dstd::String consumer =
            env_or_probe("DFTRACER_UTILS_DUR_STATS_CONSUMER_PLUGIN_PATH",
                         "dur_stats_consumer.so");
        if (run.empty() || producer.empty() || consumer.empty()) {
            MESSAGE("dftracer_run or dur_stats plugins not found, skipping.");
            return;
        }

        // No input path at all: --describe must not require a trace, an
        // index, or a directory argument.
        RunResult r = run_capture_stderr(
            run, {"--describe", "--plugin", producer, "--plugin", consumer});
        CHECK(r.exit_code == 0);
        CHECK(r.err.find("plugin: " + producer) != dstd::String::npos);
        CHECK(r.err.find("plugin: " + consumer) != dstd::String::npos);
        CHECK(r.err.find("provides: com.example.dur_stats") !=
              dstd::String::npos);
        CHECK(r.err.find("consumes: com.example.dur_stats") !=
              dstd::String::npos);
        // Neither dur_stats plugin registers an op, so the line is present
        // and reads none rather than being omitted.
        CHECK(r.err.find("ops: (none)") != dstd::String::npos);
        // Neither declares its config keys, so describe says so rather than
        // implying an empty but declared set.
        CHECK(r.err.find("config: (undeclared)") != dstd::String::npos);
        MESSAGE("describe: " << r.err);
    }

    TEST_CASE("--describe on a plugin that fails to load reports the error") {
        dstd::String run =
            env_or_probe("DFTRACER_UTILS_RUN_PATH", "dftracer_run");
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
        dstd::String run =
            env_or_probe("DFTRACER_UTILS_RUN_PATH", "dftracer_run");
        if (run.empty()) {
            MESSAGE("dftracer_run not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(10);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 10).empty());

        RunResult r = run_capture_stderr(
            run, {env.dir(), "--plugin", "/no/such/plugin.so"});
        CHECK(r.exit_code != 0);
    }
}
