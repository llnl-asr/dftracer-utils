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
//   coro_bench --record FILE         (write the per-case counts as JSON)
//   coro_bench --check FILE          (fail when a case retires more than 2%
//                                     more instructions per op, makes any
//                                     more allocations per op or holds more
//                                     than 5% more peak heap than the
//                                     record; wall time is only printed)
//   coro_bench --smoke               (about 2% of the sizes, 1 rep, and every
//                                     result must be positive and correct)

#define DFTRACER_UTILS_BENCH_COUNT_NEW
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

#include "bench_counters.h"

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

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
    std::uint64_t instructions = 0;
    std::uint64_t allocations = 0;
    std::uint64_t peak_heap = 0;
    bool correct = true;
};

double elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - start).count();
}

// The counts cover the timed part of a case, all threads of the process.
struct Meter {
    Clock::time_point time;
    std::uint64_t instructions;
    std::uint64_t allocations;
};

Meter begin_meter() {
    return {Clock::now(), dftu::bench::instructions_retired(),
            dftu::bench::allocations()};
}

void finish_meter(const Meter &start, Result &r) {
    std::uint64_t instructions = dftu::bench::instructions_retired();
    std::uint64_t allocations = dftu::bench::allocations();
    r.total_ns = elapsed_ns(start.time, Clock::now());
    r.instructions = instructions - start.instructions;
    r.allocations = allocations - start.allocations;
    r.peak_heap = dftu::bench::peak_heap_bytes();
}

// Runs `body` as the only top-level coroutine and returns what it stored.
template <class Body>
Result run_on_executor(Body body) {
    ThreadPoolExecutor executor(ExecutorConfig{.num_threads = THREADS});
    Scheduler scheduler(&executor);
    Result result;
    auto task = make_task(
        [&](CoroScope &) -> coro::CoroTask<void> { co_await body(result); },
        "coro_bench");
    scheduler.schedule(task);
    task->wait();
    executor.shutdown();
    return result;
}

coro::CoroTask<void> semaphore_worker(coro::CoroSemaphore &sem,
                                      std::size_t iters) {
    for (std::size_t i = 0; i < iters; ++i) {
        co_await sem.acquire(1);
        sem.release(1);
    }
}

Result semaphore_uncontended(std::size_t n) {
    return run_on_executor([n](Result &r) -> coro::CoroTask<void> {
        coro::CoroSemaphore sem(1 << 20);
        Meter start = begin_meter();
        co_await semaphore_worker(sem, n);
        finish_meter(start, r);
    });
}

Result semaphore_contended(std::size_t n) {
    return run_on_executor([n](Result &r) -> coro::CoroTask<void> {
        constexpr std::size_t workers = 2 * THREADS;
        coro::CoroSemaphore sem(THREADS / 2);
        std::vector<coro::CoroTask<void>> tasks;
        for (std::size_t i = 0; i < workers; ++i) {
            tasks.push_back(semaphore_worker(sem, n / workers));
        }
        Meter start = begin_meter();
        co_await coro::when_all(std::move(tasks));
        finish_meter(start, r);
    });
}

coro::CoroTask<void> mutex_worker(coro::AsyncMutex &mutex,
                                  std::uint64_t &counter, std::size_t iters) {
    for (std::size_t i = 0; i < iters; ++i) {
        co_await mutex.lock();
        ++counter;
        mutex.unlock();
    }
}

Result mutex_uncontended(std::size_t n) {
    return run_on_executor([n](Result &r) -> coro::CoroTask<void> {
        coro::AsyncMutex mutex;
        std::uint64_t counter = 0;
        Meter start = begin_meter();
        co_await mutex_worker(mutex, counter, n);
        finish_meter(start, r);
        r.correct = counter == n;
    });
}

