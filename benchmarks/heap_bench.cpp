// Benchmark: the page heap against the C library malloc.
//
// Both sides run the same code through two small policies, so nothing differs
// but the allocator: page_allocate/page_deallocate (the page heap itself, not
// the "system" backend of the memory library) against malloc/free. Every block
// gets one byte written and the pointers are kept in memory until they are
// freed, so neither side can skip the work.
//
// Cases (ns per op is wall time divided by operations, see each case):
//   small <size> batch/lifo   alloc+free of 16, 64, 256, 1024 and 8192 bytes;
//                             a batch frees in allocation order, lifo in
//                             reverse. One op is one alloc plus its free.
//   medium free               frees alone of 128 KiB blocks. One op is one
//                             free. Each medium free reads the clock because
//                             give_run stamps the pages it returns.
//   xthread NpNc              N producers allocate small blocks and N
//                             consumers free them through single-producer
//                             rings, so every free is remote. One op is one
//                             alloc plus its free, per producer.
//   mixed                     a fixed-seed trace of small, medium (64 KiB to
//                             1 MiB) and large (1 to 8 MiB) blocks with random
//                             lifetimes. One op is one alloc plus one free.
//   mixed budget              mixed with a 1 TiB budget: every new mapping
//                             compares the backed bytes with the budget and
//                             never purges.
//   mixed tight               mixed with a 64 MiB budget, below its peak: a
//                             new mapping purges cached runs first.
//
// The default run is a driver. For every case it starts this binary again for
// each allocator in rounds of system, heap, system, heap, system, heap, so
// each case has its own process and its own peak resident set. The table has
// the median ns per op and the median peak RSS over the rounds.
//
//   heap_bench                   (driver, 3 rounds)
//   heap_bench --rounds N        (driver, N rounds)
//   heap_bench --smoke           (in this process, tiny sizes, no timing
//                                 checks: every case ran and the page heap
//                                 live bytes ended where they started)
//   heap_bench --case NAME --alloc system|heap
//                                (one case in this process; prints RESULT)

#include <dftracer/utils/memory/heap_page.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mem = dftracer::utils::memory;

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t KIB = 1024;
constexpr std::size_t MIB = 1024 * KIB;
constexpr std::size_t CACHE_LINE = 128;
constexpr std::size_t RING_SLOTS = 4096;
constexpr std::size_t SEED = 42;
constexpr int DEFAULT_ROUNDS = 3;

constexpr std::array<std::size_t, 5> SMALL_SIZES{16, 64, 256, 1024, 8192};
constexpr std::array<std::size_t, 6> CROSS_SIZES{32, 64, 128, 256, 512, 1024};
constexpr std::size_t MEDIUM_FREE_SIZE = 128 * KIB;

struct Scale {
    std::size_t batch;
    std::size_t reps;
    std::size_t cross_blocks;
    std::size_t mixed_slots;
    std::size_t mixed_steps;
    std::size_t medium_count;
};

constexpr Scale SMOKE_SCALE{64, 4, 2000, 64, 2000, 32};
constexpr Scale FULL_SCALE{4096, 500, 400000, 1024, 400000, 4000};

struct HeapPolicy {
    static void* allocate(std::size_t n) { return mem::page_allocate(n, 16); }
    static void release(void* p, std::size_t n) { mem::page_deallocate(p, n); }
};

struct MallocPolicy {
    static void* allocate(std::size_t n) {
        void* p = std::malloc(n);
        if (p == nullptr) std::abort();
        return p;
    }
    static void release(void* p, std::size_t) { std::free(p); }
};

void touch(void* p) { *static_cast<volatile unsigned char*>(p) = 1; }

double elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - start).count();
}

template <class A>
double small_case(std::size_t size, bool lifo, const Scale& scale) {
    std::vector<void*> blocks(scale.batch);
    auto start = Clock::now();
    for (std::size_t rep = 0; rep < scale.reps; ++rep) {
        for (std::size_t i = 0; i < scale.batch; ++i) {
            blocks[i] = A::allocate(size);
            touch(blocks[i]);
        }
        if (lifo) {
            for (std::size_t i = scale.batch; i-- > 0;) {
                A::release(blocks[i], size);
            }
        } else {
            for (std::size_t i = 0; i < scale.batch; ++i) {
                A::release(blocks[i], size);
            }
        }
    }
    auto end = Clock::now();
    return elapsed_ns(start, end) /
           static_cast<double>(scale.batch * scale.reps);
}

