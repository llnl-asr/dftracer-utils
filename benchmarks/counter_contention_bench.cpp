// Benchmark: what a byte counter costs at many threads, and what batching
// remote frees saves.
//
// The allocation sizes come from the library's own mix. The bench runs a small
// group_by plan and a small genesis in this process, reads the power-of-two
// histogram of the counting operator new and draws every size from it (sizes
// above 1 MiB are left out). The measured loops call malloc and free directly,
// so the counters under test are the only counting in them.
//
//   a  one shared atomic: fetch_add on alloc, fetch_sub on free
//   b  a per-thread counter flushed into one shared atomic every 64 KiB of
//   change c  remote free onto the owner's page chain, one CAS per free
//      against a 64-entry thread-local buffer and one CAS per chain
//
// Variant c runs in rounds. Every thread allocates a batch and deals 256-block
// slices to 16 threads (or all threads when there are fewer), so each chain
// takes pushes from up to 16 threads at once. After the frees, each owner
// drains its chain with one exchange and frees the blocks. With one thread the
// free is local and only shows the cost of the CAS itself.
//
// Results for a and b are ns per alloc+free pair per thread: wall time of all
// threads running together divided by the pairs one thread does. Variant c
// reports ns per remote free from a clock around the push phase alone, summed
// per thread and averaged over the threads (linking the 64 nodes counts). The
// whole-round pair time, barriers included, is printed on a second line. The
// median of 5 runs is printed (1 run with --smoke), once for each size cut.
//
//   counter_contention_bench           (1, 8, 32, 64, 128 threads)
//   counter_contention_bench --smoke   (1 and 8 threads, tiny sizes, checks)
//
// Every run checks that live bytes end at zero, that the freed bytes equal the
// allocated bytes and that both equal the bytes the size tables imply.

#define DFTRACER_UTILS_MEMORY_PROBE_DEFINE_NEW
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/genesis/genesis.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "memory_probe.h"
#include "memory_workloads.h"

using namespace dftracer::utils;
using dftracer::utils::coro::CoroTask;
namespace df = dftracer::utils::dataframe;
namespace mp = dftu::memory_probe;
using namespace dftu::memory_workloads;