Result mutex_contended(std::size_t n) {
    return run_on_executor([n](Result &r) -> coro::CoroTask<void> {
        constexpr std::size_t workers = 2 * THREADS;
        coro::AsyncMutex mutex;
        std::uint64_t counter = 0;
        std::vector<coro::CoroTask<void>> tasks;
        for (std::size_t i = 0; i < workers; ++i) {
            tasks.push_back(mutex_worker(mutex, counter, n / workers));
        }
        Meter start = begin_meter();
        co_await coro::when_all(std::move(tasks));
        finish_meter(start, r);
        r.correct = counter == (n / workers) * workers;
    });
}

coro::CoroTask<int> ready_child(int value) { co_return value; }

coro::CoroTask<int> yield_child(int value) {
    co_await coro::yield();
    co_return value;
}

Result when_all_ready(std::size_t n) {
    return run_on_executor([n](Result &r) -> coro::CoroTask<void> {
        std::size_t sum = 0;
        Meter start = begin_meter();
        for (std::size_t i = 0; i < n; ++i) {
            std::vector<coro::CoroTask<int>> children;
            children.reserve(FANOUT);
            for (std::size_t k = 0; k < FANOUT; ++k) {
                children.push_back(ready_child(1));
            }
            auto values = co_await coro::when_all(std::move(children));
            for (int v : values) sum += static_cast<std::size_t>(v);
        }
        finish_meter(start, r);
        r.correct = sum == n * FANOUT;
    });
}

Result when_all_yield(std::size_t n) {
    return run_on_executor([n](Result &r) -> coro::CoroTask<void> {
        std::size_t sum = 0;
        Meter start = begin_meter();
        for (std::size_t i = 0; i < n; ++i) {
            std::vector<coro::CoroTask<int>> children;
            children.reserve(FANOUT);
            for (std::size_t k = 0; k < FANOUT; ++k) {
                children.push_back(yield_child(1));
            }
            auto values = co_await coro::when_all(std::move(children));
            for (int v : values) sum += static_cast<std::size_t>(v);
        }
        finish_meter(start, r);
        r.correct = sum == n * FANOUT;
    });
}

Result when_any_ready(std::size_t n) {
    return run_on_executor([n](Result &r) -> coro::CoroTask<void> {
        std::size_t completed = 0;
        Meter start = begin_meter();
        for (std::size_t i = 0; i < n; ++i) {
            std::vector<coro::CoroTask<int>> children;
            children.reserve(FANOUT);
            for (std::size_t k = 0; k < FANOUT; ++k) {
                children.push_back(ready_child(static_cast<int>(k)));
            }
            auto winner = co_await coro::when_any(std::move(children));
            if (winner.index < FANOUT) ++completed;
        }
        finish_meter(start, r);
        r.correct = completed == n;
    });
}