template <class A>
double medium_free_case(const Scale& scale) {
    double total_ns = 0;
    std::size_t frees = 0;
    std::vector<void*> blocks(scale.medium_count);
    for (int rep = 0; rep < 8; ++rep) {
        for (auto& b : blocks) {
            b = A::allocate(MEDIUM_FREE_SIZE);
            touch(b);
        }
        auto start = Clock::now();
        for (void* b : blocks) A::release(b, MEDIUM_FREE_SIZE);
        total_ns += elapsed_ns(start, Clock::now());
        frees += blocks.size();
    }
    return total_ns / static_cast<double>(frees);
}

struct Slot {
    void* p;
    std::size_t size;
};

struct alignas(CACHE_LINE) Ring {
    std::atomic<std::size_t> head{0};
    alignas(CACHE_LINE) std::atomic<std::size_t> tail{0};
    alignas(CACHE_LINE) std::array<Slot, RING_SLOTS> slots{};
};

template <class A>
void produce(Ring& ring, std::size_t blocks) {
    for (std::size_t i = 0; i < blocks; ++i) {
        std::size_t size = CROSS_SIZES[i % CROSS_SIZES.size()];
        void* p = A::allocate(size);
        touch(p);
        std::size_t head = ring.head.load(std::memory_order_relaxed);
        while (head - ring.tail.load(std::memory_order_acquire) == RING_SLOTS) {
            std::this_thread::yield();
        }
        ring.slots[head % RING_SLOTS] = {p, size};
        ring.head.store(head + 1, std::memory_order_release);
    }
}

template <class A>
void consume(Ring& ring, std::size_t blocks) {
    for (std::size_t done = 0; done < blocks;) {
        std::size_t tail = ring.tail.load(std::memory_order_relaxed);
        if (tail == ring.head.load(std::memory_order_acquire)) {
            std::this_thread::yield();
            continue;
        }
        Slot slot = ring.slots[tail % RING_SLOTS];
        ring.tail.store(tail + 1, std::memory_order_release);
        A::release(slot.p, slot.size);
        ++done;
    }
}

template <class A>
double cross_case(std::size_t pairs, const Scale& scale) {
    std::vector<Ring> rings(pairs);
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    auto body = [&](bool producer, std::size_t pair) {
        ready.fetch_add(1);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        if (producer) {
            produce<A>(rings[pair], scale.cross_blocks);
        } else {
            consume<A>(rings[pair], scale.cross_blocks);
        }
    };
    for (std::size_t i = 0; i < pairs; ++i) {
        threads.emplace_back(body, true, i);
        threads.emplace_back(body, false, i);
    }
    while (ready.load() < 2 * pairs) std::this_thread::yield();
    auto start = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();
    auto end = Clock::now();
    return elapsed_ns(start, end) / static_cast<double>(scale.cross_blocks);
}

struct Step {
    std::uint32_t slot;
    std::uint32_t size;
};

std::vector<Step> mixed_trace(const Scale& scale) {
    std::mt19937_64 rng(SEED);
    std::vector<Step> trace;
    trace.reserve(scale.mixed_steps);
    for (std::size_t i = 0; i < scale.mixed_steps; ++i) {
        auto slot = static_cast<std::uint32_t>(rng() % scale.mixed_slots);
        std::size_t roll = rng() % 1000;
        std::size_t size;
        if (roll < 850) {
            std::size_t octave = 4 + rng() % 10;
            size =
                (std::size_t{1} << octave) + rng() % (std::size_t{1} << octave);
        } else if (roll < 998) {
            size = 64 * KIB + rng() % (MIB - 64 * KIB + 1);
        } else {
            size = MIB + rng() % (7 * MIB + 1);
        }
        trace.push_back({slot, static_cast<std::uint32_t>(size)});
    }
    return trace;
}

