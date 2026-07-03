#ifndef DFTRACER_UTILS_CORE_COMMON_LOGGING_H
#define DFTRACER_UTILS_CORE_COMMON_LOGGING_H

#include <dftracer/utils/core/common/config.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <optional>
#include <string_view>

namespace dftracer::utils::logger {

// Severity, ordered low to high. Off is a threshold only (no Off messages).
enum class Level : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};

enum class ColorMode { Auto, Always, Never };

struct Config {
    Level level = Level::Info;
    ColorMode color = ColorMode::Auto;
    std::FILE* sink = nullptr;  // nullptr -> stderr
    bool show_location = true;
};

// Configure the logger. Environment overrides are read first:
// DFTRACER_UTILS_LOG_LEVEL (trace|debug|info|warn|error|off), and the color
// conventions NO_COLOR / FORCE_COLOR / CLICOLOR_FORCE. Then cfg is applied.
// Call once at startup.
void init(Config cfg = {});

void set_level(Level level);
Level get_level();
void set_color(ColorMode mode);

// Name <-> Level. level_from_name accepts trace|debug|info|warn|warning|error|
// off|none and returns nullopt if unrecognized; level_name returns lowercase.
std::optional<Level> level_from_name(std::string_view name);
const char* level_name(Level level);

namespace detail {

// Current runtime threshold; a message at Level L is emitted when L >= g_level.
extern std::atomic<int> g_level;

inline bool enabled(Level lvl) noexcept {
    return static_cast<int>(lvl) >= g_level.load(std::memory_order_relaxed);
}

// Emit one formatted line. Thread-safe without a lock: the whole line is built
// locally and written in a single call (stdio per-stream lock).
__attribute__((__format__(__printf__, 4, 5))) void write(Level lvl,
                                                         const char* file,
                                                         int line,
                                                         const char* fmt, ...);

// Open/close a trace scope. The returned handle lives in the ScopeTracer object
// so it follows a coroutine across thread migration (a thread-local stack
// would not).
void* scope_open(const char* file, int line, const char* label);
void scope_close(void* handle);

// Auto coroutine tracing, driven from the CoroTask awaiter. enter reads the
// coroutine's resume function from `handle`, logs "-> <name>" with the
// definition-site location (file/line captured at get_return_object), and
// returns an opaque handle (null when Trace is off); leave logs "<- <name>
// [ms]".
void* coro_trace_enter(const void* handle, const char* file, int line);
void coro_trace_leave(void* handle);

// RAII scope tracer for DFTRACER_UTILS_TRACE_SCOPE. Holds only a pointer-sized
// handle, so it is safe in a coroutine frame on GCC 12/13 (which corrupts large
// non-trivial frame locals). Near-free unless Trace is enabled.
class ScopeTracer {
   public:
    ScopeTracer(const char* file, int line, const char* func) {
        if (enabled(Level::Trace)) handle_ = scope_open(file, line, func);
    }

    __attribute__((__format__(__printf__, 5, 6))) ScopeTracer(
        const char* file, int line, const char* func, const char* fmt, ...) {
        if (!enabled(Level::Trace)) return;
        char label[256];
        int off = std::snprintf(label, sizeof(label), "%s: ", func);
        if (off < 0) off = 0;
        if (static_cast<std::size_t>(off) < sizeof(label)) {
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(label + off, sizeof(label) - off, fmt, ap);
            va_end(ap);
        }
        handle_ = scope_open(file, line, label);
    }

    ~ScopeTracer() {
        if (handle_) scope_close(handle_);
    }

    ScopeTracer(const ScopeTracer&) = delete;
    ScopeTracer& operator=(const ScopeTracer&) = delete;

   private:
    void* handle_ = nullptr;
};

}  // namespace detail
}  // namespace dftracer::utils::logger

// Emission macros: compile-gated by DFTRACER_UTILS_LOGGER_LEVEL_* and runtime-
// gated by the current level. A disabled level costs nothing (compiled out) or
// one predicted branch with its arguments left unevaluated. Do not put
// DEBUG/TRACE in a per-event/per-byte inner loop; guard those with #if instead.
#define DFTRACER_UTILS_LOG_IMPL(LVL, ...)                                     \
    do {                                                                      \
        if (::dftracer::utils::logger::detail::enabled(LVL)) [[unlikely]] {   \
            ::dftracer::utils::logger::detail::write(LVL, __FILE__, __LINE__, \
                                                     __VA_ARGS__);            \
        }                                                                     \
    } while (0)

#define DFTRACER_UTILS_DETAIL_CONCAT_(a, b) a##b
#define DFTRACER_UTILS_DETAIL_CONCAT(a, b) DFTRACER_UTILS_DETAIL_CONCAT_(a, b)