Result when_any_loser_yield(std::size_t n) {
    return run_on_executor([n](Result &r) -> coro::CoroTask<void> {
        std::size_t completed = 0;
        Meter start = begin_meter();
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
        finish_meter(start, r);
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
    Result result = run_on_executor([n, fd](Result &r) -> coro::CoroTask<void> {
        std::vector<char> buf(READ_BLOCK);
        std::size_t good = 0;
        Meter start = begin_meter();
        for (std::size_t i = 0; i < n; ++i) {
            auto offset =
                static_cast<off_t>((i % READ_FILE_BLOCKS) * READ_BLOCK);
            ssize_t got =
                co_await aio::pread(fd, buf.data(), READ_BLOCK, offset);
            if (got == static_cast<ssize_t>(READ_BLOCK)) ++good;
        }
        finish_meter(start, r);
        r.correct = good == n;
    });
    ::close(fd);
    return result;
}

struct Case {
    const char *name;
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

// A case passes --check when its instructions per op grow by at most
// max(CHECK_INSTR_PCT, the spread of the record), its allocations per op do
// not grow by more than
// CHECK_ALLOC_SLACK (the few blocks that a contended run takes depend on
// timing, a block per op or per rep is far above it), and its peak resident
// set grows by at most CHECK_PEAK_PCT. Wall time is printed and never checked.
constexpr double CHECK_INSTR_PCT = 2.0;
constexpr double CHECK_ALLOC_SLACK = 0.001;
constexpr double CHECK_PEAK_PCT = 5.0;
// read_loop takes an I/O request from the page heap on every read, about 1,000
// instructions per op more than the object pool it replaced; this allowance
// stays until the heap's small-allocation path is cheaper.
constexpr double READ_LOOP_INSTR_PCT = 3.0;

struct Recorded {
    std::string name;
    double ns = 0.0;
    double instr = 0.0;
    double allocs = 0.0;
    double peak = 0.0;
    double instr_spread_pct = 0.0;
};

bool write_record(const char *path, const std::vector<Recorded> &rows) {
    std::FILE *f = std::fopen(path, "w");
    if (f == nullptr) return false;
    std::fprintf(f, "{\n");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Recorded &r = rows[i];
        std::fprintf(f,
                     "  \"%s\": {\"instr_per_op\": %.3f, \"allocs_per_op\": "
                     "%.4f, \"peak_heap_bytes\": %.0f, \"instr_spread_pct\": "
                     "%.3f, \"ns_per_op\": %.3f}%s\n",
                     r.name.c_str(), r.instr, r.allocs, r.peak,
                     r.instr_spread_pct, r.ns, i + 1 < rows.size() ? "," : "");
    }
    std::fprintf(f, "}\n");
    return std::fclose(f) == 0;
}

bool read_record(const char *path, std::vector<Recorded> &rows) {
    std::FILE *f = std::fopen(path, "r");
    if (f == nullptr) return false;
    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char name[128];
        Recorded r;
        if (std::sscanf(line,
                        " \"%127[^\"]\": {\"instr_per_op\": %lf, "
                        "\"allocs_per_op\": %lf, \"peak_heap_bytes\": %lf, "
                        "\"instr_spread_pct\": %lf, \"ns_per_op\": %lf",
                        name, &r.instr, &r.allocs, &r.peak, &r.instr_spread_pct,
                        &r.ns) == 6) {
            r.name = name;
            rows.push_back(r);
        }
    }
    std::fclose(f);
    return !rows.empty();
}

double pct_change(double now, double base) {
    return base > 0.0 ? (now / base - 1.0) * 100.0 : 0.0;
}

int check_record(const char *path, const std::vector<Recorded> &now) {
    std::vector<Recorded> base;
    if (!read_record(path, base)) {
        std::fprintf(stderr, "cannot read record: %s\n", path);
        return 1;
    }
    int worse = 0;
    for (const Recorded &n : now) {
        for (const Recorded &b : base) {
            if (b.name != n.name) continue;
            bool have_instr = n.instr > 0.0 && b.instr > 0.0;
            double instr_change = pct_change(n.instr, b.instr);
            double peak_change = pct_change(n.peak, b.peak);
            double allowed = std::max(CHECK_INSTR_PCT, b.instr_spread_pct);
            if (n.name == "read_loop")
                allowed = std::max(allowed, READ_LOOP_INSTR_PCT);
            bool bad_instr = have_instr && instr_change > allowed;
            bool bad_allocs = n.allocs > b.allocs + CHECK_ALLOC_SLACK;
            bool bad_peak = peak_change > CHECK_PEAK_PCT;
            std::fprintf(
                stderr,
                "check %-24s instr %10.1f vs %10.1f (%+.2f%%, allowed "
                "%.1f%%)%s allocs "
                "%7.3f vs %7.3f%s peak %.0f vs %.0f (%+.1f%%)%s\n",
                n.name.c_str(), n.instr, b.instr, instr_change, allowed,
                have_instr ? (bad_instr ? " WORSE" : " ok") : " unavailable",
                n.allocs, b.allocs, bad_allocs ? " WORSE" : " ok", n.peak,
                b.peak, peak_change, bad_peak ? " WORSE" : " ok");
            if (bad_instr || bad_allocs || bad_peak) ++worse;
        }
    }
    return worse == 0 ? 0 : 1;
}

int run(const std::vector<Case> &cases, int reps, bool smoke, bool csv,
        const char *record, const char *check) {
    std::vector<Recorded> rows;
    if (csv) {
        std::printf(
            "name,median_ns_per_op,min,max,spread_pct,instr_per_op,"
            "instr_spread_pct,allocs_per_op,peak_heap_bytes\n");
    } else {
        std::printf(
            "coro_bench threads=%zu reps=%d%s instructions=%s\n", THREADS, reps,
            smoke ? " smoke" : "",
            dftu::bench::instructions_available() ? "yes" : "unavailable");
        std::printf(
            "| case | median ns/op | min | max | spread %% | instr/op | instr "
            "spread %% | allocs/op | peak heap bytes |\n"
            "|---|---|---|---|---|---|---|---|---|\n");
    }
    int failures = 0;
    for (const Case &c : cases) {
        std::size_t requested =
            smoke ? std::max<std::size_t>(
                        static_cast<std::size_t>(
                            static_cast<double>(c.full_ops) * SMOKE_FRACTION),
                        64)
                  : c.full_ops;
        std::size_t ops = c.ops(requested);
        std::vector<double> per_op;
        std::vector<double> instr_per_op;
        std::vector<double> allocs_per_op;
        double peak = 0.0;
        bool ok = true;
        for (int rep = 0; rep < reps; ++rep) {
            Result r = c.run(requested);
            ok = ok && r.correct && r.total_ns > 0.0;
            auto per = [&](std::uint64_t v) {
                return static_cast<double>(v) / static_cast<double>(ops);
            };
            per_op.push_back(r.total_ns / static_cast<double>(ops));
            instr_per_op.push_back(per(r.instructions));
            allocs_per_op.push_back(per(r.allocations));
            peak = std::max(peak, static_cast<double>(r.peak_heap));
        }
        Stats s = summarize(per_op);
        Stats si = summarize(instr_per_op);
        Stats sa = summarize(allocs_per_op);
        rows.push_back(
            {c.name, s.median, si.median, sa.median, peak, si.spread_pct});
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "case failed: %s\n", c.name);
        }
        if (csv) {
            std::printf("%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,%.4f,%.0f\n", c.name,
                        s.median, s.min, s.max, s.spread_pct, si.median,
                        si.spread_pct, sa.median, peak);
        } else {
            std::printf(
                "| %s | %.1f | %.1f | %.1f | %.1f | %.1f | %.2f | %.4f "
                "| %.0f |\n",
                c.name, s.median, s.min, s.max, s.spread_pct, si.median,
                si.spread_pct, sa.median, peak);
        }
        std::fflush(stdout);
    }
    int status = failures == 0 ? 0 : 1;
    if (record != nullptr && !write_record(record, rows)) {
        std::fprintf(stderr, "cannot write record: %s\n", record);
        status = 1;
    }
    if (check != nullptr && check_record(check, rows) != 0) status = 1;
    return status;
}