template <class A>
double mixed_case(const Scale& scale) {
    std::vector<Step> trace = mixed_trace(scale);
    std::vector<void*> live(scale.mixed_slots, nullptr);
    std::vector<std::uint32_t> sizes(scale.mixed_slots, 0);
    auto start = Clock::now();
    for (const Step& step : trace) {
        if (live[step.slot] != nullptr) {
            A::release(live[step.slot], sizes[step.slot]);
        }
        live[step.slot] = A::allocate(step.size);
        sizes[step.slot] = step.size;
        touch(live[step.slot]);
    }
    auto end = Clock::now();
    for (std::size_t i = 0; i < live.size(); ++i) {
        if (live[i] != nullptr) A::release(live[i], sizes[i]);
    }
    return elapsed_ns(start, end) / static_cast<double>(trace.size());
}

template <class A>
double mixed_budget_case(const Scale& scale, std::size_t budget) {
    mem::Config config;
    config.budget = budget;
    mem::configure(config);
    double ns = mixed_case<A>(scale);
    mem::configure(mem::Config{});
    return ns;
}

struct Case {
    std::string name;
    std::function<double(bool heap, const Scale&)> run;
};

template <class Fn>
std::function<double(bool, const Scale&)> both(Fn fn) {
    return [fn](bool heap, const Scale& scale) {
        return heap ? fn(HeapPolicy{}, scale) : fn(MallocPolicy{}, scale);
    };
}

std::vector<Case> make_cases() {
    std::vector<Case> cases;
    for (std::size_t size : SMALL_SIZES) {
        for (bool lifo : {false, true}) {
            std::string name = "small " + std::to_string(size) + "B " +
                               (lifo ? "lifo" : "batch");
            cases.push_back(
                {name, both([size, lifo](auto policy, const Scale& scale) {
                     using A = decltype(policy);
                     return small_case<A>(size, lifo, scale);
                 })});
        }
    }
    cases.push_back({"medium free", both([](auto policy, const Scale& scale) {
                         return medium_free_case<decltype(policy)>(scale);
                     })});
    std::size_t cores =
        std::max<std::size_t>(std::thread::hardware_concurrency() / 2, 1);
    std::vector<std::size_t> pair_counts{1, 8};
    if (cores != 1 && cores != 8) pair_counts.push_back(cores);
    for (std::size_t pairs : pair_counts) {
        std::string name = "xthread " + std::to_string(pairs) + "p" +
                           std::to_string(pairs) + "c";
        if (pairs == cores) name += " (all cores)";
        cases.push_back({name, both([pairs](auto policy, const Scale& scale) {
                             return cross_case<decltype(policy)>(pairs, scale);
                         })});
    }
    cases.push_back({"mixed", both([](auto policy, const Scale& scale) {
                         return mixed_case<decltype(policy)>(scale);
                     })});
    for (auto [name, budget] : {std::pair<const char*, std::size_t>{
                                    "mixed budget", 1024 * 1024 * MIB},
                                {"mixed tight", 64 * MIB}}) {
        cases.push_back({name, both([budget](auto policy, const Scale& scale) {
                             return mixed_budget_case<decltype(policy)>(scale,
                                                                        budget);
                         })});
    }
    return cases;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

struct ChildResult {
    double ns;
    double rss_mib;
};

std::optional<ChildResult> run_child(const char* self, const std::string& name,
                                     bool heap) {
    int fds[2];
    if (::pipe(fds) != 0) return std::nullopt;
    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[1]);
        ::execl(self, self, "--case", name.c_str(), "--alloc",
                heap ? "heap" : "system", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(fds[1]);
    std::string out;
    char buffer[256];
    ssize_t n;
    while ((n = ::read(fds[0], buffer, sizeof(buffer))) > 0) {
        out.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fds[0]);
    int status = 0;
    struct rusage usage{};
    ::wait4(pid, &status, 0, &usage);
    double ns = 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        std::sscanf(out.c_str(), "RESULT %lf", &ns) != 1) {
        return std::nullopt;
    }
#if defined(__APPLE__)
    double rss_bytes = static_cast<double>(usage.ru_maxrss);
#else
    double rss_bytes = static_cast<double>(usage.ru_maxrss) * 1024.0;
#endif
    return ChildResult{ns, rss_bytes / static_cast<double>(MIB)};
}

