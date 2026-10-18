// Benchmark: coalescing decoded gzip members across concurrent readers.
//
// Models a loaded server: R concurrent queries each touch the same M members
// (overlapping time windows). Compares wall-clock and decode count for three
// single-flight strategies:
//   none      - every request decodes its member (no sharing)
//   asyncmutex- share one Entry per member, serialize on a coro::AsyncMutex
//   asynconce - share one AsyncOnce per member, broadcast wake
//
// Not a unit test (too slow for CI): built only with
// DFTRACER_UTILS_BUILD_BENCHMARKS=ON and run manually.
//
//   member_cache_bench [uncompressed_member_MB] [members] [concurrent_readers]

#include <dftracer/utils/runtime/coro/async_once.h>
#include <dftracer/utils/runtime/coro/async_mutex.h>
#include <dftracer/utils/runtime/coro/coro.h>
#include <dftracer/utils/runtime/coro/when_all.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dftracer::utils;
namespace compress = dftracer::utils::utilities::fileio::compress;

using Bytes = std::shared_ptr<const std::vector<std::uint8_t>>;

namespace {

std::atomic<std::uint64_t> g_decodes{0};

// A compressed gzip member plus its decoded size, shared by all keys.
struct Blob {
    std::vector<std::uint8_t> compressed;
    std::size_t uncompressed = 0;
};

Blob make_blob(std::size_t uncompressed_bytes) {
    // Mildly compressible bytes so decode does real work (not all-zero).
    std::vector<std::uint8_t> raw(uncompressed_bytes);
    std::uint32_t x = 0x12345678u;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        x = x * 1664525u + 1013904223u;
        raw[i] = static_cast<std::uint8_t>((x >> 24) & 0x3F);
    }
    compress::GzipMemberCompressor comp(6);
    auto member = comp.compress_member(raw.data(), raw.size());
    if (!member) {
        std::fprintf(stderr, "compress failed\n");
        std::exit(1);
    }
    return Blob{std::move(*member), uncompressed_bytes};
}

// The actual work: a real libdeflate decode of the shared blob.
coro::CoroTask<Bytes> decode(const Blob* blob) {
    g_decodes.fetch_add(1, std::memory_order_relaxed);
    compress::GzipMemberDecompressor dec;
    auto out = dec.decompress_member(blob->compressed.data(),
                                     blob->compressed.size(),
                                     blob->uncompressed);
    if (!out) {
        std::fprintf(stderr, "decode failed\n");
        std::exit(1);
    }
    co_return std::make_shared<const std::vector<std::uint8_t>>(std::move(*out));
}

// --- strategy: no coalescing ------------------------------------------------
struct NoCoalesce {
    coro::CoroTask<Bytes> get(std::uint64_t /*key*/, const Blob* blob) {
        co_return co_await decode(blob);
    }
};

// --- strategy: AsyncMutex single-flight (the shipped cache) ------------------
struct MutexCoalesce {
    struct Entry {
        coro::AsyncMutex mutex;
        bool ready = false;
        Bytes bytes;
    };
    std::mutex m_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Entry>> map_;

    coro::CoroTask<Bytes> get(std::uint64_t key, const Blob* blob) {
        std::shared_ptr<Entry> e;
        {
            std::lock_guard<std::mutex> g(m_);
            auto& slot = map_[key];
            if (!slot) slot = std::make_shared<Entry>();
            e = slot;
        }
        co_await e->mutex.lock();
        coro::AsyncMutexGuard guard(e->mutex);
        if (!e->ready) {
            e->bytes = co_await decode(blob);
            e->ready = true;
        }
        co_return e->bytes;
    }
};

// --- strategy: AsyncOnce single-flight (broadcast wake) ----------------------
struct OnceCoalesce {
    std::mutex m_;
    std::unordered_map<std::uint64_t, std::shared_ptr<coro::AsyncOnce<Bytes>>>
        map_;

    coro::CoroTask<Bytes> get(std::uint64_t key, const Blob* blob) {
        std::shared_ptr<coro::AsyncOnce<Bytes>> once;
        {
            std::lock_guard<std::mutex> g(m_);
            auto& slot = map_[key];
            if (!slot) slot = std::make_shared<coro::AsyncOnce<Bytes>>();
            once = slot;
        }
        co_return co_await once->get(
            [blob]() -> coro::CoroTask<Bytes> { co_return co_await decode(blob); });
    }
};