namespace {

#if defined(__APPLE__) && defined(__aarch64__)
constexpr std::size_t CACHE_LINE = 128;
#else
constexpr std::size_t CACHE_LINE = 64;
#endif

constexpr std::int64_t FLUSH_BYTES = 64 * 1024;
constexpr std::size_t TABLE_SIZE = 4096;
constexpr std::size_t RING = 32;
constexpr std::size_t MIN_BLOCK = 16;
constexpr std::size_t MAX_BUCKET = 20;
constexpr std::size_t SMALL_BUCKET = 12;
constexpr std::size_t REMOTE_BUFFER = 64;
constexpr std::size_t SLICE = 256;
constexpr std::size_t MAX_GROUPS = 16;
constexpr std::size_t RUNS = 5;

static_assert(SLICE % REMOTE_BUFFER == 0);
static_assert(TABLE_SIZE % (SLICE * MAX_GROUPS) == 0);

struct Sizes {
    std::int64_t frame_rows;
    std::int64_t genesis_calls;
    std::size_t tables_per_thread;
    std::size_t runs;
};

constexpr Sizes SMOKE_SIZES{20'000, 2'000, 1, 1};
constexpr Sizes FULL_SIZES{1'000'000, 200'000, 128, RUNS};

constexpr std::array<std::size_t, 5> FULL_THREADS{1, 8, 32, 64, 128};
constexpr std::array<std::size_t, 2> SMOKE_THREADS{1, 8};

enum class Variant : std::uint8_t {
    SHARED_ATOMIC,
    PER_THREAD,
    REMOTE_CAS_PER_FREE,
    REMOTE_BUFFERED,
};

struct VariantInfo {
    Variant id;
    const char* name;
};

constexpr VariantInfo VARIANTS[] = {
    {Variant::SHARED_ATOMIC, "a shared atomic"},
    {Variant::PER_THREAD, "b per-thread 64K"},
    {Variant::REMOTE_CAS_PER_FREE, "c CAS per free"},
    {Variant::REMOTE_BUFFERED, "c buffer of 64"},
};

struct alignas(CACHE_LINE) ThreadStats {
    std::uint64_t allocated = 0;
    std::uint64_t freed = 0;
    std::uint64_t push_ns = 0;
};

struct alignas(CACHE_LINE) PaddedCounter {
    std::atomic<std::int64_t> value{0};
};

struct Node {
    Node* next;
    std::uint64_t size;
};

static_assert(sizeof(Node) == MIN_BLOCK);

struct alignas(CACHE_LINE) PaddedHead {
    std::atomic<Node*> head{nullptr};
};

// xorshift64* by Vigna, public domain.
std::uint64_t next_random(std::uint64_t& s) {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1DULL;
}

void group_by(const Sizes& s) {
    const df::LazyFrame plan =
        gen(s.frame_rows, s.frame_rows, 1)
            .group_by(std::vector<std::string>{"k", "v"},
                      {df::GroupAgg{df::Agg::Sum, "t", "t_sum"}});
    default_runtime()
        .submit(drain_count(plan.memory_budget(PLAN_BUDGET)))
        .get();
}

void genesis(const std::string& root) {
    default_runtime()
        .submit(run_coro_scope(
            default_runtime().executor(),
            [&](CoroScope& ctx) { return genesis_run(ctx, root); }))
        .wait();
}

std::string prepare_genesis(const std::string& dir, const Sizes& s) {
    fs::remove_all(dir);
    const std::string run = dir + "/genesis/app1/sys1/in1/nodes_1/ppn_1";
    fs::create_directories(run);
    std::ofstream(run + "/summary.json")
        << R"({"ok":true,"app":"app1","system":"sys1","input":"in1",)"
           R"("nodes":1,"ppn":)"
        << RANKS << R"(,"sets":["set1"]})";
    write_gz(run + "/compacted/set1.pfw.gz", genesis_text(s.genesis_calls));
    return dir + "/genesis";
}

struct SizeMix {
    std::array<std::uint64_t, mp::HISTOGRAM_BUCKETS> counts{};
    std::uint64_t total = 0;
};

std::uint64_t calls_up_to(const SizeMix& m, std::size_t max_bucket) {
    std::uint64_t n = 0;
    for (std::size_t b = 0; b <= max_bucket; ++b) n += m.counts[b];
    return n;
}

SizeMix collect_mix(const Sizes& s) {
    const std::string dir =
        (fs::temp_directory_path() / "counter_contention_bench").string();
    const std::string root = prepare_genesis(dir, s);
    SizeMix mix_out;
    std::array<std::uint64_t, mp::HISTOGRAM_BUCKETS> before{};
    for (std::size_t b = 0; b < before.size(); ++b)
        before[b] = mp::histogram_count(b);
    group_by(s);
    genesis(root);
    for (std::size_t b = 0; b < before.size(); ++b) {
        mix_out.counts[b] = mp::histogram_count(b) - before[b];
        mix_out.total += mix_out.counts[b];
    }
    fs::remove_all(dir);
    return mix_out;
}

void print_mix(const SizeMix& m) {
    std::printf("size mix of group_by + genesis (operator new calls: %llu)\n",
                static_cast<unsigned long long>(m.total));
    std::printf("%-22s %12s %8s\n", "request bytes", "calls", "share");
    for (std::size_t b = 0; b < m.counts.size(); ++b) {
        if (m.counts[b] == 0) continue;
        const std::uint64_t lo = b == 0 ? 0 : 1ULL << (b - 1);
        const std::uint64_t hi = b == 0 ? 0 : (1ULL << b) - 1;
        std::printf("%10llu - %-9llu %12llu %7.2f%%%s\n",
                    static_cast<unsigned long long>(lo),
                    static_cast<unsigned long long>(hi),
                    static_cast<unsigned long long>(m.counts[b]),
                    100.0 * static_cast<double>(m.counts[b]) /
                        static_cast<double>(m.total),
                    b > MAX_BUCKET ? "  (left out, above 1 MiB)" : "");
    }
    std::printf("sizes below %zu bytes are raised to %zu\n", MIN_BLOCK,
                MIN_BLOCK);
    std::printf(
        "cut 1 draws from sizes up to 1 MiB, cut 2 from sizes below 4 KiB\n");
}

struct SizeTables {
    std::vector<std::vector<std::size_t>> table;
    std::uint64_t bytes_per_pass = 0;
};

SizeTables make_tables(const SizeMix& m, std::size_t max_bucket,
                       std::size_t threads) {
    const std::uint64_t used = calls_up_to(m, max_bucket);
    SizeTables out;
    out.table.resize(threads);
    for (std::size_t t = 0; t < threads; ++t) {
        std::uint64_t state = mix(t + 1);
        auto& table = out.table[t];
        table.resize(TABLE_SIZE);
        for (std::size_t i = 0; i < TABLE_SIZE; ++i) {
            std::uint64_t pick = next_random(state) % used;
            std::size_t bucket = 0;
            while (pick >= m.counts[bucket]) pick -= m.counts[bucket++];
            std::size_t size = 0;
            if (bucket > 0) {
                const std::uint64_t lo = 1ULL << (bucket - 1);
                size = static_cast<std::size_t>(lo + next_random(state) % lo);
            }
            table[i] = std::max(size, MIN_BLOCK);
            out.bytes_per_pass += table[i];
        }
    }
    return out;
}

void* checked_malloc(std::size_t n) {
    void* p = std::malloc(n);
    if (p == nullptr) std::abort();
    return p;
}

class SharedCounter {
   public:
    explicit SharedCounter(PaddedCounter& shared) : shared_(shared.value) {}
    void add(std::size_t n) {
        shared_.fetch_add(static_cast<std::int64_t>(n),
                          std::memory_order_relaxed);
    }
    void sub(std::size_t n) {
        shared_.fetch_sub(static_cast<std::int64_t>(n),
                          std::memory_order_relaxed);
    }
    void finish() {}

   private:
    std::atomic<std::int64_t>& shared_;
};

class BatchedCounter {
   public:
    explicit BatchedCounter(PaddedCounter& shared) : shared_(shared.value) {}
    void add(std::size_t n) {
        delta_ += static_cast<std::int64_t>(n);
        gross_ += static_cast<std::int64_t>(n);
        maybe_flush();
    }
    void sub(std::size_t n) {
        delta_ -= static_cast<std::int64_t>(n);
        gross_ += static_cast<std::int64_t>(n);
        maybe_flush();
    }
    void finish() { flush(); }

   private:
    void maybe_flush() {
        if (gross_ >= FLUSH_BYTES) flush();
    }
    void flush() {
        shared_.fetch_add(delta_, std::memory_order_relaxed);
        delta_ = 0;
        gross_ = 0;
    }

    std::atomic<std::int64_t>& shared_;
    std::int64_t delta_ = 0;
    std::int64_t gross_ = 0;
};

template <class Counter>
void counter_worker(Counter counter, const std::vector<std::size_t>& table,
                    std::size_t pairs, ThreadStats& st) {
    struct Slot {
        void* p = nullptr;
        std::size_t n = 0;
    };
    Slot ring[RING];
    const auto release = [&](Slot& s) {
        counter.sub(s.n);
        st.freed += s.n;
        std::free(s.p);
        s.p = nullptr;
    };
    for (std::size_t i = 0; i < pairs; ++i) {
        Slot& s = ring[i % RING];
        if (s.p != nullptr) release(s);
        const std::size_t n = table[i % TABLE_SIZE];
        void* p = checked_malloc(n);
        *static_cast<volatile char*>(p) = 1;
        counter.add(n);
        st.allocated += n;
        s = Slot{p, n};
    }
    for (Slot& s : ring)
        if (s.p != nullptr) release(s);
    counter.finish();
}

void push_one(std::atomic<Node*>& head, Node* node) {
    Node* old = head.load(std::memory_order_relaxed);
    do {
        node->next = old;
    } while (!head.compare_exchange_weak(old, node, std::memory_order_release,
                                         std::memory_order_relaxed));
}

void push_chain(std::atomic<Node*>& head, Node* first, Node* last) {
    Node* old = head.load(std::memory_order_relaxed);
    do {
        last->next = old;
    } while (!head.compare_exchange_weak(old, first, std::memory_order_release,
                                         std::memory_order_relaxed));
}

struct RemoteShared {
    RemoteShared(std::size_t threads, std::size_t batch)
        : chains(threads),
          outbox(threads, std::vector<Node*>(batch)),
          barrier(static_cast<std::ptrdiff_t>(threads)) {}

    std::vector<PaddedHead> chains;
    std::vector<std::vector<Node*>> outbox;
    std::barrier<> barrier;
};

template <bool BUFFERED>
void remote_worker(RemoteShared& sh, std::size_t t, std::size_t threads,
                   std::size_t groups, const std::vector<std::size_t>& table,
                   std::size_t rounds, ThreadStats& st) {
    const std::size_t batch = SLICE * groups;
    std::size_t at = 0;
    for (std::size_t r = 0; r < rounds; ++r) {
        Node** out = sh.outbox[t].data();
        for (std::size_t k = 0; k < batch; ++k) {
            const std::size_t n = table[at++ % TABLE_SIZE];
            Node* node = static_cast<Node*>(checked_malloc(n));
            node->size = n;
            out[k] = node;
            st.allocated += n;
        }
        sh.barrier.arrive_and_wait();
        const auto push_begin = std::chrono::steady_clock::now();
        for (std::size_t g = 0; g < groups; ++g) {
            const std::size_t owner = (t + threads - g) % threads;
            Node** blocks = sh.outbox[owner].data() + g * SLICE;
            std::atomic<Node*>& chain = sh.chains[owner].head;
            if constexpr (BUFFERED) {
                Node* buffer[REMOTE_BUFFER];
                for (std::size_t k = 0; k < SLICE; k += REMOTE_BUFFER) {
                    for (std::size_t j = 0; j < REMOTE_BUFFER; ++j) {
                        buffer[j] = blocks[k + j];
                        if (j > 0) buffer[j - 1]->next = buffer[j];
                    }
                    push_chain(chain, buffer[0], buffer[REMOTE_BUFFER - 1]);
                }
            } else {
                for (std::size_t k = 0; k < SLICE; ++k)
                    push_one(chain, blocks[k]);
            }
        }
        st.push_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - push_begin)
                .count());
        sh.barrier.arrive_and_wait();
        Node* p =
            sh.chains[t].head.exchange(nullptr, std::memory_order_acquire);
        while (p != nullptr) {
            Node* next = p->next;
            st.freed += p->size;
            std::free(p);
            p = next;
        }
    }
}

