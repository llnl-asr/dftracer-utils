// Test-only library that counts the path-based stat-family and open-family
// calls of the process it is injected into (DYLD_INSERT_LIBRARIES on macOS,
// LD_PRELOAD on Linux). At exit it writes "<class>\t<count>\t<path>" lines to
// the file named by DFTRACER_UTILS_TEST_SYSCALL_LOG. fd-based fstat is not
// counted. Paths are recorded as the caller gave them.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#ifdef __linux__
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/syscall.h>
#endif

namespace {

enum class CallClass { Stat, Open };

std::atomic<bool> g_ready{false};
thread_local bool t_in_hook = false;

std::mutex& counts_mutex() {
    static std::mutex* m = new std::mutex();
    return *m;
}

std::map<std::pair<int, std::string>, unsigned long>& counts() {
    static auto* c = new std::map<std::pair<int, std::string>, unsigned long>();
    return *c;
}

void record(CallClass cls, const char* path) {
    if (!g_ready.load(std::memory_order_acquire) || t_in_hook ||
        path == nullptr || path[0] == '\0')
        return;
    t_in_hook = true;
    {
        std::lock_guard<std::mutex> lock(counts_mutex());
        ++counts()[{static_cast<int>(cls), std::string(path)}];
    }
#ifdef __linux__
    // Debug aid: DFTRACER_UTILS_TEST_SYSCALL_BACKTRACE=<substring> prints the
    // call stack of every counted call whose path contains the substring.
    static const char* const trace_substr =
        std::getenv("DFTRACER_UTILS_TEST_SYSCALL_BACKTRACE");
    if (trace_substr != nullptr && std::strstr(path, trace_substr) != nullptr) {
        void* frames[48];
        const int n = backtrace(frames, 48);
        dprintf(2, "SYSCALL %s %s\n", cls == CallClass::Stat ? "stat" : "open",
                path);
        backtrace_symbols_fd(frames, n, 2);
    }
#endif
    t_in_hook = false;
}

__attribute__((constructor)) void interpose_init() {
    counts_mutex();
    counts();
    g_ready.store(true, std::memory_order_release);
}

__attribute__((destructor)) void interpose_fini() {
    g_ready.store(false, std::memory_order_release);
    const char* log = std::getenv("DFTRACER_UTILS_TEST_SYSCALL_LOG");
    if (log == nullptr || log[0] == '\0') return;
    t_in_hook = true;
    FILE* f = std::fopen(log, "w");
    if (f != nullptr) {
        std::lock_guard<std::mutex> lock(counts_mutex());
        for (const auto& kv : counts()) {
            std::fprintf(f, "%s\t%lu\t%s\n",
                         kv.first.first == static_cast<int>(CallClass::Stat)
                             ? "stat"
                             : "open",
                         kv.second, kv.first.second.c_str());
        }
        std::fclose(f);
    }
}

}  // namespace

#ifdef __APPLE__

#define DFTU_INTERPOSE(replacement, original)              \
    __attribute__((used)) static const struct {            \
        const void* repl;                                  \
        const void* orig;                                  \
    } dftu_interpose_##replacement                         \
        __attribute__((section("__DATA,__interpose"))) = { \
            reinterpret_cast<const void*>(replacement),    \
            reinterpret_cast<const void*>(original)}

extern "C" {

static int hook_stat(const char* p, struct stat* b) {
    record(CallClass::Stat, p);
    return ::stat(p, b);
}
static int hook_lstat(const char* p, struct stat* b) {
    record(CallClass::Stat, p);
    return ::lstat(p, b);
}
static int hook_fstatat(int fd, const char* p, struct stat* b, int flag) {
    record(CallClass::Stat, p);
    return ::fstatat(fd, p, b, flag);
}
static int hook_access(const char* p, int mode) {
    record(CallClass::Stat, p);
    return ::access(p, mode);
}
static int hook_faccessat(int fd, const char* p, int mode, int flag) {
    record(CallClass::Stat, p);
    return ::faccessat(fd, p, mode, flag);
}
static char* hook_realpath(const char* p, char* out) {
    record(CallClass::Stat, p);
    return ::realpath(p, out);
}
static int hook_open(const char* p, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    record(CallClass::Open, p);
    return ::open(p, flags, mode);
}
static int hook_openat(int fd, const char* p, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    record(CallClass::Open, p);
    return ::openat(fd, p, flags, mode);
}
static FILE* hook_fopen(const char* p, const char* m) {
    record(CallClass::Open, p);
    return ::fopen(p, m);
}

DFTU_INTERPOSE(hook_stat,
               static_cast<int (*)(const char*, struct stat*)>(::stat));
DFTU_INTERPOSE(hook_lstat,
               static_cast<int (*)(const char*, struct stat*)>(::lstat));
DFTU_INTERPOSE(hook_fstatat, ::fstatat);
DFTU_INTERPOSE(hook_access, ::access);
DFTU_INTERPOSE(hook_faccessat, ::faccessat);
DFTU_INTERPOSE(hook_realpath, ::realpath);
DFTU_INTERPOSE(hook_open, ::open);
DFTU_INTERPOSE(hook_openat, ::openat);
DFTU_INTERPOSE(hook_fopen, ::fopen);

}  // extern "C"

