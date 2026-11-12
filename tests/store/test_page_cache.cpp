// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/page_cache.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

using namespace dftracer::utils;

namespace {

constexpr std::size_t PAGE = 32 * 1024;

dstd::SharedPtr<dstd::RawBuffer<std::uint8_t>> page_of(std::uint8_t fill) {
    auto page = dstd::make_shared<dstd::RawBuffer<std::uint8_t>>(PAGE);
    std::fill(page->data(), page->data() + page->size(), fill);
    return page;
}

}  // namespace

TEST_SUITE("index store page cache") {
    TEST_CASE("a page is found by its file and its offset") {
        store::engine::PageCache cache(16 * 64 * PAGE);
        CHECK(cache.find(1, 0) == nullptr);
        const store::engine::CachedPage kept = cache.insert(1, 0, page_of(7));
        REQUIRE(kept != nullptr);
        CHECK(cache.find(1, 0) == kept);
        CHECK(cache.find(2, 0) == nullptr);
        CHECK(cache.find(1, PAGE) == nullptr);
        CHECK(cache.bytes() == PAGE);
    }

    TEST_CASE("a second insert of one place returns the page kept first") {
        store::engine::PageCache cache(16 * 64 * PAGE);
        const store::engine::CachedPage first = cache.insert(1, 0, page_of(1));
        const store::engine::CachedPage second = cache.insert(1, 0, page_of(2));
        CHECK(second == first);
        CHECK((*second)[0] == 1);
        CHECK(cache.bytes() == PAGE);
    }

    TEST_CASE("the cache stays within its capacity") {
        const std::size_t capacity = 16 * 4 * PAGE;
        store::engine::PageCache cache(capacity);
        for (std::uint64_t i = 0; i < 4096; ++i) {
            cache.insert(i % 3, i * PAGE, page_of(1));
            CHECK(cache.bytes() <= capacity);
        }
        std::size_t found = 0;
        for (std::uint64_t i = 0; i < 4096; ++i) {
            if (cache.find(i % 3, i * PAGE) != nullptr) ++found;
        }
        CHECK(found > 0);
        CHECK(found <= 16 * 4);
    }

    TEST_CASE("a page the cache drops lives while a query holds it") {
        store::engine::PageCache cache(16 * PAGE);
        const store::engine::CachedPage held = cache.insert(1, 0, page_of(9));
        for (std::uint64_t i = 1; i < 1024; ++i) {
            cache.insert(1, i * PAGE, page_of(0));
        }
        CHECK(cache.find(1, 0) == nullptr);
        REQUIRE(held != nullptr);
        CHECK(held->size() == PAGE);
        CHECK((*held)[PAGE - 1] == 9);
    }

    TEST_CASE("a page found again is kept over one never found") {
        // Two pages a shard. Three places of one shard: a cache of one page a
        // shard keeps only one of two places when they share a shard.
        dstd::Vector<std::uint64_t> same{0};
        for (std::uint64_t i = 1; same.size() < 3; ++i) {
            store::engine::PageCache probe(16 * PAGE);
            probe.insert(1, 0, page_of(0));
            probe.insert(1, i * PAGE, page_of(0));
            if (probe.find(1, 0) == nullptr) same.push_back(i);
        }
        store::engine::PageCache cache(16 * 2 * PAGE);
        cache.insert(1, same[0] * PAGE, page_of(1));
        cache.insert(1, same[1] * PAGE, page_of(2));
        CHECK(cache.find(1, same[0] * PAGE) != nullptr);
        cache.insert(1, same[2] * PAGE, page_of(3));
        CHECK(cache.find(1, same[0] * PAGE) != nullptr);
        CHECK(cache.find(1, same[1] * PAGE) == nullptr);
        CHECK(cache.find(1, same[2] * PAGE) != nullptr);
    }

    TEST_CASE("threads that insert and find at once see whole pages") {
        store::engine::PageCache cache(16 * 8 * PAGE);
        std::atomic<std::size_t> wrong{0};
        dstd::Vector<dstd::Thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&cache, &wrong, t] {
                for (std::uint64_t i = 0; i < 2000; ++i) {
                    const std::uint64_t at = (i * 7 + t) % 64;
                    const auto fill = static_cast<std::uint8_t>(at);
                    store::engine::CachedPage page = cache.find(0, at * PAGE);
                    if (page == nullptr) {
                        page = cache.insert(0, at * PAGE, page_of(fill));
                    }
                    if ((*page)[0] != fill || (*page)[PAGE - 1] != fill) {
                        wrong.fetch_add(1);
                    }
                }
            });
        }
        for (dstd::Thread& thread : threads) thread.join();
        CHECK(wrong.load() == 0);
        CHECK(cache.bytes() <= 16 * 8 * PAGE);
    }
}