struct RunResult {
    double ns_per_pair = 0;
    double ns_per_remote_free = 0;
    std::int64_t counter_end = 0;
    std::uint64_t allocated = 0;
    std::uint64_t freed = 0;
};

template <class Body>
double time_threads(std::size_t threads, Body body) {
    std::barrier<> gate(static_cast<std::ptrdiff_t>(threads + 1));
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (std::size_t t = 0; t < threads; ++t)
        pool.emplace_back([&gate, &body, t] {
            gate.arrive_and_wait();
            body(t);
        });
    gate.arrive_and_wait();
    const auto t0 = std::chrono::steady_clock::now();
    for (std::thread& th : pool) th.join();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

RunResult run_once(Variant v, std::size_t threads, const SizeTables& tables,
                   std::size_t pairs) {
    std::vector<ThreadStats> stats(threads);
    PaddedCounter shared;
    RunResult r;
    double ns = 0;
    switch (v) {
        case Variant::SHARED_ATOMIC:
            ns = time_threads(threads, [&](std::size_t t) {
                counter_worker(SharedCounter(shared), tables.table[t], pairs,
                               stats[t]);
            });
            break;
        case Variant::PER_THREAD:
            ns = time_threads(threads, [&](std::size_t t) {
                counter_worker(BatchedCounter(shared), tables.table[t], pairs,
                               stats[t]);
            });
            break;
        case Variant::REMOTE_CAS_PER_FREE:
        case Variant::REMOTE_BUFFERED: {
            const std::size_t groups = std::min(threads, MAX_GROUPS);
            const std::size_t rounds = pairs / (SLICE * groups);
            RemoteShared sh(threads, SLICE * groups);
            ns = time_threads(threads, [&](std::size_t t) {
                if (v == Variant::REMOTE_BUFFERED)
                    remote_worker<true>(sh, t, threads, groups, tables.table[t],
                                        rounds, stats[t]);
                else
                    remote_worker<false>(sh, t, threads, groups,
                                         tables.table[t], rounds, stats[t]);
            });
            break;
        }
    }
    r.ns_per_pair = ns / static_cast<double>(pairs);
    std::uint64_t push_ns = 0;
    for (const ThreadStats& s : stats) {
        r.allocated += s.allocated;
        r.freed += s.freed;
        push_ns += s.push_ns;
    }
    r.ns_per_remote_free = static_cast<double>(push_ns) /
                           static_cast<double>(threads) /
                           static_cast<double>(pairs);
    const bool remote =
        v == Variant::REMOTE_CAS_PER_FREE || v == Variant::REMOTE_BUFFERED;
    r.counter_end = remote ? static_cast<std::int64_t>(r.allocated - r.freed)
                           : shared.value.load(std::memory_order_relaxed);
    return r;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    const bool smoke = argc == 2 && std::string_view(argv[1]) == "--smoke";
    if (argc > 2 || (argc == 2 && !smoke)) {
        std::fprintf(stderr, "usage: counter_contention_bench [--smoke]\n");
        return 2;
    }
    const Sizes sizes = smoke ? SMOKE_SIZES : FULL_SIZES;
    const std::vector<std::size_t> thread_counts =
        smoke ? std::vector<std::size_t>(SMOKE_THREADS.begin(),
                                         SMOKE_THREADS.end())
              : std::vector<std::size_t>(FULL_THREADS.begin(),
                                         FULL_THREADS.end());

    const SizeMix mixture = collect_mix(sizes);
    print_mix(mixture);
    if (calls_up_to(mixture, SMALL_BUCKET) == 0) {
        std::printf("empty size mix  FAIL\n");
        return 1;
    }

    const std::size_t pairs = TABLE_SIZE * sizes.tables_per_thread;
    int failures = 0;
    for (const std::size_t cut : {MAX_BUCKET, SMALL_BUCKET}) {
        std::vector<std::vector<double>> medians(std::size(VARIANTS));
        std::vector<std::vector<double>> with_barriers(std::size(VARIANTS));
        for (const std::size_t threads : thread_counts) {
            const SizeTables tables = make_tables(mixture, cut, threads);
            const std::uint64_t expected =
                tables.bytes_per_pass * sizes.tables_per_thread;
            for (std::size_t vi = 0; vi < std::size(VARIANTS); ++vi) {
                const bool remote =
                    VARIANTS[vi].id == Variant::REMOTE_CAS_PER_FREE ||
                    VARIANTS[vi].id == Variant::REMOTE_BUFFERED;
                std::vector<double> ns;
                std::vector<double> ns_round;
                for (std::size_t run = 0; run < sizes.runs; ++run) {
                    const RunResult r =
                        run_once(VARIANTS[vi].id, threads, tables, pairs);
                    ns.push_back(remote ? r.ns_per_remote_free : r.ns_per_pair);
                    ns_round.push_back(r.ns_per_pair);
                    if (r.counter_end != 0 || r.allocated != r.freed ||
                        r.allocated != expected) {
                        std::printf(
                            "%s at %zu threads: live %lld, allocated %llu, "
                            "freed %llu, expected %llu  FAIL\n",
                            VARIANTS[vi].name, threads,
                            static_cast<long long>(r.counter_end),
                            static_cast<unsigned long long>(r.allocated),
                            static_cast<unsigned long long>(r.freed),
                            static_cast<unsigned long long>(expected));
                        ++failures;
                    }
                }
                medians[vi].push_back(median(ns));
                if (remote) with_barriers[vi].push_back(median(ns_round));
            }
        }

        std::printf(
            "\ncut %d: median of %zu run%s, %zu pairs per thread\n"
            "a, b: ns per alloc+free pair per thread\n"
            "c: ns per remote free, push phase only (the barriers, the "
            "allocs and the drain are not timed)\n",
            cut == MAX_BUCKET ? 1 : 2, sizes.runs, sizes.runs == 1 ? "" : "s",
            pairs);
        std::printf("%-18s", "variant");
        for (const std::size_t threads : thread_counts)
            std::printf(" %9zu", threads);
        std::printf("   (threads)\n");
        for (std::size_t vi = 0; vi < std::size(VARIANTS); ++vi) {
            std::printf("%-18s", VARIANTS[vi].name);
            for (const double v : medians[vi]) std::printf(" %9.1f", v);
            std::printf("\n");
        }
        std::printf("c, ns per alloc+free pair per thread, with barriers\n");
        for (std::size_t vi = 0; vi < std::size(VARIANTS); ++vi) {
            if (with_barriers[vi].empty()) continue;
            std::printf("%-18s", VARIANTS[vi].name);
            for (const double v : with_barriers[vi]) std::printf(" %9.1f", v);
            std::printf("\n");
        }
    }
    return failures == 0 ? 0 : 1;
}
