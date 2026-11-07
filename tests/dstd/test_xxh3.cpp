// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/hash/xxh3.h>
#include <dftracer/utils/dstd/string.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;

using dstd::hash::xxh3_64;

TEST_SUITE("xxh3") {
    TEST_CASE("xxh3_64 - frozen values of the XXH3 specification") {
        CHECK(xxh3_64(dstd::StringView{""}) == 0x2D06800538D394C2ull);
        CHECK(xxh3_64(dstd::StringView{"a"}) == 0xE6C632B61E964E1Full);
        CHECK(xxh3_64(dstd::StringView{"dftracer"}) == 0xF58937704FB30767ull);
    }

    TEST_CASE("xxh3_64 - 1 KiB input") {
        dstd::String bytes(1024, '\0');
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<char>((i * 31 + 7) & 0xFF);
        }
        CHECK(xxh3_64(bytes.data(), bytes.size()) == 0x23BC880EBF0D29C6ull);
    }

    TEST_CASE("xxh3_64 - pointer and view overloads agree") {
        const dstd::StringView text{"PyTorch.forward"};
        CHECK(xxh3_64(text.data(), text.size()) == xxh3_64(text));
    }
}
