// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/hash/partition.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <doctest/doctest.h>

#include <cstddef>

using namespace dftracer::utils;

TEST_CASE("a key always lands in the same partition at one depth") {
    for (int i = 0; i < 100; ++i) {
        const dstd::String key = "key" + dstd::to_string(i);
        CHECK(dstd::hash::partition_of(key, 16, 0) ==
              dstd::hash::partition_of(key, 16, 0));
        CHECK(dstd::hash::partition_of(key, 16, 2) < 16);
    }
}

TEST_CASE("every partition gets keys, and a deeper level spreads a partition") {
    dstd::Set<std::size_t> seen;
    dstd::Set<std::size_t> deeper;
    for (int i = 0; i < 4000; ++i) {
        const dstd::String key = "key" + dstd::to_string(i);
        seen.insert(dstd::hash::partition_of(key, 16, 0));
        if (dstd::hash::partition_of(key, 16, 0) == 3)
            deeper.insert(dstd::hash::partition_of(key, 16, 1));
    }
    CHECK(seen.size() == 16);
    // The keys of one partition are not all in one partition at the next
    // level, or a recursive split of an oversize partition would not progress.
    CHECK(deeper.size() > 8);
}

TEST_CASE("a partition count that is not a power of two is covered") {
    dstd::Set<std::size_t> seen;
    for (int i = 0; i < 4000; ++i)
        seen.insert(dstd::hash::partition_of("k" + dstd::to_string(i), 7, 0));
    CHECK(seen.size() == 7);
}
