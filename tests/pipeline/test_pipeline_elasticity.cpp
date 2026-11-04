// SPDX-License-Identifier: MIT
// Every pipeline runs on the one process runtime. An explicit
// with_elastic()/with_eager() sets the mode of its pool, a pipeline that sets
// none keeps it, and DFTRACER_UTILS_ELASTIC decides the mode of a new runtime.
// The observable is the live worker count.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/runtime/pipeline/pipeline.h>
#include <dftracer/utils/runtime/pipeline/pipeline_config.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

using namespace dftracer::utils;

namespace {

constexpr std::size_t THREADS = 4;

// Save and restore DFTRACER_UTILS_ELASTIC so a case setting it does not leak
// into the next (the test preset sets it process-wide).
class ElasticEnv {
   public:
    explicit ElasticEnv(const char* value) {
        if (const char* prev = std::getenv("DFTRACER_UTILS_ELASTIC")) {
            had_ = true;
            prev_ = prev;
        }
        if (value)
            setenv("DFTRACER_UTILS_ELASTIC", value, 1);
        else
            unsetenv("DFTRACER_UTILS_ELASTIC");
    }
    ~ElasticEnv() {
        if (had_)
            setenv("DFTRACER_UTILS_ELASTIC", prev_.c_str(), 1);
        else
            unsetenv("DFTRACER_UTILS_ELASTIC");
    }

   private:
    bool had_ = false;
    std::string prev_;
};

std::size_t live_workers(const Pipeline& p) {
    return p.runtime().progress().workers.size();
}

// An eager pool starts at its full compute width. Nothing clamps that to the
// core count - a pool asked for THREADS workers runs THREADS of them even on a
// host with fewer cores (blocking coroutines need a real thread each). Valgrind
// mode is the one exception: it caps the pool at 2 to bound thread churn.
std::size_t eager_full_width() {
#ifdef DFTRACER_UTILS_VALGRIND_MODE
    return THREADS < 2 ? THREADS : 2;
#else
    return THREADS;
#endif
}

PipelineConfig base() {
    return PipelineConfig().with_compute_threads(THREADS).with_watchdog(false);
}

// Waits up to five seconds for the pool to reach `want` live workers.
bool settles_at(const Pipeline& p, std::size_t want) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (live_workers(p) != want) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

// In a fork child, which makes its own runtime: the live workers of the first
// pipeline under `env`, as the exit status.
std::size_t first_pipeline_workers(const char* env) {
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ElasticEnv set(env);
        Pipeline p(base());
        ::_exit(static_cast<int>(live_workers(p)));
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    return static_cast<std::size_t>(WEXITSTATUS(status));
}

}  // namespace

TEST_SUITE("PipelineElasticity") {
    TEST_CASE("with_eager runs the full width and unset keeps it") {
        Pipeline eager(base().with_eager());
        CHECK(live_workers(eager) == eager_full_width());
        Pipeline unset(base());
        CHECK(live_workers(unset) == eager_full_width());
    }

    TEST_CASE("with_elastic lets an eager pool settle at its floor") {
        Pipeline eager(base().with_eager());
        REQUIRE(live_workers(eager) == eager_full_width());
        Pipeline elastic(base().with_elastic());
        CHECK(settles_at(elastic, 1));
    }

    TEST_CASE("the env decides the mode of a new runtime") {
#ifdef DFTRACER_UTILS_TSAN_ENABLED
        // ThreadSanitizer stops a child of a multi-threaded fork that starts
        // a thread, which this child must.
        MESSAGE("not run under ThreadSanitizer");
        return;
#endif
        CHECK(first_pipeline_workers("0") == eager_full_width());
        CHECK(first_pipeline_workers("1") == 1);
    }
}
