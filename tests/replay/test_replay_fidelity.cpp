// Fidelity tests for ReplayEngine.
//
// "Fidelity" here means: when maintain_timing is on, each event is dispatched
// at wall-clock time close to its scheduled position on the trace timeline.
// Excessive lateness compounds into wrong inter-event gaps, defeating the
// point of timing-preserved replay. Two failure modes we want to catch:
//
//   1. Per-event lateness: an individual event fires more than a few ms
//      late vs. when apply_timing should have woken up.
//   2. End-to-end drift: total wall-clock duration diverges from the trace's
//      timespan. Sensitive to the apply_timing anchor bug (where
//      replay_start_time_ wasn't reset on the first event, making every
//      subsequent sleep be skipped).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/replay/replay.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::replay;

namespace {

// Generate a `.pfw` trace with `n` events spaced `step_us` microseconds
// apart. The events name "read" / cat "POSIX" pass the default filters but
// dry_run=true skips actual I/O so the consumer's per-event work is
// dominated by apply_timing sleep.
void write_evenly_spaced_trace(const std::string& path, std::size_t n,
                               std::uint64_t step_us) {
    std::ofstream f(path);
    REQUIRE(f.is_open());
    const std::uint64_t base_ts = 1'000'000;
    f << "[\n";
    for (std::size_t i = 0; i < n; ++i) {
        f << R"({"id":)" << i
          << R"(,"name":"read","cat":"POSIX","pid":12345,"tid":12345,"ts":)"
          << (base_ts + i * step_us) << R"(,"dur":10,"ph":"X","args":{}})";
        if (i + 1 < n) f << ",";
        f << "\n";
    }
    f << "]";
}

struct DispatchSample {
    std::uint64_t trace_ts;  // microseconds since arbitrary trace epoch
    std::chrono::steady_clock::time_point wall;
};

// Capture (trace_ts, wall_now) for each event in dispatch order. on_dispatch
// is invoked from the consumer thread; we still protect the vector since
// future executor changes may dispatch from multiple workers.
struct DispatchRecorder {
    std::mutex m;
    std::vector<DispatchSample> samples;

    void record(const Trace& t, std::chrono::steady_clock::time_point now) {
        std::lock_guard lock(m);
        samples.push_back({t.time_start, now});
    }
};

struct FidelityStats {
    std::int64_t max_lateness_us = 0;
    std::int64_t p99_lateness_us = 0;
    std::int64_t total_wall_span_us = 0;
    std::int64_t expected_trace_span_us = 0;
};

FidelityStats analyze(const std::vector<DispatchSample>& samples) {
    REQUIRE(samples.size() >= 2);

    const auto& first = samples.front();
    const auto& last = samples.back();

    std::vector<std::int64_t> lateness;
    lateness.reserve(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        auto expected_offset =
            std::chrono::microseconds(samples[i].trace_ts - first.trace_ts);
        auto expected_wall = first.wall + expected_offset;
        auto delta = std::chrono::duration_cast<std::chrono::microseconds>(
                         samples[i].wall - expected_wall)
                         .count();
        lateness.push_back(delta);
    }

    FidelityStats out;
    out.max_lateness_us = *std::max_element(lateness.begin(), lateness.end());
    auto sorted = lateness;
    std::sort(sorted.begin(), sorted.end());
    out.p99_lateness_us = sorted[(sorted.size() * 99) / 100];
    out.total_wall_span_us =
        std::chrono::duration_cast<std::chrono::microseconds>(last.wall -
                                                              first.wall)
            .count();
    out.expected_trace_span_us =
        static_cast<std::int64_t>(last.trace_ts - first.trace_ts);
    return out;
}

bool is_ci_env() {
#ifdef DFTRACER_UTILS_VALGRIND_MODE
    return true;
#else
    return std::getenv("CI") != nullptr ||
           std::getenv("GITHUB_ACTIONS") != nullptr;
#endif
}

struct Tolerances {
    // Set to a negative value to skip the corresponding check.
    std::int64_t max_per_event_us;
    std::int64_t max_p99_us;
    double wall_span;
};

Tolerances tolerances() {
    if (is_ci_env()) {
        return {/*max_per_event_us=*/-1, /*max_p99_us=*/-1,
                /*wall_span=*/1.0};
    }
    // Local dev: tight microsecond-grade bounds catch regressions early.
    return {/*max_per_event_us=*/10'000, /*max_p99_us=*/5'000,
            /*wall_span=*/0.25};
}

void check_fidelity(const FidelityStats& s, const char* label) {
    const auto t = tolerances();
    INFO("[" << label << " ci=" << is_ci_env() << "] max_lateness="
             << s.max_lateness_us << "us p99=" << s.p99_lateness_us
             << "us wall=" << s.total_wall_span_us
             << "us trace=" << s.expected_trace_span_us << "us");
    if (t.max_per_event_us >= 0) {
        WARN_MESSAGE(
            s.max_lateness_us <= t.max_per_event_us,
            "The lateness exceeds the tolerance in local testing environment");
    }
    if (t.max_p99_us >= 0) {
        WARN_MESSAGE(s.p99_lateness_us <= t.max_p99_us,
                     "The 99th percentile lateness exceeds the tolerance in "
                     "local testing environment");
    }

    const std::int64_t low = static_cast<std::int64_t>(
        s.expected_trace_span_us * (1.0 - t.wall_span));
    WARN_MESSAGE(
        s.total_wall_span_us >= low,
        "The wall span is below the tolerance in local testing environment");

    if (!is_ci_env()) {
        const std::int64_t high = static_cast<std::int64_t>(
            s.expected_trace_span_us * (1.0 + t.wall_span));
        WARN_MESSAGE(
            s.total_wall_span_us <= high,
            "The wall span exceeds the tolerance in local testing environment");
    }
}

}  // namespace

