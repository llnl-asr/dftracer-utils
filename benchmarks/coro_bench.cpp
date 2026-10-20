// Benchmark: the coroutine runtime primitives on a 4 thread executor.
//
// Every case runs one top-level coroutine on a ThreadPoolExecutor. The clock
// starts and stops inside that coroutine, so executor start-up and the final
// wait are not timed. The result of a rep is wall time divided by operations.
//
// Cases (one op is named per case):
//   semaphore_uncontended  one coroutine, acquire(1)+release(1) on a semaphore
//                          that never runs out. One op is one pair.
//   semaphore_contended    2x threads coroutines loop acquire(1)+release(1) on
//                          a semaphore of threads/2 permits, so most acquires
//                          queue. One op is one pair, over all coroutines.
//   mutex_uncontended      one coroutine, lock+unlock. One op is one pair.
//   mutex_contended        2x threads coroutines increment one counter under
//                          the lock. One op is one increment.
//   when_all_ready         when_all over K children that are already done. One
//                          op is one when_all (K children).
//   when_all_yield         when_all over K children that each yield once. One
//                          op is one when_all (K children).
//   when_any_ready         when_any over K children that are all ready. One op
//                          is one when_any (K children).
//   when_any_loser_yield   when_any over K children: child 0 is ready, the
//                          others complete after one yield. A loser is
//                          cancelled and awaited, so this also pays for the
//                          cancelled losers, which code that lets a loser run
//                          on does not. One op is one when_any (K children).
//   read_loop              pread of a 4 KiB block of a regular file, one after
//                          the other in one coroutine. One op is one read.
//
//   coro_bench                       (all cases, 5 reps, table)
//   coro_bench --reps R              (R reps per case)
//   coro_bench --case NAME           (one case)
//   coro_bench --format csv|table    (output shape)
//   coro_bench --smoke               (about 2% of the sizes, 1 rep, and every
//                                     result must be positive and correct)

#include <dftracer/utils/runtime/coro/async_mutex.h>
#include <dftracer/utils/runtime/coro/async_semaphore.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/coro/when_all.h>
#include <dftracer/utils/runtime/coro/when_any.h>
#include <dftracer/utils/runtime/coro/yield.h>
#include <dftracer/utils/runtime/io/io.h>
#include <dftracer/utils/runtime/pipeline/scheduler.h>
#include <dftracer/utils/runtime/scheduler/thread_pool_executor.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/runtime/tasks/task.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dftracer::utils;
namespace aio = dftracer::utils::io;

using Clock = std::chrono::steady_clock;

constexpr std::size_t THREADS = 4;
constexpr std::size_t KIB = 1024;
constexpr std::size_t READ_BLOCK = 4 * KIB;
constexpr std::size_t READ_FILE_BLOCKS = 16;
constexpr std::size_t FANOUT = 8;
constexpr int DEFAULT_REPS = 5;
constexpr double SMOKE_FRACTION = 0.02;

struct Result {
    double total_ns = 0;
    bool correct = true;
};

double elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - start).count();
}

// Runs `body` as the only top-level coroutine and returns what it stored.
template <class Body>
Result run_on_executor(Body body) {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = THREADS});
    Scheduler scheduler(&executor);
    Result result;
    auto task = make_task(
        [&](CoroScope&) -> coro::CoroTask<void> { co_await body(result); },
        "coro_bench");
    scheduler.schedule(task);
    task->wait();
    executor.shutdown();
    return result;
}

coro::CoroTask<void> semaphore_worker(coro::CoroSemaphore& sem,
                                      std::size_t iters) {
    for (std::size_t i = 0; i < iters; ++i) {
        co_await sem.acquire(1);
        sem.release(1);
    }
}

Result semaphore_uncontended(std::size_t n) {
    return run_on_executor([n](Result& r) -> coro::CoroTask<void> {
        coro::CoroSemaphore sem(1 << 20);
        auto start = Clock::now();
        co_await semaphore_worker(sem, n);
        r.total_ns = elapsed_ns(start, Clock::now());
    });
}

Result semaphore_contended(std::size_t n) {
    return run_on_executor([n](Result& r) -> coro::CoroTask<void> {
        constexpr std::size_t workers = 2 * THREADS;
        coro::CoroSemaphore sem(THREADS / 2);
        std::vector<coro::CoroTask<void>> tasks;
        for (std::size_t i = 0; i < workers; ++i) {
            tasks.push_back(semaphore_worker(sem, n / workers));
        }
        auto start = Clock::now();
        co_await coro::when_all(std::move(tasks));
        r.total_ns = elapsed_ns(start, Clock::now());
    });
}

coro::CoroTask<void> mutex_worker(coro::AsyncMutex& mutex,
                                  std::uint64_t& counter, std::size_t iters) {
    for (std::size_t i = 0; i < iters; ++i) {
        co_await mutex.lock();
        ++counter;
        mutex.unlock();
    }
}