int drive(const char* self, int rounds) {
    std::vector<Case> cases = make_cases();
    std::vector<std::vector<double>> ns[2];
    std::vector<std::vector<double>> rss[2];
    for (auto& side : ns) side.resize(cases.size());
    for (auto& side : rss) side.resize(cases.size());
    std::printf("heap_bench rounds=%d order=system,heap per case, hw=%u\n",
                rounds, std::thread::hardware_concurrency());
    for (int round = 0; round < rounds; ++round) {
        for (std::size_t c = 0; c < cases.size(); ++c) {
            for (int side = 0; side < 2; ++side) {
                auto result = run_child(self, cases[c].name, side == 1);
                if (!result) {
                    std::fprintf(stderr, "case failed: %s (%s)\n",
                                 cases[c].name.c_str(),
                                 side == 1 ? "heap" : "system");
                    return 1;
                }
                ns[side][c].push_back(result->ns);
                rss[side][c].push_back(result->rss_mib);
                std::printf(
                    "raw round=%d case=\"%s\" alloc=%s ns=%.3f rss_mib=%.1f\n",
                    round, cases[c].name.c_str(), side == 1 ? "heap" : "system",
                    result->ns, result->rss_mib);
                std::fflush(stdout);
            }
        }
    }
    std::printf(
        "\n| case | system median ns | heap median ns | ratio | peak RSS "
        "system MiB | peak RSS heap MiB |\n|---|---|---|---|---|---|\n");
    for (std::size_t c = 0; c < cases.size(); ++c) {
        double sys = median(ns[0][c]);
        double heap = median(ns[1][c]);
        std::printf("| %s | %.1f | %.1f | %.2f | %.1f | %.1f |\n",
                    cases[c].name.c_str(), sys, heap, heap / sys,
                    median(rss[0][c]), median(rss[1][c]));
    }
    return 0;
}

int smoke() {
    std::vector<Case> cases = make_cases();
    int failures = 0;
    for (const Case& c : cases) {
        for (bool heap : {false, true}) {
            std::size_t live_before = mem::page_stats().live;
            double ns = c.run(heap, SMOKE_SCALE);
            std::size_t live_after = mem::page_stats().live;
            bool ran = ns > 0.0;
            bool freed = !heap || live_after == live_before;
            std::printf("smoke %-28s %-6s %s\n", c.name.c_str(),
                        heap ? "heap" : "system",
                        ran && freed ? "ok" : "FAILED");
            if (!ran || !freed) ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}

int run_one(const std::string& name, const std::string& alloc) {
    bool heap = alloc == "heap";
    for (const Case& c : make_cases()) {
        if (c.name != name) continue;
        std::size_t live_before = mem::page_stats().live;
        double ns = c.run(heap, FULL_SCALE);
        if (heap && mem::page_stats().live != live_before) {
            std::fprintf(stderr, "live bytes did not return to start\n");
            return 1;
        }
        std::printf("RESULT %.3f\n", ns);
        return 0;
    }
    std::fprintf(stderr, "unknown case: %s\n", name.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string_view first = argc > 1 ? argv[1] : "";
    if (first == "--smoke" && argc == 2) return smoke();
    if (first == "--case" && argc == 5 &&
        std::string_view(argv[3]) == "--alloc") {
        return run_one(argv[2], argv[4]);
    }
    int rounds = DEFAULT_ROUNDS;
    if (first == "--rounds" && argc == 3) {
        rounds = std::atoi(argv[2]);
    } else if (argc != 1) {
        std::fprintf(stderr,
                     "usage: heap_bench [--smoke | --rounds N | --case NAME "
                     "--alloc system|heap]\n");
        return 2;
    }
    return rounds > 0 ? drive(argv[0], rounds) : 2;
}
