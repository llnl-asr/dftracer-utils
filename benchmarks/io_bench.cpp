// SPDX-License-Identifier: MIT
//
// Cost of one awaited I/O operation by thread count.
//
//   pread_4k     4x threads tasks read 4 KiB of a cached file in a loop.
//   pwrite_4k    4x threads tasks write 4 KiB at their own offsets in a loop.
//   open_close   4x threads tasks open and close one file in a loop.
//   lone_pread   one task reads, so that every read finds the ring or the pool
//                asleep.
//   busy_fsync   one task per thread submits an fsync and spins until it ends,
//                so no worker reaps its own ring.
//   stat_op      4x threads tasks stat one file with the statx operation.
//   stat_call    the same with a blocking stat on the thread pool.
//   mkdir_op     4x threads tasks make and remove their own directory with the
//                mkdir and rmdir operations.
//   mkdir_call   the same with blocking calls on the thread pool.
//
//   io_bench --threads N [--reps R] [--scale F] [--case NAME]
//   io_bench --smoke   (small sizes; exits nonzero on a failed operation)
//
// Each row prints the median ns per op. DFTRACER_UTILS_IO_BACKEND picks the
// backend.
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/io/ops.h>
#include <dftracer/utils/runtime/runtime.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dftracer::utils;

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t BLOCK = 4096;
constexpr std::size_t FILE_BLOCKS = 4096;

struct Config {
    std::size_t threads = 4;
    std::size_t scale_percent = 100;
    int reps = 5;
    const char* only = nullptr;
};

struct Shared {
    int fd = -1;
    const char* path = nullptr;
    std::atomic<std::uint64_t> failures{0};
};

std::size_t scaled(const Config& config, std::size_t n) {
    return std::max<std::size_t>(n * config.scale_percent / 100, 1);
}