int usage() {
    std::fprintf(stderr,
                 "usage: coro_bench [--smoke] [--reps R] [--case NAME] "
                 "[--format csv|table] [--record FILE] [--check FILE]\n");
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
#ifdef __APPLE__
    // Executor threads inherit this class, so they stay on performance cores
    // and the timings do not depend on where the scheduler put a thread.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    dftu::bench::init_counters();
    int reps = DEFAULT_REPS;
    bool smoke = false;
    bool csv = false;
    std::string only;
    const char *record = nullptr;
    const char *check = nullptr;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        bool has_value = i + 1 < argc;
        if (arg == "--smoke") {
            smoke = true;
        } else if (arg == "--reps" && has_value) {
            reps = std::atoi(argv[++i]);
        } else if (arg == "--case" && has_value) {
            only = argv[++i];
        } else if (arg == "--record" && has_value) {
            record = argv[++i];
        } else if (arg == "--check" && has_value) {
            check = argv[++i];
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
        for (const Case &c : cases) {
            if (only == c.name) picked.push_back(c);
        }
        if (picked.empty()) {
            std::fprintf(stderr, "unknown case: %s\n", only.c_str());
            return 2;
        }
        cases = std::move(picked);
    }
    return run(cases, reps, smoke, csv, record, check);
}
