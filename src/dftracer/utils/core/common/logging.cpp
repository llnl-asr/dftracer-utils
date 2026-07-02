#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/ptr_hash.h>
#include <dftracer/utils/core/common/symbolize.h>
#include <dftracer/utils/core/env.h>
#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace dftracer::utils::logger {

namespace detail {
std::atomic<int> g_level{static_cast<int>(Level::Info)};
}  // namespace detail

namespace {

std::FILE* g_sink = stderr;
std::atomic<bool> g_use_color{false};
bool g_show_location = true;

struct LevelStyle {
    const char* name;   // padded to width 5
    const char* color;  // ANSI SGR sequence
};

LevelStyle level_style(Level lvl) {
    switch (lvl) {
        case Level::Trace:
            return {"TRACE", "\x1b[90m"};    // bright black
        case Level::Debug:
            return {"DEBUG", "\x1b[36m"};    // cyan
        case Level::Info:
            return {"INFO ", "\x1b[32m"};    // green
        case Level::Warn:
            return {"WARN ", "\x1b[33m"};    // yellow
        case Level::Error:
            return {"ERROR", "\x1b[1;31m"};  // bold red
        case Level::Off:
            return {"OFF  ", ""};
    }
    return {"?????", ""};
}

Level parse_level(std::optional<std::string_view> env, Level fallback) {
    if (!env) return fallback;
    if (auto lvl = level_from_name(*env)) return *lvl;
    return fallback;
}

ColorMode parse_color_mode(std::optional<std::string_view> env,
                           ColorMode fallback) {
    if (!env) return fallback;
    const std::string_view s = *env;
    if (s == "always" || s == "1" || s == "on") return ColorMode::Always;
    if (s == "never" || s == "0" || s == "off") return ColorMode::Never;
    if (s == "auto") return ColorMode::Auto;
    return fallback;
}

bool resolve_color(ColorMode mode, std::FILE* sink) {
    if (mode == ColorMode::Always) return true;
    if (mode == ColorMode::Never) return false;
    // Auto: honor the common conventions, then fall back to TTY detection.
    if (Env::get<std::string_view>("NO_COLOR")) return false;
    if (Env::get<std::string_view>("FORCE_COLOR") ||
        Env::get<std::string_view>("CLICOLOR_FORCE"))
        return true;
    if (!isatty(fileno(sink))) return false;
    const auto term = Env::get<std::string_view>("TERM");
    if (term && *term == "dumb") return false;
    return true;
}

const char* base_name(const char* path) {
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void make_timestamp(char* out, std::size_t n) {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch())
                  .count() %
              1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&t, &tm);
    std::snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec, static_cast<int>(ms));
}

long long steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Per-scope state, heap-allocated so it follows a coroutine across threads.
struct ScopeFrame {
    std::string label;
    const char* file;
    int line;
    long long start_ns;
};

}  // namespace

void init(Config cfg) {
    // Environment overrides the programmatic config.
    cfg.level = parse_level(
        Env::get<std::string_view>("DFTRACER_UTILS_LOG_LEVEL"), cfg.level);
    cfg.color = parse_color_mode(
        Env::get<std::string_view>("DFTRACER_UTILS_LOG_COLOR"), cfg.color);

    std::FILE* sink = cfg.sink ? cfg.sink : stderr;
    if (const auto path = Env::get<std::string_view>("DFTRACER_UTILS_LOG_FILE");
        path && !path->empty()) {
        const std::string p(*path);
        if (std::FILE* fp = std::fopen(p.c_str(), "a")) sink = fp;
    }

    g_sink = sink;
    g_show_location = cfg.show_location;
    g_use_color.store(resolve_color(cfg.color, g_sink),
                      std::memory_order_relaxed);
    detail::g_level.store(static_cast<int>(cfg.level),
                          std::memory_order_relaxed);
}

