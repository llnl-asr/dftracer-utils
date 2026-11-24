// SPDX-License-Identifier: MIT
// Eight threads run kernels on one shared Series and DataFrame at once, as
// Python threads do with the GIL released; every result equals the result of
// one thread. Run under the tsan preset to look for races in shared state.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

constexpr std::int64_t ROWS = 200000;
constexpr int THREADS = 8;
constexpr int ROUNDS = 20;
constexpr int TASKS = 7;

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    return (h ^ v) * dstd::hash::FNV1A_PRIME;
}

std::uint64_t digest_ints(const Series& s) {
    const Series flat = s.materialize();
    std::uint64_t h = dstd::hash::FNV1A_OFFSET_BASIS;
    for (const std::int64_t v : flat.values<std::int64_t>())
        h = mix(h, static_cast<std::uint64_t>(v));
    return mix(h, static_cast<std::uint64_t>(flat.length()));
}

std::uint64_t digest_strings(const Series& s) {
    std::uint64_t h = dstd::hash::FNV1A_OFFSET_BASIS;
    for (std::int64_t i = 0; i < s.length(); ++i)
        for (const char c : s.string_at(i))
            h = mix(h, static_cast<std::uint64_t>(c));
    return h;
}

struct Inputs {
    Series values;
    Series words;
    DataFrame frame;
};

Inputs make_inputs() {
    dstd::Vector<std::int64_t> v(ROWS), k(ROWS);
    dstd::Vector<dstd::String> w;
    for (std::int64_t i = 0; i < ROWS; ++i) {
        v[i] = (i * 7919) % 2001 - 1000;
        k[i] = i % 16;
        w.push_back(dstd::fmt<"w{}">(i % 997));
    }
    Inputs in;
    in.values = Series::flat_i64(v.data(), ROWS);
    in.words = Series::strings(w);
    in.frame.names.push_back(dstd::SharedString{dstd::String("k")});
    in.frame.columns.push_back(Series::flat_i64(k.data(), ROWS));
    in.frame.names.push_back(dstd::SharedString{dstd::String("v")});
    in.frame.columns.push_back(Series::flat_i64(v.data(), ROWS));
    return in;
}

std::uint64_t run_task(const Inputs& in, int task) {
    switch (task) {
        case 0:
            return digest_ints(in.values.abs());
        case 1: {
            auto s = in.values.sum();
            return s ? static_cast<std::uint64_t>(s->i64()) : 0;
        }
        case 2:
            return digest_ints(in.values.sort());
        case 3:
            return digest_ints(in.values.filter(in.values.gt(0)));
        case 4:
            return digest_strings(in.words.to_uppercase());
        case 5:
            return digest_strings(in.words.filter(in.words.str_contains("7")));
        default: {
            GroupAgg agg;
            agg.op = Agg::Sum;
            agg.column = "v";
            agg.out = "sum";
            dstd::Vector<GroupAgg> aggs;
            aggs.push_back(agg);
            auto out = in.frame.group_by(dstd::String("k"), aggs);
            if (!out) return 0;
            return digest_ints(out->column("sum"));
        }
    }
}

}  // namespace

TEST_CASE("threads share one Series and DataFrame") {
    const Inputs in = make_inputs();
    std::uint64_t expected[TASKS];
    for (int t = 0; t < TASKS; ++t) expected[t] = run_task(in, t);

    std::atomic<int> wrong{0};
    std::atomic<int> by_task[TASKS] = {};
    dstd::Vector<dstd::Thread> threads;
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&, i] {
            for (int r = 0; r < ROUNDS; ++r) {
                const int t = (i + r) % TASKS;
                if (run_task(in, t) != expected[t]) {
                    ++wrong;
                    ++by_task[t];
                }
            }
        });
    }
    for (auto& t : threads) t.join();
    for (int t = 0; t < TASKS; ++t)
        CHECK_MESSAGE(by_task[t].load() == 0, "task ", t);
    CHECK(wrong.load() == 0);
}