Result mutex_uncontended(std::size_t n) {
    return run_on_executor([n](Result& r) -> coro::CoroTask<void> {
        coro::AsyncMutex mutex;
        std::uint64_t counter = 0;
        auto start = Clock::now();
        co_await mutex_worker(mutex, counter, n);
        r.total_ns = elapsed_ns(start, Clock::now());
        r.correct = counter == n;
    });
}

Result mutex_contended(std::size_t n) {
    return run_on_executor([n](Result& r) -> coro::CoroTask<void> {
        constexpr std::size_t workers = 2 * THREADS;
        coro::AsyncMutex mutex;
        std::uint64_t counter = 0;
        std::vector<coro::CoroTask<void>> tasks;
        for (std::size_t i = 0; i < workers; ++i) {
            tasks.push_back(mutex_worker(mutex, counter, n / workers));
        }
        auto start = Clock::now();
        co_await coro::when_all(std::move(tasks));
        r.total_ns = elapsed_ns(start, Clock::now());
        r.correct = counter == (n / workers) * workers;
    });
}

coro::CoroTask<int> ready_child(int value) { co_return value; }

coro::CoroTask<int> yield_child(int value) {
    co_await coro::yield();
    co_return value;
}

Result when_all_ready(std::size_t n) {
    return run_on_executor([n](Result& r) -> coro::CoroTask<void> {
        std::size_t sum = 0;
        auto start = Clock::now();
        for (std::size_t i = 0; i < n; ++i) {
            std::vector<coro::CoroTask<int>> children;
            children.reserve(FANOUT);
            for (std::size_t k = 0; k < FANOUT; ++k) {
                children.push_back(ready_child(1));
            }
            auto values = co_await coro::when_all(std::move(children));
            for (int v : values) sum += static_cast<std::size_t>(v);
        }
        r.total_ns = elapsed_ns(start, Clock::now());
        r.correct = sum == n * FANOUT;
    });
}

Result when_all_yield(std::size_t n) {
    return run_on_executor([n](Result& r) -> coro::CoroTask<void> {
        std::size_t sum = 0;
        auto start = Clock::now();
        for (std::size_t i = 0; i < n; ++i) {
            std::vector<coro::CoroTask<int>> children;
            children.reserve(FANOUT);
            for (std::size_t k = 0; k < FANOUT; ++k) {
                children.push_back(yield_child(1));
            }
            auto values = co_await coro::when_all(std::move(children));
            for (int v : values) sum += static_cast<std::size_t>(v);
        }
        r.total_ns = elapsed_ns(start, Clock::now());
        r.correct = sum == n * FANOUT;
    });
}

Result when_any_ready(std::size_t n) {
    return run_on_executor([n](Result& r) -> coro::CoroTask<void> {
        std::size_t completed = 0;
        auto start = Clock::now();
        for (std::size_t i = 0; i < n; ++i) {
            std::vector<coro::CoroTask<int>> children;
            children.reserve(FANOUT);
            for (std::size_t k = 0; k < FANOUT; ++k) {
                children.push_back(ready_child(static_cast<int>(k)));
            }
            auto winner = co_await coro::when_any(std::move(children));
            if (winner.index < FANOUT) ++completed;
        }
        r.total_ns = elapsed_ns(start, Clock::now());
        r.correct = completed == n;
    });
}

Result when_any_loser_yield(std::size_t n) {
    return run_on_executor([n](Result& r) -> coro::CoroTask<void> {
        std::size_t completed = 0;
        auto start = Clock::now();
        for (std::size_t i = 0; i < n; ++i) {
            std::vector<coro::CoroTask<int>> children;
            children.reserve(FANOUT);
            children.push_back(ready_child(0));
            for (std::size_t k = 1; k < FANOUT; ++k) {
                children.push_back(yield_child(static_cast<int>(k)));
            }
            auto winner = co_await coro::when_any(std::move(children));
            if (winner.index < FANOUT) ++completed;
        }
        r.total_ns = elapsed_ns(start, Clock::now());
        r.correct = completed == n;
    });
}

Result read_loop(std::size_t n) {
    char path[] = "/tmp/dftracer_coro_bench_XXXXXX";
    int fd = ::mkstemp(path);
    if (fd < 0) std::abort();
    ::unlink(path);
    std::string block(READ_BLOCK * READ_FILE_BLOCKS, 'x');
    if (::write(fd, block.data(), block.size()) !=
        static_cast<ssize_t>(block.size())) {
        std::abort();
    }
    Result result = run_on_executor([n, fd](Result& r) -> coro::CoroTask<void> {
        std::vector<char> buf(READ_BLOCK);
        std::size_t good = 0;
        auto start = Clock::now();
        for (std::size_t i = 0; i < n; ++i) {
            auto offset =
                static_cast<off_t>((i % READ_FILE_BLOCKS) * READ_BLOCK);
            ssize_t got =
                co_await aio::pread(fd, buf.data(), READ_BLOCK, offset);
            if (got == static_cast<ssize_t>(READ_BLOCK)) ++good;
        }
        r.total_ns = elapsed_ns(start, Clock::now());
        r.correct = good == n;
    });
    ::close(fd);
    return result;
}

