// Benchmark: adding 1M lognormal(sigma 2) values to a quantile sketch.
//
// Run one case per process so `perf stat` counts only that case:
//   perf stat -e instructions,cycles,cache-misses quantile_store_bench CASE
//
// Cases:
//   dd_add          DDSketch::add, one value at a time.
//   merge_add       MergeSketch::add, one value at a time.
//   store_add       QuantileStore::add, one value at a time.
//   store_add_many  QuantileStore::add_many in batches of 4096.
//
// The values are generated before the clock starts. The result is wall time
// divided by values; `--baseline` runs the generation alone, so its
// instructions can be subtracted from a perf stat run.

#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/quantile_store.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <random>

namespace {

using dftracer::utils::dstd::Vector;
using dftracer::utils::index::schemas::dft::agg::QuantileStore;
namespace df = dftracer::utils::dataframe;

constexpr std::size_t VALUES = 1000000;
constexpr std::size_t BATCH = 4096;

}  // namespace

int main(int argc, char** argv) {
    const char* name = argc > 1 ? argv[1] : "store_add_many";
    std::mt19937_64 rng(1);
    std::lognormal_distribution<double> dist(std::log(1e4), 2.0);
    Vector<double> v(VALUES);
    for (auto& x : v) x = dist(rng);

    const auto t0 = std::chrono::steady_clock::now();
    double sink = 0.0;
    if (!std::strcmp(name, "dd_add")) {
        df::DDSketch s;
        for (double x : v) s.add(x);
        sink = s.quantile(0.5);
    } else if (!std::strcmp(name, "merge_add")) {
        df::MergeSketch s;
        for (double x : v) s.add(x);
        sink = s.quantile(0.5);
    } else if (!std::strcmp(name, "store_add")) {
        QuantileStore s;
        for (double x : v) s.add(x);
        sink = s.quantile(0.5);
    } else if (!std::strcmp(name, "store_add_many")) {
        QuantileStore s;
        for (std::size_t at = 0; at < VALUES; at += BATCH)
            s.add_many({v.data() + at, std::min(BATCH, VALUES - at)});
        sink = s.quantile(0.5);
    } else if (std::strcmp(name, "--baseline") != 0) {
        std::fprintf(stderr, "unknown case %s\n", name);
        return 2;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count() / VALUES;
    std::printf("%s: %.2f ns/value (median %.1f)\n", name, ns, sink);
    return 0;
}