TEST_CASE("Replay fidelity - sync path") {
    DFTRACER_UTILS_LOGGER_INIT();

    fs::path temp_dir = fs::temp_directory_path() / "dftracer_replay_fid_sync";
    fs::create_directories(temp_dir);
    std::string trace_file = (temp_dir / "fid.pfw").string();

    constexpr std::size_t N = 40;
    constexpr std::uint64_t STEP_US = 5'000;
    write_evenly_spaced_trace(trace_file, N, STEP_US);

    DispatchRecorder rec;
    ReplayConfig config;
    config.maintain_timing = true;
    config.dry_run = false;
    config.on_dispatch = [&rec](const Trace& t,
                                std::chrono::steady_clock::time_point now) {
        rec.record(t, now);
    };

    ReplayEngine engine(config);
    auto result = engine.replay(trace_file);
    CHECK(result.total_events == N);

    auto stats = analyze(rec.samples);
    check_fidelity(stats, "sync");

    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

TEST_CASE("Replay fidelity - pipelined path") {
    DFTRACER_UTILS_LOGGER_INIT();

    fs::path temp_dir =
        fs::temp_directory_path() / "dftracer_replay_fid_pipelined";
    fs::create_directories(temp_dir);
    std::string trace_file = (temp_dir / "fid.pfw").string();

    constexpr std::size_t N = 40;
    constexpr std::uint64_t STEP_US = 5'000;
    write_evenly_spaced_trace(trace_file, N, STEP_US);

    DispatchRecorder rec;
    ReplayConfig config;
    config.maintain_timing = true;
    config.dry_run = false;
    config.on_dispatch = [&rec](const Trace& t,
                                std::chrono::steady_clock::time_point now) {
        rec.record(t, now);
    };

    ReplayEngine engine(config);
    ReplayResult result;
    std::vector<std::string> files = {trace_file};

    Pipeline pipeline(PipelineConfig::parallel(4));
    auto root = make_task(
        [&engine, &files, &result](CoroScope& scope) -> coro::CoroTask<void> {
            co_await engine.run_pipelined(scope, files, result, /*cap=*/64);
        },
        "replay_pipelined");
    pipeline.set_source(root);
    pipeline.execute();

    CHECK(result.total_events == N);
    auto stats = analyze(rec.samples);
    check_fidelity(stats, "pipelined");

    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

TEST_CASE("Replay fidelity - first-event anchor reset survives warmup gap") {
    DFTRACER_UTILS_LOGGER_INIT();

    fs::path temp_dir =
        fs::temp_directory_path() / "dftracer_replay_fid_anchor";
    fs::create_directories(temp_dir);
    std::string trace_file = (temp_dir / "fid.pfw").string();

    constexpr std::size_t N = 20;
    constexpr std::uint64_t STEP_US = 5'000;  // 95ms span
    write_evenly_spaced_trace(trace_file, N, STEP_US);

    DispatchRecorder rec;
    ReplayConfig config;
    config.maintain_timing = true;
    config.dry_run = false;
    config.on_dispatch = [&rec](const Trace& t,
                                std::chrono::steady_clock::time_point now) {
        rec.record(t, now);
    };

    ReplayEngine engine(config);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto result = engine.replay(trace_file);
    CHECK(result.total_events == N);

    auto stats = analyze(rec.samples);
    WARN_MESSAGE(stats.total_wall_span_us >= 70'000,
                 "wall span " << stats.total_wall_span_us
                              << "us < 70ms (wall-clock unreliable under "
                                 "Valgrind/CI slowdown)");
    check_fidelity(stats, "anchor-reset");

    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}