void set_level(Level level) {
    detail::g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

Level get_level() {
    return static_cast<Level>(detail::g_level.load(std::memory_order_relaxed));
}

void set_color(ColorMode mode) {
    g_use_color.store(resolve_color(mode, g_sink), std::memory_order_relaxed);
}

std::optional<Level> level_from_name(std::string_view name) {
    if (name == "trace") return Level::Trace;
    if (name == "debug") return Level::Debug;
    if (name == "info") return Level::Info;
    if (name == "warn" || name == "warning") return Level::Warn;
    if (name == "error") return Level::Error;
    if (name == "off" || name == "none") return Level::Off;
    return std::nullopt;
}

const char* level_name(Level level) {
    switch (level) {
        case Level::Trace:
            return "trace";
        case Level::Debug:
            return "debug";
        case Level::Info:
            return "info";
        case Level::Warn:
            return "warn";
        case Level::Error:
            return "error";
        case Level::Off:
            return "off";
    }
    return "unknown";
}

namespace detail {

void write(Level lvl, const char* file, int line, const char* fmt, ...) {
    char msg[1536];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char ts[64];
    make_timestamp(ts, sizeof(ts));

    const LevelStyle style = level_style(lvl);
    const bool color = g_use_color.load(std::memory_order_relaxed);

    // No source location for coroutine traces (file == nullptr).
    const bool loc = g_show_location && file != nullptr;
    char out[2048];
    int n;
    if (color) {
        if (loc) {
            n = std::snprintf(out, sizeof(out),
                              "\x1b[2m[%s]\x1b[0m %s%s\x1b[0m %s "
                              "\x1b[2m(%s:%d)\x1b[0m\n",
                              ts, style.color, style.name, msg, base_name(file),
                              line);
        } else {
            n = std::snprintf(out, sizeof(out),
                              "\x1b[2m[%s]\x1b[0m %s%s\x1b[0m %s\n", ts,
                              style.color, style.name, msg);
        }
    } else {
        if (loc) {
            n = std::snprintf(out, sizeof(out), "[%s] %s %s (%s:%d)\n", ts,
                              style.name, msg, base_name(file), line);
        } else {
            n = std::snprintf(out, sizeof(out), "[%s] %s %s\n", ts, style.name,
                              msg);
        }
    }
    if (n < 0) return;
    std::size_t len = (static_cast<std::size_t>(n) < sizeof(out))
                          ? static_cast<std::size_t>(n)
                          : sizeof(out) - 1;
    std::fwrite(out, 1, len, g_sink);
    if (lvl == Level::Error) std::fflush(g_sink);  // survive a crash
}

void* scope_open(const char* file, int line, const char* label) {
    write(Level::Trace, file, line, "-> %s", label);
    return new ScopeFrame{std::string(label), file, line, steady_now_ns()};
}

void scope_close(void* handle) {
    auto* frame = static_cast<ScopeFrame*>(handle);
    const double ms =
        static_cast<double>(steady_now_ns() - frame->start_ns) / 1e6;
    write(Level::Trace, frame->file, frame->line, "<- %s [%.2f ms]",
          frame->label.c_str(), ms);
    delete frame;
}

namespace {

// Per-thread cache: many coroutine instances share one resume function, so the
// dladdr + demangle runs once per function per thread.
const std::string& ct_name(const void* fn) {
    static thread_local ankerl::unordered_dense::map<const void*, std::string,
                                                     PtrHash>
        cache;
    auto it = cache.find(fn);
    if (it != cache.end()) return it->second;
    return cache.emplace(fn, symbolize_function(fn)).first->second;
}

}  // namespace

void* coro_trace_enter(const void* handle, const char* file, int line) {
    if (!enabled(Level::Trace) || handle == nullptr) return nullptr;
    // The resume-function pointer sits at frame offset 0 (coroutine ABI).
    const void* resume_fn = *reinterpret_cast<const void* const*>(handle);
    return scope_open(file, line, ct_name(resume_fn).c_str());
}

void coro_trace_leave(void* handle) {
    if (handle) scope_close(handle);
}

}  // namespace detail
}  // namespace dftracer::utils::logger
