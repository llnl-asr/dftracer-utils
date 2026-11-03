// SPDX-License-Identifier: MIT
// Benchmark: Prefilter::may_match per line for queries with 1, 3 and 16
// needles, on synthetic trace lines where about a quarter pass, and for one
// OR clause of 2 to 16 needles. With PREFILTER_PER_NEEDLE set in the
// environment every case runs may_match_per_needle, to compare the paths.
//
// Built only with DFTRACER_UTILS_BUILD_BENCHMARKS=ON and run manually.
//
//   prefilter_bench [lines] [rounds]

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/plan/prefilter.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace dftracer::utils;

using dftracer::utils::index::plan::Prefilter;

namespace {

constexpr const char* NAMES[] = {
    "read",    "write",  "open64",   "MPI_Send", "MPI_Recv",      "cudaMemcpy",
    "pread64", "fwrite", "H5Dwrite", "lseek64",  "MPI_Allreduce", "close"};
constexpr const char* CATS[] = {"POSIX", "STDIO", "MPI"};

dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> make_lines(
    std::size_t n) {
    std::mt19937 rng(12345);
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto name = rng() % 12;
        const auto cat = rng() % 3;
        const auto dir = rng() % 4;
        out.push_back(
            R"({"id":)" + dftracer::utils::dstd::to_string(i) + R"(,"name":")" +
            NAMES[name] + R"(","cat":")" + CATS[cat] + R"(","pid":)" +
            dftracer::utils::dstd::to_string(1000 + rng() % 64) + R"(,"tid":)" +
            dftracer::utils::dstd::to_string(1000 + rng() % 64) + R"(,"ts":)" +
            dftracer::utils::dstd::to_string(1700000000000ULL + i * 37) +
            R"(,"dur":)" + dftracer::utils::dstd::to_string(rng() % 5000) +
            R"(,"ph":"X","args":{"fname":"/p/lustre/scratch/u)" +
            dftracer::utils::dstd::to_string(rng() % 8) + "/data/file_" +
            dftracer::utils::dstd::to_string(rng() % 256) + R"(.dat","size":)" +
            dftracer::utils::dstd::to_string(rng() % 1048576) +
            R"(,"hostname":"node)" + dftracer::utils::dstd::to_string(dir) +
            R"("}})");
    }
    return out;
}

void run(
    const char* label, const dftracer::utils::dstd::String& query,
    const dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& lines,
    int rounds) {
    const Prefilter p(duql::parse_or_throw(query));
    std::size_t needles = 0;
    for (const auto& c : p.clauses()) needles += c.size();
    const bool per_needle = std::getenv("PREFILTER_PER_NEEDLE") != nullptr;
    dftracer::utils::dstd::Vector<double> ns;
    std::size_t passed = 0;
    for (int r = 0; r < rounds; ++r) {
        passed = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto& l : lines)
            passed += (per_needle ? p.may_match_per_needle(l) : p.may_match(l))
                          ? 1
                          : 0;
        const auto t1 = std::chrono::steady_clock::now();
        ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() /
                     static_cast<double>(lines.size()));
    }
    std::sort(ns.begin(), ns.end());
    std::printf("%-8s needles=%2zu passed=%7zu  median %8.1f ns/line\n", label,
                needles, passed, ns[ns.size() / 2]);
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t n =
        argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 9;
    const auto lines = make_lines(n);
    run("one", R"(name == "MPI_Send")", lines, rounds);
    run("three",
        R"(name == "read" and cat == "POSIX" and args.hostname == "node1")",
        lines, rounds);
    dftracer::utils::dstd::String in = "pid in [";
    for (int i = 0; i < 16; ++i) {
        if (i) in += ", ";
        in += dftracer::utils::dstd::to_string(1000 + i * 4);
    }
    run("sixteen", in + "]", lines, rounds);
    for (const bool miss : {false, true}) {
        for (const int k : {2, 4, 6, 8, 12, 16}) {
            dftracer::utils::dstd::String names = "name in [";
            for (int i = 0; i < k; ++i) {
                if (i) names += ", ";
                names += miss ? "\"absent" +
                                    dftracer::utils::dstd::to_string(i) + "\""
                              : dftracer::utils::dstd::String("\"") +
                                    NAMES[i % 12] + "\"";
            }
            run(((miss ? "miss" : "or") + dftracer::utils::dstd::to_string(k))
                    .c_str(),
                names + "]", lines, rounds);
        }
    }
    return 0;
}