coro::CoroTask<void> reader(Shared& shared, std::size_t rounds,
                            std::size_t seed) {
    char buffer[BLOCK];
    std::size_t block = seed;
    for (std::size_t i = 0; i < rounds; ++i) {
        block = (block * 31 + 7) % FILE_BLOCKS;
        const ssize_t got = co_await runtime::io::pread(shared.fd, buffer,
                                                        BLOCK, block * BLOCK);
        if (got != static_cast<ssize_t>(BLOCK)) {
            shared.failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

coro::CoroTask<void> writer(Shared& shared, std::size_t rounds,
                            std::size_t seed) {
    char buffer[BLOCK] = {};
    const std::size_t block = seed % FILE_BLOCKS;
    for (std::size_t i = 0; i < rounds; ++i) {
        const ssize_t put = co_await runtime::io::pwrite(shared.fd, buffer,
                                                         BLOCK, block * BLOCK);
        if (put != static_cast<ssize_t>(BLOCK)) {
            shared.failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

coro::CoroTask<void> opener(Shared& shared, std::size_t rounds) {
    for (std::size_t i = 0; i < rounds; ++i) {
        const ssize_t fd = co_await runtime::io::open(shared.path, O_RDONLY);
        if (fd < 0) {
            shared.failures.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const ssize_t closed =
            co_await runtime::io::close(static_cast<int>(fd));
        if (closed != 0) {
            shared.failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

#ifdef __linux__
coro::CoroTask<void> stat_op_task(Shared& shared, std::size_t rounds) {
    runtime::io::Statx info;
    for (std::size_t i = 0; i < rounds; ++i) {
        const ssize_t got = co_await runtime::io::statx(
            AT_FDCWD, shared.path, 0, runtime::io::STATX_BASIC_MASK, &info);
        if (got != 0) shared.failures.fetch_add(1, std::memory_order_relaxed);
    }
}
#endif

ssize_t blocking_stat(void* context) noexcept {
    struct stat info;
    return ::stat(static_cast<const char*>(context), &info);
}

coro::CoroTask<void> stat_call_task(Shared& shared, std::size_t rounds) {
    for (std::size_t i = 0; i < rounds; ++i) {
        const ssize_t got = co_await runtime::io::call(
            blocking_stat, const_cast<char*>(shared.path));
        if (got != 0) shared.failures.fetch_add(1, std::memory_order_relaxed);
    }
}

dstd::String task_dir(const Shared& shared, std::size_t seed) {
    return dstd::fmt<"{}.d{}">(shared.path, seed);
}

coro::CoroTask<void> mkdir_op_task(Shared& shared, std::size_t rounds,
                                   std::size_t seed) {
    const dstd::String name = task_dir(shared, seed);
    for (std::size_t i = 0; i < rounds; ++i) {
        const ssize_t made = co_await runtime::io::mkdir(name.c_str());
        const ssize_t removed = co_await runtime::io::rmdir(name.c_str());
        if (made != 0 || removed != 0) {
            shared.failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

ssize_t blocking_mkdir(void* context) noexcept {
    return ::mkdir(static_cast<const char*>(context), 0777);
}

ssize_t blocking_rmdir(void* context) noexcept {
    return ::rmdir(static_cast<const char*>(context));
}

coro::CoroTask<void> mkdir_call_task(Shared& shared, std::size_t rounds,
                                     std::size_t seed) {
    dstd::String name = task_dir(shared, seed);
    for (std::size_t i = 0; i < rounds; ++i) {
        const ssize_t made =
            co_await runtime::io::call(blocking_mkdir, name.data());
        const ssize_t removed =
            co_await runtime::io::call(blocking_rmdir, name.data());
        if (made != 0 || removed != 0) {
            shared.failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void busy_done(runtime::io::IoOp& op) noexcept {
    static_cast<std::atomic<ssize_t>*>(op.context)
        ->store(op.result, std::memory_order_release);
}

coro::CoroTask<void> busy_syncer(Shared& shared, runtime::io::IoBackend& io,
                                 std::size_t rounds) {
    constexpr ssize_t RUNNING = 1;
    for (std::size_t i = 0; i < rounds; ++i) {
        std::atomic<ssize_t> result{RUNNING};
        runtime::io::IoOp op;
        op.kind = runtime::io::IoOpKind::Fsync;
        op.handle = shared.fd;
        op.complete = &busy_done;
        op.context = &result;
        io.submit(op);
        io.flush();
        while (result.load(std::memory_order_acquire) == RUNNING) {
        }
        if (result.load(std::memory_order_relaxed) != 0) {
            shared.failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
    co_return;
}

using Run = std::uint64_t (*)(const Config&, Runtime&, Shared&);

template <class T>
void require_ok(const dstd::Expected<T, dstd::Error>& result,
                const char* what) {
    if (result.has_value()) return;
    std::fprintf(stderr, "%s failed: %s\n", what, result.error().c_str());
    std::exit(1);
}

template <typename Make>
std::uint64_t fan_out(Runtime& rt, std::size_t tasks, std::size_t rounds,
                      Make make) {
    require_ok(rt.block_on(coro::with_scope(
                   [&](coro::Scope& scope) -> coro::CoroTask<void> {
                       for (std::size_t i = 0; i < tasks; ++i)
                           scope.spawn(make(i));
                       co_return;
                   })),
               "fan_out");
    return tasks * rounds;
}

std::uint64_t pread_4k(const Config& config, Runtime& rt, Shared& shared) {
    const std::size_t rounds = scaled(config, 20000);
    return fan_out(rt, 4 * config.threads, rounds,
                   [&](std::size_t i) { return reader(shared, rounds, i); });
}

std::uint64_t pwrite_4k(const Config& config, Runtime& rt, Shared& shared) {
    const std::size_t rounds = scaled(config, 5000);
    return fan_out(rt, 4 * config.threads, rounds,
                   [&](std::size_t i) { return writer(shared, rounds, i); });
}

std::uint64_t open_close(const Config& config, Runtime& rt, Shared& shared) {
    const std::size_t rounds = scaled(config, 2000);
    return fan_out(rt, 4 * config.threads, rounds,
                   [&](std::size_t) { return opener(shared, rounds); }) *
           2;
}

std::uint64_t lone_pread(const Config& config, Runtime& rt, Shared& shared) {
    const std::size_t rounds = scaled(config, 50000);
    return fan_out(rt, 1, rounds,
                   [&](std::size_t) { return reader(shared, rounds, 1); });
}

std::uint64_t busy_fsync(const Config& config, Runtime& rt, Shared& shared) {
    const std::size_t rounds = scaled(config, 2000);
    runtime::io::IoBackend& io = rt.executor()->io_backend();
    return fan_out(rt, config.threads, rounds, [&](std::size_t) {
        return busy_syncer(shared, io, rounds);
    });
}

#ifdef __linux__
std::uint64_t stat_op(const Config& config, Runtime& rt, Shared& shared) {
    const std::size_t rounds = scaled(config, 5000);
    return fan_out(rt, 4 * config.threads, rounds,
                   [&](std::size_t) { return stat_op_task(shared, rounds); });
}
#endif

std::uint64_t stat_call(const Config& config, Runtime& rt, Shared& shared) {
    const std::size_t rounds = scaled(config, 5000);
    return fan_out(rt, 4 * config.threads, rounds,
                   [&](std::size_t) { return stat_call_task(shared, rounds); });
}

std::uint64_t mkdir_op(const Config& config, Runtime& rt, Shared& shared) {
    const std::size_t rounds = scaled(config, 2000);
    return fan_out(rt, 4 * config.threads, rounds,
                   [&](std::size_t i) {
                       return mkdir_op_task(shared, rounds, i);
                   }) *
           2;
}

std::uint64_t mkdir_call(const Config& config, Runtime& rt, Shared& shared) {
    const std::size_t rounds = scaled(config, 2000);
    return fan_out(rt, 4 * config.threads, rounds,
                   [&](std::size_t i) {
                       return mkdir_call_task(shared, rounds, i);
                   }) *
           2;
}

struct Case {
    const char* name;
    Run run;
};

constexpr Case CASES[] = {
    {"pread_4k", pread_4k},     {"pwrite_4k", pwrite_4k},
    {"open_close", open_close}, {"lone_pread", lone_pread},
    {"busy_fsync", busy_fsync},
#ifdef __linux__
    {"stat_op", stat_op},
#endif
    {"stat_call", stat_call},   {"mkdir_op", mkdir_op},
    {"mkdir_call", mkdir_call}};

}  // namespace

int main(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const dstd::StringView arg = argv[i];
        if (arg == "--smoke") {
            config.reps = 1;
            config.scale_percent = 1;
        } else if (arg == "--threads" && i + 1 < argc) {
            config.threads = static_cast<std::size_t>(std::atoi(argv[++i]));
        } else if (arg == "--reps" && i + 1 < argc) {
            config.reps = std::atoi(argv[++i]);
        } else if (arg == "--scale" && i + 1 < argc) {
            config.scale_percent =
                static_cast<std::size_t>(std::atoi(argv[++i]));
        } else if (arg == "--case" && i + 1 < argc) {
            config.only = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    char path[] = "/tmp/dftu_io_bench_XXXXXX";
    Shared shared;
    shared.fd = ::mkstemp(path);
    shared.path = path;
    if (shared.fd < 0) return 2;
    dstd::Vector<char> block(BLOCK, 'x');
    for (std::size_t i = 0; i < FILE_BLOCKS; ++i) {
        if (::write(shared.fd, block.data(), BLOCK) !=
            static_cast<ssize_t>(BLOCK)) {
            return 2;
        }
    }
    for (const Case& item : CASES) {
        if (config.only != nullptr &&
            std::strcmp(config.only, item.name) != 0) {
            continue;
        }
        dstd::Vector<double> times;
        for (int rep = 0; rep < config.reps; ++rep) {
            Runtime rt(ExecutorConfig{.thread_count = config.threads}, false);
            const auto start = Clock::now();
            const std::uint64_t ops = item.run(config, rt, shared);
            const std::chrono::duration<double, std::nano> took =
                Clock::now() - start;
            times.push_back(took.count() / static_cast<double>(ops));
        }
        std::sort(times.begin(), times.end());
        std::printf("%-12s threads=%zu ns_per_op=%.1f\n", item.name,
                    config.threads, times[times.size() / 2]);
        std::fflush(stdout);
    }
    ::close(shared.fd);
    ::unlink(path);
    return shared.failures.load() == 0 ? 0 : 1;
}