#else  // Linux

namespace {

template <class F>
F next_symbol(const char* name) {
    return reinterpret_cast<F>(dlsym(RTLD_NEXT, name));
}

#define DFTU_NEXT(type, name)                 \
    static type fn = next_symbol<type>(name); \
    if (fn == nullptr) {                      \
        errno = ENOSYS;                       \
        return fail;                          \
    }

using StatFn = int (*)(const char*, void*);
using XStatFn = int (*)(int, const char*, void*);
using FStatAtFn = int (*)(int, const char*, void*, int);
using XFStatAtFn = int (*)(int, int, const char*, void*, int);
using StatxFn = int (*)(int, const char*, int, unsigned, void*);
using AccessFn = int (*)(const char*, int);
using FAccessAtFn = int (*)(int, const char*, int, int);
using RealpathFn = char* (*)(const char*, char*);
using CanonFn = char* (*)(const char*);
using OpenFn = int (*)(const char*, int, ...);
using OpenAtFn = int (*)(int, const char*, int, ...);
using FopenFn = FILE* (*)(const char*, const char*);

mode_t fetch_mode(int flags, va_list& ap) {
    if (flags & O_CREAT) return static_cast<mode_t>(va_arg(ap, int));
    return 0;
}

}  // namespace

#define DFTU_STAT_HOOK(hook, sym)                             \
    extern "C" int hook(const char* p, void* b) __asm__(sym); \
    extern "C" int hook(const char* p, void* b) {             \
        const int fail = -1;                                  \
        DFTU_NEXT(StatFn, sym)                                \
        record(CallClass::Stat, p);                           \
        return fn(p, b);                                      \
    }

#define DFTU_XSTAT_HOOK(hook, sym)                                   \
    extern "C" int hook(int v, const char* p, void* b) __asm__(sym); \
    extern "C" int hook(int v, const char* p, void* b) {             \
        const int fail = -1;                                         \
        DFTU_NEXT(XStatFn, sym)                                      \
        record(CallClass::Stat, p);                                  \
        return fn(v, p, b);                                          \
    }

#define DFTU_FSTATAT_HOOK(hook, sym)                                          \
    extern "C" int hook(int fd, const char* p, void* b, int fl) __asm__(sym); \
    extern "C" int hook(int fd, const char* p, void* b, int fl) {             \
        const int fail = -1;                                                  \
        DFTU_NEXT(FStatAtFn, sym)                                             \
        record(CallClass::Stat, p);                                           \
        return fn(fd, p, b, fl);                                              \
    }

#define DFTU_XFSTATAT_HOOK(hook, sym)                                    \
    extern "C" int hook(int v, int fd, const char* p, void* b,           \
                        int fl) __asm__(sym);                            \
    extern "C" int hook(int v, int fd, const char* p, void* b, int fl) { \
        const int fail = -1;                                             \
        DFTU_NEXT(XFStatAtFn, sym)                                       \
        record(CallClass::Stat, p);                                      \
        return fn(v, fd, p, b, fl);                                      \
    }

