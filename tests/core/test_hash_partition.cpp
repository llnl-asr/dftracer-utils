#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/hash/partition.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <set>
#include <string>

using dftracer::utils::hash::partition_of;

TEST_CASE("a key always lands in the same partition at one depth") {
    for (int i = 0; i < 100; ++i) {
        const std::string key = "key" + std::to_string(i);
        CHECK(partition_of(key, 16, 0) == partition_of(key, 16, 0));
        CHECK(partition_of(key, 16, 2) < 16);
    }
}

TEST_CASE("every partition gets keys, and a deeper level spreads a partition") {
    std::set<std::size_t> seen;
    std::set<std::size_t> deeper;
    for (int i = 0; i < 4000; ++i) {
        const std::string key = "key" + std::to_string(i);
        seen.insert(partition_of(key, 16, 0));
        if (partition_of(key, 16, 0) == 3)
            deeper.insert(partition_of(key, 16, 1));
    }
    CHECK(seen.size() == 16);
    // The keys of one partition are not all in one partition at the next
    // level, or a recursive split of an oversize partition would not progress.
    CHECK(deeper.size() > 8);
}

TEST_CASE("a partition count that is not a power of two is covered") {
    std::set<std::size_t> seen;
    for (int i = 0; i < 4000; ++i)
        seen.insert(partition_of("k" + std::to_string(i), 7, 0));
    CHECK(seen.size() == 7);
}
