// SPDX-License-Identifier: MIT
// Counts the path-based stat and open calls of the process it is injected into
// (DYLD_INSERT_LIBRARIES on macOS, LD_PRELOAD on Linux), and the bytes that
// read and pread return for each opened path. At exit it writes
// "<class>\t<count>\t<path>" lines to DFTRACER_UTILS_TEST_SYSCALL_LOG, with
// the class "bytes" for the bytes read. fd-based fstat is not counted.
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

enum class CallClass { Stat, Open, Bytes };

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

std::map<int, std::string>& fd_paths() {
    static auto* m = new std::map<int, std::string>();
    return *m;
}

// The thread-local guard is not usable before the library is initialized, and
// the process closes and reads files then, so each of these checks g_ready
// first, in a caller that touches no thread-local.
__attribute__((noinline)) void track_open_ready(int fd, const char* path) {
    if (t_in_hook || path == nullptr) return;
    t_in_hook = true;
    {
        std::lock_guard<std::mutex> lock(counts_mutex());
        fd_paths()[fd] = path;
    }
    t_in_hook = false;
}

__attribute__((noinline)) void track_close_ready(int fd) {
    if (t_in_hook) return;
    t_in_hook = true;
    {
        std::lock_guard<std::mutex> lock(counts_mutex());
        fd_paths().erase(fd);
    }
    t_in_hook = false;
}

__attribute__((noinline)) void count_bytes_ready(int fd, ssize_t n) {
    if (t_in_hook) return;
    t_in_hook = true;
    {
        std::lock_guard<std::mutex> lock(counts_mutex());
        const auto it = fd_paths().find(fd);
        if (it != fd_paths().end())
            counts()[{static_cast<int>(CallClass::Bytes), it->second}] +=
                static_cast<unsigned long>(n);
    }
    t_in_hook = false;
}

void track_open(int fd, const char* path) {
    if (fd >= 0 && g_ready.load(std::memory_order_acquire))
        track_open_ready(fd, path);
}

void track_close(int fd) {
    if (g_ready.load(std::memory_order_acquire)) track_close_ready(fd);
}

void count_bytes(int fd, ssize_t n) {
    if (n > 0 && g_ready.load(std::memory_order_acquire))
        count_bytes_ready(fd, n);
}

__attribute__((constructor)) void interpose_init() {
    fd_paths();
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
            const int cls = kv.first.first;
            std::fprintf(f, "%s\t%lu\t%s\n",
                         cls == static_cast<int>(CallClass::Stat)   ? "stat"
                         : cls == static_cast<int>(CallClass::Open) ? "open"
                                                                    : "bytes",
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
    const int fd = ::open(p, flags, mode);
    track_open(fd, p);
    return fd;
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
    const int opened = ::openat(fd, p, flags, mode);
    track_open(opened, p);
    return opened;
}
static ssize_t hook_pread(int fd, void* b, size_t n, off_t off) {
    const ssize_t got = ::pread(fd, b, n, off);
    count_bytes(fd, got);
    return got;
}
static ssize_t hook_read(int fd, void* b, size_t n) {
    const ssize_t got = ::read(fd, b, n);
    count_bytes(fd, got);
    return got;
}
static int hook_close(int fd) {
    track_close(fd);
    return ::close(fd);
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
DFTU_INTERPOSE(hook_pread, ::pread);
DFTU_INTERPOSE(hook_read, ::read);
DFTU_INTERPOSE(hook_close, ::close);
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
using PreadFn = ssize_t (*)(int, void*, size_t, off_t);
using ReadFn = ssize_t (*)(int, void*, size_t);
using CloseFn = int (*)(int);

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
        const int opened = fn(p, flags, mode);                       \
        track_open(opened, p);                                       \
        return opened;                                               \
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
        const int opened = fn(fd, p, flags, mode);                           \
        track_open(opened, p);                                               \
        return opened;                                                       \
    }

DFTU_OPEN_HOOK(dftu_open, "open")
DFTU_OPEN_HOOK(dftu_open64, "open64")
DFTU_OPENAT_HOOK(dftu_openat, "openat")
DFTU_OPENAT_HOOK(dftu_openat64, "openat64")

#define DFTU_PREAD_HOOK(hook, sym)                                  \
    extern "C" ssize_t hook(int fd, void* b, size_t n,              \
                            off_t off) __asm__(sym);                \
    extern "C" ssize_t hook(int fd, void* b, size_t n, off_t off) { \
        const ssize_t fail = -1;                                    \
        DFTU_NEXT(PreadFn, sym)                                     \
        const ssize_t got = fn(fd, b, n, off);                      \
        count_bytes(fd, got);                                       \
        return got;                                                 \
    }

DFTU_PREAD_HOOK(dftu_pread, "pread")
DFTU_PREAD_HOOK(dftu_pread64, "pread64")

extern "C" ssize_t dftu_read(int fd, void* b, size_t n) __asm__("read");
extern "C" ssize_t dftu_read(int fd, void* b, size_t n) {
    const ssize_t fail = -1;
    DFTU_NEXT(ReadFn, "read")
    const ssize_t got = fn(fd, b, n);
    count_bytes(fd, got);
    return got;
}

extern "C" int dftu_close(int fd) __asm__("close");
extern "C" int dftu_close(int fd) {
    const int fail = -1;
    DFTU_NEXT(CloseFn, "close")
    track_close(fd);
    return fn(fd);
}

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