template <typename Strategy>
double run_round(Runtime& rt, Strategy& strat, const Blob& blob,
                 int members, int readers) {
    g_decodes.store(0, std::memory_order_relaxed);
    const auto* blob_ptr = &blob;
    auto t0 = std::chrono::steady_clock::now();

    auto task = run_coro_scope(
        rt.executor(),
        [&strat, blob_ptr, members, readers](CoroScope&) -> coro::CoroTask<void> {
            std::vector<coro::CoroTask<Bytes>> tasks;
            tasks.reserve(static_cast<std::size_t>(readers) * members);
            // Reader r requests every member -> R readers overlap on each.
            for (int r = 0; r < readers; ++r) {
                for (int m = 0; m < members; ++m) {
                    tasks.push_back(strat.get(static_cast<std::uint64_t>(m),
                                              blob_ptr));
                }
            }
            co_await coro::when_all(std::move(tasks));
            co_return;
        });
    rt.submit(std::move(task), "bench").wait();

    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// --- wake micro-bench -------------------------------------------------------
// Each round uses a FRESH primitive: N concurrent gets pile onto one leader
// whose producer yields a few times so all N are waiting before it completes.
// This isolates coalescing + wake cost (no decode, no map lookup), which is
// where a blocking std::mutex would bite.
coro::CoroTask<int> cheap_producer() {
    for (int i = 0; i < 3; ++i) co_await coro::yield();
    co_return 1;
}

double bench_mutex_wake(Runtime& rt, int rounds, int waiters) {
    auto t0 = std::chrono::steady_clock::now();
    auto task = run_coro_scope(
        rt.executor(), [rounds, waiters](CoroScope&) -> coro::CoroTask<void> {
            for (int r = 0; r < rounds; ++r) {
                struct Entry {
                    coro::AsyncMutex mutex;
                    bool ready = false;
                    int value = 0;
                };
                auto e = std::make_shared<Entry>();
                std::vector<coro::CoroTask<int>> tasks;
                tasks.reserve(waiters);
                for (int w = 0; w < waiters; ++w) {
                    tasks.push_back([](std::shared_ptr<Entry> ent)
                                        -> coro::CoroTask<int> {
                        co_await ent->mutex.lock();
                        coro::AsyncMutexGuard g(ent->mutex);
                        if (!ent->ready) {
                            ent->value = co_await cheap_producer();
                            ent->ready = true;
                        }
                        co_return ent->value;
                    }(e));
                }
                co_await coro::when_all(std::move(tasks));
            }
            co_return;
        });
    rt.submit(std::move(task), "bench").wait();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

double bench_once_wake(Runtime& rt, int rounds, int waiters) {
    auto t0 = std::chrono::steady_clock::now();
    auto task = run_coro_scope(
        rt.executor(), [rounds, waiters](CoroScope&) -> coro::CoroTask<void> {
            for (int r = 0; r < rounds; ++r) {
                auto once = std::make_shared<coro::AsyncOnce<int>>();
                std::vector<coro::CoroTask<int>> tasks;
                tasks.reserve(waiters);
                for (int w = 0; w < waiters; ++w) {
                    tasks.push_back(
                        [](std::shared_ptr<coro::AsyncOnce<int>> o)
                            -> coro::CoroTask<int> {
                            co_return co_await o->get(cheap_producer);
                        }(once));
                }
                co_await coro::when_all(std::move(tasks));
            }
            co_return;
        });
    rt.submit(std::move(task), "bench").wait();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t member_mb = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 16;
    int members = argc > 2 ? std::atoi(argv[2]) : 8;
    int readers = argc > 3 ? std::atoi(argv[3]) : 32;

    std::printf("member=%zuMB members=%d readers=%d  (requests=%d)\n", member_mb,
                members, readers, members * readers);

    Blob blob = make_blob(member_mb * 1024 * 1024);
    std::printf("compressed %.1f MB -> %.1f MB uncompressed\n",
                blob.compressed.size() / 1048576.0,
                blob.uncompressed / 1048576.0);

    Runtime rt(0);  // hardware_concurrency workers

    // Warm up (first decode primes libdeflate/zlib functables).
    {
        NoCoalesce warm;
        run_round(rt, warm, blob, 1, 1);
    }

    for (int rep = 0; rep < 3; ++rep) {
        NoCoalesce none;
        double t_none = run_round(rt, none, blob, members, readers);
        std::uint64_t d_none = g_decodes.load();

        MutexCoalesce mtx;
        double t_mtx = run_round(rt, mtx, blob, members, readers);
        std::uint64_t d_mtx = g_decodes.load();

        OnceCoalesce once;
        double t_once = run_round(rt, once, blob, members, readers);
        std::uint64_t d_once = g_decodes.load();

        std::printf(
            "[rep %d] none: %8.2f ms (%llu decodes) | "
            "asyncmutex: %8.2f ms (%llu) %.2fx | "
            "asynconce: %8.2f ms (%llu) %.2fx\n",
            rep, t_none, (unsigned long long)d_none, t_mtx,
            (unsigned long long)d_mtx, t_none / t_mtx, t_once,
            (unsigned long long)d_once, t_none / t_once);
    }

    // Wake micro-bench: cheap producer, high fan-in, so sync/wake dominates.
    const int rounds = 2000;
    const int waiters = 256;
    std::printf("\nwake micro-bench: %d rounds x %d waiters (cheap producer)\n",
                rounds, waiters);
    for (int rep = 0; rep < 3; ++rep) {
        double t_mtx = bench_mutex_wake(rt, rounds, waiters);
        double t_once = bench_once_wake(rt, rounds, waiters);
        std::printf("[rep %d] asyncmutex: %8.2f ms | asynconce: %8.2f ms  (%.2fx)\n",
                    rep, t_mtx, t_once, t_mtx / t_once);
    }

    rt.shutdown();
    return 0;
}