struct Case {
    const char* name;
    std::size_t full_ops;
    std::function<Result(std::size_t)> run;
    // Ops a run does for a requested size; the contended cases round down.
    std::function<std::size_t(std::size_t)> ops;
};

std::size_t exact(std::size_t n) { return n; }
std::size_t per_worker(std::size_t n) {
    constexpr std::size_t workers = 2 * THREADS;
    return (n / workers) * workers;
}

std::vector<Case> make_cases() {
    return {
        {"semaphore_uncontended", 25000000, semaphore_uncontended, exact},
        {"semaphore_contended", 25000000, semaphore_contended, per_worker},
        {"mutex_uncontended", 50000000, mutex_uncontended, exact},
        {"mutex_contended", 50000000, mutex_contended, per_worker},
        {"when_all_ready", 1000000, when_all_ready, exact},
        {"when_all_yield", 200000, when_all_yield, exact},
        {"when_any_ready", 1000000, when_any_ready, exact},
        {"when_any_loser_yield", 4000, when_any_loser_yield, exact},
        {"read_loop", 40000, read_loop, exact},
    };
}

struct Stats {
    double median;
    double min;
    double max;
    double spread_pct;
};

Stats summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    Stats s{};
    s.median = values[values.size() / 2];
    s.min = values.front();
    s.max = values.back();
    s.spread_pct = s.median > 0 ? (s.max - s.min) / s.median * 100.0 : 0.0;
    return s;
}

int run(const std::vector<Case>& cases, int reps, bool smoke, bool csv) {
    if (csv) {
        std::printf("name,median_ns_per_op,min,max,spread_pct\n");
    } else {
        std::printf("coro_bench threads=%zu reps=%d%s\n", THREADS, reps,
                    smoke ? " smoke" : "");
        std::printf(
            "| case | median ns/op | min | max | spread %% |\n"
            "|---|---|---|---|---|\n");
    }
    int failures = 0;
    for (const Case& c : cases) {
        std::size_t requested =
            smoke ? std::max<std::size_t>(
                        static_cast<std::size_t>(
                            static_cast<double>(c.full_ops) * SMOKE_FRACTION),
                        64)
                  : c.full_ops;
        std::size_t ops = c.ops(requested);
        std::vector<double> per_op;
        bool ok = true;
        for (int rep = 0; rep < reps; ++rep) {
            Result r = c.run(requested);
            ok = ok && r.correct && r.total_ns > 0.0;
            per_op.push_back(r.total_ns / static_cast<double>(ops));
        }
        Stats s = summarize(per_op);
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "case failed: %s\n", c.name);
        }
        if (csv) {
            std::printf("%s,%.1f,%.1f,%.1f,%.1f\n", c.name, s.median, s.min,
                        s.max, s.spread_pct);
        } else {
            std::printf("| %s | %.1f | %.1f | %.1f | %.1f |\n", c.name,
                        s.median, s.min, s.max, s.spread_pct);
        }
        std::fflush(stdout);
    }
    return failures == 0 ? 0 : 1;
}

int usage() {
    std::fprintf(stderr,
                 "usage: coro_bench [--smoke] [--reps R] [--case NAME] "
                 "[--format csv|table]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    int reps = DEFAULT_REPS;
    bool smoke = false;
    bool csv = false;
    std::string only;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        bool has_value = i + 1 < argc;
        if (arg == "--smoke") {
            smoke = true;
        } else if (arg == "--reps" && has_value) {
            reps = std::atoi(argv[++i]);
        } else if (arg == "--case" && has_value) {
            only = argv[++i];
        } else if (arg == "--format" && has_value) {
            std::string_view fmt = argv[++i];
            if (fmt != "csv" && fmt != "table") return usage();
            csv = fmt == "csv";
        } else {
            return usage();
        }
    }
    if (smoke) reps = 1;
    if (reps <= 0) return usage();

    std::vector<Case> cases = make_cases();
    if (!only.empty()) {
        std::vector<Case> picked;
        for (const Case& c : cases) {
            if (only == c.name) picked.push_back(c);
        }
        if (picked.empty()) {
            std::fprintf(stderr, "unknown case: %s\n", only.c_str());
            return 2;
        }
        cases = std::move(picked);
    }
    return run(cases, reps, smoke, csv);
}
