// Both pool paths must be exercisable in one suite run: an explicit
// with_elastic()/with_eager() pins the path regardless of the
// DFTRACER_UTILS_ELASTIC env, and an unset config resolves from that env. The
// live worker count right after construction is the observable (an elastic pool
// starts at its floor, an eager pool starts at the full width).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/runtime.h>
#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

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
    return p.runtime().get_progress().workers.size();
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

}  // namespace

TEST_SUITE("PipelineElasticity") {
    TEST_CASE("explicit path wins over the env") {
        SUBCASE("with_elastic under an eager env starts at the floor") {
            ElasticEnv env("0");
            Pipeline p(base().with_elastic());
            CHECK(live_workers(p) == 1);
        }
        SUBCASE("with_eager under an elastic env starts at full width") {
            ElasticEnv env("1");
            Pipeline p(base().with_eager());
            CHECK(live_workers(p) == eager_full_width());
        }
    }

    TEST_CASE("unset config resolves from the env default") {
        SUBCASE("env off -> eager") {
            ElasticEnv env("0");
            Pipeline p(base());
            CHECK(live_workers(p) == eager_full_width());
        }
        SUBCASE("env on -> elastic floor") {
            ElasticEnv env("1");
            Pipeline p(base());
            CHECK(live_workers(p) == 1);
        }
    }
}
