// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/bits.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/plugins/prims.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>

using namespace dftracer::utils;

namespace {

int count_by_bit(std::uint64_t x) {
    int count = 0;
    for (int i = 0; i < 64; ++i) count += static_cast<int>((x >> i) & 1u);
    return count;
}

std::uint64_t next_value(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

}  // namespace

TEST_CASE("popcount_u64 counts every bit") {
    CHECK(dstd::bits::popcount_u64(0) == 0);
    CHECK(dstd::bits::popcount_u64(~std::uint64_t{0}) == 64);
    CHECK(dftu_popcount_u64(~std::uint64_t{0}) == 64);
    for (int i = 0; i < 64; ++i) {
        const std::uint64_t one = std::uint64_t{1} << i;
        CHECK(dstd::bits::popcount_u64(one) == 1);
        CHECK(dstd::bits::popcount_u64(~one) == 63);
    }
    std::uint64_t state = dstd::hash::GOLDEN_RATIO;
    for (int i = 0; i < 100000; ++i) {
        const std::uint64_t x = next_value(state);
        const int expected = count_by_bit(x);
        REQUIRE(dstd::bits::popcount_u64(x) == expected);
        REQUIRE(dftu_popcount_u64(x) == expected);
    }
}

TEST_CASE("popcount_bytes counts whole words and the tail") {
    std::uint8_t data[37];
    std::uint64_t state = 0x2545F4914F6CDD1DULL;
    for (auto& b : data) b = static_cast<std::uint8_t>(next_value(state));
    for (std::size_t n = 0; n <= sizeof(data); ++n) {
        for (std::size_t off = 0; off + n <= sizeof(data) && off < 8; ++off) {
            std::size_t expected = 0;
            for (std::size_t i = 0; i < n; ++i)
                expected +=
                    static_cast<std::size_t>(count_by_bit(data[off + i]));
            REQUIRE(dstd::bits::popcount_bytes(data + off, n) == expected);
        }
    }
}

// prims.h keeps its own copy of the SplitMix64 numbers, so plugins need no
// dstd header; the two must agree.
TEST_CASE("dftu_mix64 of the plugin ABI is dstd splitmix64") {
    CHECK(dftu_mix64(0) == dstd::hash::splitmix64(0));
    CHECK(dftu_mix64(~std::uint64_t{0}) ==
          dstd::hash::splitmix64(~std::uint64_t{0}));
    std::uint64_t state = dstd::hash::GOLDEN_RATIO;
    for (int i = 0; i < 10000; ++i) {
        const std::uint64_t x = next_value(state);
        REQUIRE(dftu_mix64(x) == dstd::hash::splitmix64(x));
    }
}
