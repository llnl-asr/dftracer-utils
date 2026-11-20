// SPDX-License-Identifier: MIT
// unique and value_counts of a long column of 1 to 8 byte values give the
// first-seen order and the counts of a plain walk, whether the values mostly
// repeat (numbered in parallel runs), mostly differ (numbered by partition) or
// the engine runs on one thread.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <algorithm>
#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

constexpr std::int64_t N = 700'000;

struct Reference {
    dstd::Vector<std::int64_t> distinct;  // first-seen order
    dstd::Vector<std::int64_t> counts;    // per distinct value
    dstd::Vector<std::int64_t> by_count;  // indices into distinct, best first
};

dstd::Vector<std::int64_t> column(std::int64_t distinct) {
    dstd::Vector<std::int64_t> v(static_cast<std::size_t>(N));
    for (std::int64_t i = 0; i < N; ++i)
        v[static_cast<std::size_t>(i)] = static_cast<std::int64_t>(
            dstd::hash::splitmix64(static_cast<std::uint64_t>(i)) %
            static_cast<std::uint64_t>(distinct));
    return v;
}

Reference walk(dstd::Span<const std::int64_t> v,
               dstd::Span<const std::uint8_t> valid) {
    Reference r;
    dstd::HashMap<std::int64_t, std::size_t> at;
    for (std::int64_t i = 0; i < N; ++i) {
        if (!valid.empty() &&
            !((valid[static_cast<std::size_t>(i >> 3)] >> (i & 7)) & 1))
            continue;
        auto [it, fresh] =
            at.try_emplace(v[static_cast<std::size_t>(i)], r.distinct.size());
        if (fresh) {
            r.distinct.push_back(v[static_cast<std::size_t>(i)]);
            r.counts.push_back(0);
        }
        ++r.counts[it->second];
    }
    r.by_count.resize(r.distinct.size());
    for (std::size_t k = 0; k < r.by_count.size(); ++k)
        r.by_count[k] = static_cast<std::int64_t>(k);
    std::sort(r.by_count.begin(), r.by_count.end(),
              [&](std::int64_t a, std::int64_t b) {
                  const auto ca = r.counts[static_cast<std::size_t>(a)];
                  const auto cb = r.counts[static_cast<std::size_t>(b)];
                  return ca != cb ? ca > cb : a < b;
              });
    return r;
}

void check(std::int64_t distinct, bool with_nulls) {
    const dstd::Vector<std::int64_t> v = column(distinct);
    dstd::Vector<std::uint8_t> valid;
    if (with_nulls) {
        valid.assign(static_cast<std::size_t>((N + 7) / 8), 0xFF);
        for (std::int64_t i = 5; i < N; i += 13)
            valid[static_cast<std::size_t>(i >> 3)] &=
                static_cast<std::uint8_t>(~(1u << (i & 7)));
    }
    const Series s =
        Series::flat_i64(v.data(), N, with_nulls ? valid.data() : nullptr);
    const Reference want = walk(v, valid);
    for (const bool threads : {true, false}) {
        if (threads)
            install_runtime_parallel_backend();
        else
            set_parallel_backend(nullptr, nullptr);
        INFO("distinct " << distinct << " nulls " << with_nulls << " threads "
                         << threads);
        const DataFrame vc = dftu_utils_test::ok(s.value_counts());
        const Series values = vc.column("value").materialize();
        const Series counts = vc.column("count").materialize();
        REQUIRE(values.length() ==
                static_cast<std::int64_t>(want.distinct.size()));
        bool same = true;
        for (std::int64_t k = 0; same && k < values.length(); ++k) {
            const auto id = static_cast<std::size_t>(
                want.by_count[static_cast<std::size_t>(k)]);
            same = values.data<std::int64_t>()[k] == want.distinct[id] &&
                   counts.data<std::int64_t>()[k] == want.counts[id];
        }
        CHECK(same);
        const Series uniq = s.unique(true).drop_nulls().materialize();
        REQUIRE(uniq.length() ==
                static_cast<std::int64_t>(want.distinct.size()));
        same = true;
        for (std::int64_t k = 0; same && k < uniq.length(); ++k)
            same = uniq.data<std::int64_t>()[k] ==
                   want.distinct[static_cast<std::size_t>(k)];
        CHECK(same);
    }
    install_runtime_parallel_backend();
}

}  // namespace

TEST_CASE("a long column of repeated values is numbered in first-seen order") {
    check(7, false);
    check(5'000, false);
    check(5'000, true);
}

TEST_CASE(
    "a long column of mostly new values is numbered in first-seen order") {
    check(3'000'000, false);
    check(3'000'000, true);
    check(400'000, false);
}
