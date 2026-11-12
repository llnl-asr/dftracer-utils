// SPDX-License-Identifier: MIT
// Benchmark: per-call cost of the string hashes, the ankerl wyhash that
// dstd::StringHash used before, XXH3-64 and FNV-1a.
//
// Each case hashes a power-of-two pool of keys of one length, or of realistic
// keys, round robin. The key of the next call depends on the low bit of the sum
// so far, so the calls cannot be dropped or overlapped and the numbers are
// latency per call. The result of a rep is wall time divided by calls.
//
//   hash_bench                (all cases, 7 reps, median)
//   hash_bench --reps R       (R reps per case)

#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/hash/xxh3.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/dstd/vendor/unordered_dense.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using namespace dftracer::utils;

using Clock = std::chrono::steady_clock;

constexpr std::size_t POOL = 1024;
constexpr std::size_t CALLS = 4000000;
constexpr int DEFAULT_REPS = 7;

struct Wyhash {
    std::uint64_t operator()(dstd::StringView s) const {
        return ankerl::unordered_dense::hash<dstd::StringView>{}(s);
    }
};

struct Xxh3 {
    std::uint64_t operator()(dstd::StringView s) const {
        return dstd::hash::xxh3_64(s);
    }
};

struct Fnv1a {
    std::uint64_t operator()(dstd::StringView s) const {
        return dstd::hash::fnv1a_hash(s);
    }
};

struct KeySet {
    dstd::String label;
    dstd::Vector<dstd::String> keys;
    double mean_bytes = 0;
};

KeySet fixed_size(std::size_t len) {
    KeySet set;
    set.label = dstd::fmt<"len {}">(len);
    std::uint64_t x = dstd::hash::GOLDEN_RATIO ^ len;
    for (std::size_t i = 0; i < POOL; ++i) {
        dstd::String key(len, '\0');
        for (std::size_t j = 0; j < len; ++j) {
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
            key[j] = static_cast<char>('a' + (x >> 59) % 26);
        }
        set.keys.push_back(std::move(key));
    }
    set.mean_bytes = static_cast<double>(len);
    return set;
}

KeySet realistic() {
    KeySet set;
    set.label = "realistic";
    const char* const base[] = {
        "read",
        "write",
        "open64",
        "close",
        "lseek64",
        "fsync",
        "PyTorch.forward",
        "PyTorch.backward",
        "DataLoader.next",
        "torch.optim.step",
        "POSIX",
        "STDIO",
        "MPI",
        "COMPUTE",
        "DATA_LOADER",
        "CHECKPOINT",
        "/p/lustre1/user/dlio/data/unet3d/train/img_00123-of-01024.npz",
        "/p/lustre1/user/dlio/data/resnet50/valid/shard_000045.tfrecord",
        "/usr/WS2/user/run/checkpoints/model/epoch_12/rank_007.pt",
        "/dev/shm/dftracer/trace-0-1234-5678-00042.pfw.gz",
        "args.fhash",
        "args.size",
        "hostname.llnl.gov",
    };
    for (const char* b : base) set.keys.push_back(b);
    for (std::size_t i = 0; i < 41; ++i) {
        set.keys.push_back(
            dstd::fmt<"/p/lustre1/user/run/rank_{}/data.bin">(i * 37));
    }
    double total = 0;
    for (const auto& k : set.keys) total += static_cast<double>(k.size());
    set.mean_bytes = total / static_cast<double>(set.keys.size());
    return set;
}

template <class Fn>
double time_once(const KeySet& set, Fn fn, std::uint64_t& sink) {
    const std::size_t mask = set.keys.size() - 1;
    const dstd::String* keys = set.keys.data();
    std::uint64_t acc = 0;
    const auto start = Clock::now();
    for (std::size_t i = 0; i < CALLS; ++i) {
        acc += fn(keys[(i + (acc & 1)) & mask]);
    }
    const auto stop = Clock::now();
    sink += acc;
    return std::chrono::duration<double, std::nano>(stop - start).count() /
           static_cast<double>(CALLS);
}

template <class Fn>
double median_ns(const KeySet& set, Fn fn, int reps, std::uint64_t& sink) {
    time_once(set, fn, sink);
    dstd::Vector<double> times;
    for (int r = 0; r < reps; ++r) times.push_back(time_once(set, fn, sink));
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

void report(const KeySet& set, const char* name, double ns) {
    std::printf("%-12s %-8s %10.2f %10.0f\n", set.label.c_str(), name, ns,
                set.mean_bytes / ns * 1000.0);
}

}  // namespace

int main(int argc, char** argv) {
    int reps = DEFAULT_REPS;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
            reps = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: hash_bench [--reps R]\n");
            return 2;
        }
    }
    if (reps < 1) reps = 1;

    dstd::Vector<KeySet> sets;
    for (std::size_t len : {4, 8, 16, 32, 64, 256}) {
        sets.push_back(fixed_size(len));
    }
    sets.push_back(realistic());

    std::uint64_t sink = 0;
    std::printf("hash_bench reps=%d calls=%zu\n", reps, CALLS);
    std::printf("%-12s %-8s %10s %10s\n", "keys", "hash", "ns/hash", "MB/s");
    for (const KeySet& set : sets) {
        report(set, "wyhash", median_ns(set, Wyhash{}, reps, sink));
        report(set, "xxh3", median_ns(set, Xxh3{}, reps, sink));
        report(set, "fnv1a", median_ns(set, Fnv1a{}, reps, sink));
    }
    std::printf("sink %llu\n", static_cast<unsigned long long>(sink));
    return 0;
}