#if defined(DFTRACER_UTILS_LOGGER_LEVEL_TRACE) && \
    (DFTRACER_UTILS_LOGGER_LEVEL_TRACE == 1)
#define DFTRACER_UTILS_LOGGER_TRACE_ENABLED 1
#define DFTRACER_UTILS_LOG_TRACE(...)                                \
    DFTRACER_UTILS_LOG_IMPL(::dftracer::utils::logger::Level::Trace, \
                            __VA_ARGS__)
// Trace the enclosing scope ("-> label" on entry, "<- label [ms]" on exit),
// only when Trace is enabled. Label defaults to the function name; extra args
// are printf-appended: DFTRACER_UTILS_TRACE_SCOPE("off=%zu", off).
#define DFTRACER_UTILS_TRACE_SCOPE(...)                       \
    ::dftracer::utils::logger::detail::ScopeTracer            \
    DFTRACER_UTILS_DETAIL_CONCAT(dft_trace_scope_, __LINE__)( \
        __FILE__, __LINE__,                                   \
        static_cast<const char*>(__func__) __VA_OPT__(, ) __VA_ARGS__)
#else
#define DFTRACER_UTILS_LOGGER_TRACE_ENABLED 0
#define DFTRACER_UTILS_LOG_TRACE(...)
#define DFTRACER_UTILS_TRACE_SCOPE(...)
#endif
#define DFTRACER_UTILS_LOG_TRACE_FORMAT(...) \
    DFTRACER_UTILS_LOG_TRACE(__VA_ARGS__)

#if defined(DFTRACER_UTILS_LOGGER_LEVEL_DEBUG) && \
    (DFTRACER_UTILS_LOGGER_LEVEL_DEBUG == 1)
#define DFTRACER_UTILS_LOGGER_DEBUG_ENABLED 1
#define DFTRACER_UTILS_LOG_DEBUG(...)                                \
    DFTRACER_UTILS_LOG_IMPL(::dftracer::utils::logger::Level::Debug, \
                            __VA_ARGS__)
#else
#define DFTRACER_UTILS_LOGGER_DEBUG_ENABLED 0
#define DFTRACER_UTILS_LOG_DEBUG(...)
#endif

#if defined(DFTRACER_UTILS_LOGGER_LEVEL_INFO) && \
    (DFTRACER_UTILS_LOGGER_LEVEL_INFO == 1)
#define DFTRACER_UTILS_LOGGER_INFO_ENABLED 1
#define DFTRACER_UTILS_LOG_INFO(...) \
    DFTRACER_UTILS_LOG_IMPL(::dftracer::utils::logger::Level::Info, __VA_ARGS__)
#else
#define DFTRACER_UTILS_LOGGER_INFO_ENABLED 0
#define DFTRACER_UTILS_LOG_INFO(...)
#endif

#if defined(DFTRACER_UTILS_LOGGER_LEVEL_WARN) && \
    (DFTRACER_UTILS_LOGGER_LEVEL_WARN == 1)
#define DFTRACER_UTILS_LOGGER_WARN_ENABLED 1
#define DFTRACER_UTILS_LOG_WARN(...) \
    DFTRACER_UTILS_LOG_IMPL(::dftracer::utils::logger::Level::Warn, __VA_ARGS__)
#else
#define DFTRACER_UTILS_LOGGER_WARN_ENABLED 0
#define DFTRACER_UTILS_LOG_WARN(...)
#endif

#if defined(DFTRACER_UTILS_LOGGER_LEVEL_ERROR) && \
    (DFTRACER_UTILS_LOGGER_LEVEL_ERROR == 1)
#define DFTRACER_UTILS_LOGGER_ERROR_ENABLED 1
#define DFTRACER_UTILS_LOG_ERROR(...)                                \
    DFTRACER_UTILS_LOG_IMPL(::dftracer::utils::logger::Level::Error, \
                            __VA_ARGS__)
#else
#define DFTRACER_UTILS_LOGGER_ERROR_ENABLED 0
#define DFTRACER_UTILS_LOG_ERROR(...)
#endif

// Unconditional line to stdout (no level gate, no decoration).
#define DFTRACER_UTILS_LOG_PRINT(...) std::fprintf(stdout, __VA_ARGS__)

#define DFTRACER_UTILS_LOG_STDOUT_REDIRECT(fpath) \
    std::freopen((fpath), "a+", stdout)
#define DFTRACER_UTILS_LOG_STDERR_REDIRECT(fpath) \
    std::freopen((fpath), "a+", stderr)

#endif  // DFTRACER_UTILS_CORE_COMMON_LOGGING_H