DFTU_STAT_HOOK(dftu_stat, "stat")
DFTU_STAT_HOOK(dftu_lstat, "lstat")
DFTU_STAT_HOOK(dftu_stat64, "stat64")
DFTU_STAT_HOOK(dftu_lstat64, "lstat64")
DFTU_XSTAT_HOOK(dftu_xstat, "__xstat")
DFTU_XSTAT_HOOK(dftu_lxstat, "__lxstat")
DFTU_XSTAT_HOOK(dftu_xstat64, "__xstat64")
DFTU_XSTAT_HOOK(dftu_lxstat64, "__lxstat64")
DFTU_FSTATAT_HOOK(dftu_fstatat, "fstatat")
DFTU_FSTATAT_HOOK(dftu_fstatat64, "fstatat64")
DFTU_XFSTATAT_HOOK(dftu_fxstatat, "__fxstatat")
DFTU_XFSTATAT_HOOK(dftu_fxstatat64, "__fxstatat64")

extern "C" int dftu_statx(int fd, const char* p, int fl, unsigned mask,
                          void* b) __asm__("statx");
extern "C" int dftu_statx(int fd, const char* p, int fl, unsigned mask,
                          void* b) {
    const int fail = -1;
    DFTU_NEXT(StatxFn, "statx")
    record(CallClass::Stat, p);
    return fn(fd, p, fl, mask, b);
}

extern "C" int dftu_access(const char* p, int mode) __asm__("access");
extern "C" int dftu_access(const char* p, int mode) {
    const int fail = -1;
    DFTU_NEXT(AccessFn, "access")
    record(CallClass::Stat, p);
    return fn(p, mode);
}

extern "C" int dftu_faccessat(int fd, const char* p, int mode,
                              int fl) __asm__("faccessat");
extern "C" int dftu_faccessat(int fd, const char* p, int mode, int fl) {
    const int fail = -1;
    DFTU_NEXT(FAccessAtFn, "faccessat")
    record(CallClass::Stat, p);
    return fn(fd, p, mode, fl);
}

extern "C" char* dftu_realpath(const char* p, char* out) __asm__("realpath");
extern "C" char* dftu_realpath(const char* p, char* out) {
    char* const fail = nullptr;
    DFTU_NEXT(RealpathFn, "realpath")
    record(CallClass::Stat, p);
    return fn(p, out);
}

extern "C" char* dftu_canon(const char* p) __asm__("canonicalize_file_name");
extern "C" char* dftu_canon(const char* p) {
    char* const fail = nullptr;
    DFTU_NEXT(CanonFn, "canonicalize_file_name")
    record(CallClass::Stat, p);
    return fn(p);
}

#define DFTU_OPEN_HOOK(hook, sym)                                    \
    extern "C" int hook(const char* p, int flags, ...) __asm__(sym); \
    extern "C" int hook(const char* p, int flags, ...) {             \
        const int fail = -1;                                         \
        DFTU_NEXT(OpenFn, sym)                                       \
        va_list ap;                                                  \
        va_start(ap, flags);                                         \
        const mode_t mode = fetch_mode(flags, ap);                   \
        va_end(ap);                                                  \
        record(CallClass::Open, p);                                  \
        return fn(p, flags, mode);                                   \
    }

#define DFTU_OPENAT_HOOK(hook, sym)                                          \
    extern "C" int hook(int fd, const char* p, int flags, ...) __asm__(sym); \
    extern "C" int hook(int fd, const char* p, int flags, ...) {             \
        const int fail = -1;                                                 \
        DFTU_NEXT(OpenAtFn, sym)                                             \
        va_list ap;                                                          \
        va_start(ap, flags);                                                 \
        const mode_t mode = fetch_mode(flags, ap);                           \
        va_end(ap);                                                          \
        record(CallClass::Open, p);                                          \
        return fn(fd, p, flags, mode);                                       \
    }

DFTU_OPEN_HOOK(dftu_open, "open")
DFTU_OPEN_HOOK(dftu_open64, "open64")
DFTU_OPENAT_HOOK(dftu_openat, "openat")
DFTU_OPENAT_HOOK(dftu_openat64, "openat64")

#define DFTU_FOPEN_HOOK(hook, sym)                                    \
    extern "C" FILE* hook(const char* p, const char* m) __asm__(sym); \
    extern "C" FILE* hook(const char* p, const char* m) {             \
        FILE* const fail = nullptr;                                   \
        DFTU_NEXT(FopenFn, sym)                                       \
        record(CallClass::Open, p);                                   \
        return fn(p, m);                                              \
    }

DFTU_FOPEN_HOOK(dftu_fopen, "fopen")
DFTU_FOPEN_HOOK(dftu_fopen64, "fopen64")

#endif
