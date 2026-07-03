#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace logger = dftracer::utils::logger;
using logger::ColorMode;
using logger::Config;
using logger::Level;

namespace {

// Run fn() with the logger writing to an in-memory sink and return everything
// it emitted. Ambient logging env vars are cleared first so cfg alone drives
// behavior; a safe stderr sink is restored before the memory stream is closed.
template <typename F>
std::string capture_log(Config cfg, F&& fn) {
    ::unsetenv("DFTRACER_UTILS_LOG_LEVEL");
    ::unsetenv("DFTRACER_UTILS_LOG_COLOR");
    ::unsetenv("DFTRACER_UTILS_LOG_FILE");
    ::unsetenv("NO_COLOR");
    ::unsetenv("FORCE_COLOR");
    ::unsetenv("CLICOLOR_FORCE");

    char* buf = nullptr;
    std::size_t size = 0;
    std::FILE* fp = ::open_memstream(&buf, &size);
    REQUIRE(fp != nullptr);

    cfg.sink = fp;
    logger::init(cfg);
    fn();
    std::fflush(fp);
    std::string out(buf, size);

    logger::init(Config{});  // restore stderr sink before closing the stream
    std::fclose(fp);
    std::free(buf);
    return out;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("level gating suppresses below the threshold") {
    auto out =
        capture_log({.level = Level::Warn, .color = ColorMode::Never}, [] {
            DFTRACER_UTILS_LOG_DEBUG("dbg-line");
            DFTRACER_UTILS_LOG_INFO("inf-line");
            DFTRACER_UTILS_LOG_WARN("wrn-line");
            DFTRACER_UTILS_LOG_ERROR("err-line");
        });
    CHECK_FALSE(contains(out, "dbg-line"));
    CHECK_FALSE(contains(out, "inf-line"));
    CHECK(contains(out, "wrn-line"));
    CHECK(contains(out, "err-line"));
}

TEST_CASE("set_level / get_level roundtrip and runtime suppression") {
    logger::set_level(Level::Debug);
    CHECK(logger::get_level() == Level::Debug);
    logger::set_level(Level::Error);
    CHECK(logger::get_level() == Level::Error);

    auto out =
        capture_log({.level = Level::Trace, .color = ColorMode::Never}, [] {
            logger::set_level(Level::Error);
            DFTRACER_UTILS_LOG_INFO("hidden-line");
            DFTRACER_UTILS_LOG_ERROR("shown-line");
        });
    CHECK_FALSE(contains(out, "hidden-line"));
    CHECK(contains(out, "shown-line"));
}

TEST_CASE("color modes") {
    auto never = capture_log({.level = Level::Info, .color = ColorMode::Never},
                             [] { DFTRACER_UTILS_LOG_INFO("plain"); });
    CHECK_FALSE(contains(never, "\x1b["));

    auto always =
        capture_log({.level = Level::Info, .color = ColorMode::Always},
                    [] { DFTRACER_UTILS_LOG_INFO("colored"); });
    CHECK(contains(always, "\x1b["));
}

TEST_CASE("format: level tag, message, and location") {
    auto out = capture_log({.level = Level::Info, .color = ColorMode::Never},
                           [] { DFTRACER_UTILS_LOG_ERROR("boom %d", 42); });
    CHECK(contains(out, "ERROR"));
    CHECK(contains(out, "boom 42"));
    CHECK(contains(out, "test_logging.cpp:"));  // (basename:line)
}

TEST_CASE("show_location=false drops the file:line suffix") {
    auto out = capture_log({.level = Level::Info,
                            .color = ColorMode::Never,
                            .show_location = false},
                           [] { DFTRACER_UTILS_LOG_INFO("no-loc-line"); });
    CHECK(contains(out, "no-loc-line"));
    CHECK_FALSE(contains(out, "test_logging.cpp"));
}

TEST_CASE("env DFTRACER_UTILS_LOG_LEVEL overrides the config") {
    ::setenv("DFTRACER_UTILS_LOG_LEVEL", "error", 1);
    logger::init(Config{.level = Level::Debug});  // env should win
    CHECK(logger::get_level() == Level::Error);

    ::unsetenv("DFTRACER_UTILS_LOG_LEVEL");
    logger::init(Config{.level = Level::Info});
    CHECK(logger::get_level() == Level::Info);
}

#if DFTRACER_UTILS_LOGGER_TRACE_ENABLED
static void inner_scope() { DFTRACER_UTILS_TRACE_SCOPE("k=%d", 7); }

TEST_CASE("TRACE_SCOPE emits nested enter/exit with timing") {
    auto out =
        capture_log({.level = Level::Trace, .color = ColorMode::Never}, [] {
            DFTRACER_UTILS_TRACE_SCOPE();  // label = func name
            inner_scope();
        });
    CHECK(contains(out, "-> "));
    CHECK(contains(out, "<- "));
    CHECK(contains(out, "ms]"));  // exit carries a duration
    CHECK(contains(out, "k=7"));  // labeled inner scope
    // Flat output (no indentation): the inner scope's enter follows the
    // outer's.
    auto outer = out.find("-> ");
    auto inner = out.find("k=7");
    CHECK(inner != std::string::npos);
    CHECK(inner > outer);
}

TEST_CASE("TRACE_SCOPE is silent when Trace is disabled") {
    auto out =
        capture_log({.level = Level::Info, .color = ColorMode::Never}, [] {
            DFTRACER_UTILS_TRACE_SCOPE("should-not-appear");
            DFTRACER_UTILS_LOG_INFO("visible-line");
        });
    CHECK_FALSE(contains(out, "-> "));
    CHECK_FALSE(contains(out, "should-not-appear"));
    CHECK(contains(out, "visible-line"));
}

static dftracer::utils::coro::CoroTask<int> traced_leaf(int x) {
    DFTRACER_UTILS_TRACE_SCOPE("leaf x=%d", x);
    co_return x * 2;
}

static dftracer::utils::coro::CoroTask<int> traced_root() {
    // The ScopeTracer lives in this coroutine frame across the co_await
    // suspensions below; on GCC 12/13 a large frame local here would corrupt.
    DFTRACER_UTILS_TRACE_SCOPE("root");
    int a = co_await traced_leaf(10);
    int b = co_await traced_leaf(20);
    co_return a + b;
}

TEST_CASE("TRACE_SCOPE is frame-safe across coroutine suspension") {
    auto out =
        capture_log({.level = Level::Trace, .color = ColorMode::Never}, [] {
            dftracer::utils::Runtime rt(2);
            int r = rt.submit(traced_root(), "traced").get();
            CHECK(r == 60);  // (10*2) + (20*2); wrong value => frame corruption
        });
    CHECK(contains(out, "-> traced_root: root"));
    CHECK(contains(out, "<- traced_root: root"));
    CHECK(contains(out, "leaf x=10"));
    CHECK(contains(out, "leaf x=20"));
}
#endif  // DFTRACER_UTILS_LOGGER_TRACE_ENABLED
