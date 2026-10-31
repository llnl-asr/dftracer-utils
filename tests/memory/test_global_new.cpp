#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/debug.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

using namespace dftracer::utils::memory;

namespace {

constexpr std::size_t BIG_BYTES = std::size_t{3} << 20;
constexpr std::size_t OVER_ALIGN = 256;
constexpr std::size_t PLANTED_BYTES = 777777;
constexpr std::size_t PLANTED_SPAN = 512;
constexpr std::size_t EARLY_BYTES = std::size_t{64} << 20;
constexpr std::size_t LATE_BYTES = std::size_t{128} << 20;

struct alignas(OVER_ALIGN) Wide {
    char bytes[OVER_ALIGN];
};

// The debug mode holds freed blocks back, so drain them before a count.
std::size_t live() {
    debug_drain();
    return Manager::get().root().live();
}

// Runs @p make, checks the heap counted at least @p bytes more while the
// result lives and gave them back when it was destroyed.
template <class Make>
void check_served(std::size_t bytes, Make make) {
    const std::size_t base = live();
    const std::uint64_t count = debug_allocation_count();
    std::size_t held = 0;
    {
        auto owner = make();
        held = live();
        CHECK(debug_allocation_count() > count);
        CHECK(held >= base + bytes);
    }
    CHECK(live() + bytes <= held);
}

// C++ lets the compiler drop a new and delete pair whose block does not
// escape, and clang does at -O2; the empty asm makes the block escape.
template <class T>
T* escape(T* p) {
    asm volatile("" : "+r"(p) : : "memory");
    return p;
}

// The empty asm keeps the call to new from becoming a tail call, which would
// make its caller the caller of plant.
__attribute__((noinline)) char* plant() {
    char* p = new char[PLANTED_BYTES];
    asm volatile("" : "+r"(p));
    return p;
}

struct Found {
    std::uintptr_t site = 0;
    std::size_t live = 0;
    std::size_t peak = 0;
};

void find_planted(void* context, const DebugSiteTotal& total) noexcept {
    if (total.peak == PLANTED_BYTES)
        *static_cast<Found*>(context) = {total.site, total.live, total.peak};
}

// Own functions, so each has its own site; the empty asm keeps the call to new
// from becoming a tail call.
__attribute__((noinline)) char* plant_early() {
    char* p = new char[EARLY_BYTES];
    asm volatile("" : "+r"(p));
    return p;
}

__attribute__((noinline)) char* plant_late() {
    char* p = new char[LATE_BYTES];
    asm volatile("" : "+r"(p));
    return p;
}

struct AtPeak {
    std::size_t early = 0;
    std::size_t late = 0;
};

// True when @p site is in @p fn, the closer of the two planters below it.
bool inside(std::uintptr_t site, char* (*fn)()) {
    auto start = reinterpret_cast<std::uintptr_t>(fn);
    auto early = reinterpret_cast<std::uintptr_t>(&plant_early);
    auto late = reinterpret_cast<std::uintptr_t>(&plant_late);
    std::uintptr_t other = start == early ? late : early;
    bool other_between = other > start && other < site;
    return site > start && site < start + PLANTED_SPAN && !other_between;
}

void find_at_peak(void* context, const DebugSiteTotal& total) noexcept {
    auto& found = *static_cast<AtPeak*>(context);
    if (inside(total.site, plant_early)) found.early = total.live;
    if (inside(total.site, plant_late)) found.late = total.live;
}

}  // namespace

TEST_SUITE("GlobalNew") {
    TEST_CASE("a std::vector is served and counted by the heap") {
        check_served(1000 * sizeof(int),
                     [] { return std::make_unique<std::vector<int>>(1000); });
    }

    TEST_CASE("a std::string past the small string size is counted") {
        check_served(200, [] {
            return std::make_unique<std::string>(std::string(200, 'x'));
        });
    }

    TEST_CASE("a block above the page heap size classes is served") {
        const std::size_t base = live();
        const std::uint64_t count = debug_allocation_count();
        char* big = escape(new char[BIG_BYTES]);
        for (std::size_t i = 0; i < BIG_BYTES; i += 4096) big[i] = 1;
        big[BIG_BYTES - 1] = 1;
        const std::size_t held = live();
        CHECK(debug_allocation_count() > count);
        CHECK(held >= base + BIG_BYTES);
        delete[] big;
        CHECK(live() + BIG_BYTES <= held);
    }

    TEST_CASE("an over-aligned new keeps its alignment and is counted") {
        const std::uint64_t count = debug_allocation_count();
        Wide* w = escape(new Wide);
        CHECK(reinterpret_cast<std::uintptr_t>(w) % OVER_ALIGN == 0);
        CHECK(debug_allocation_count() > count);
        delete w;
        Wide* many = escape(new Wide[4]);
        CHECK(reinterpret_cast<std::uintptr_t>(many) % OVER_ALIGN == 0);
        delete[] many;
    }

    TEST_CASE("a nothrow new is served and counted") {
        const std::size_t base = live();
        const std::uint64_t count = debug_allocation_count();
        char* p = escape(new (std::nothrow) char[5000]);
        REQUIRE(p != nullptr);
        CHECK(debug_allocation_count() > count);
        CHECK(live() >= base + 5000);
        const std::size_t held = live();
        delete[] p;
        CHECK(live() + 5000 <= held);
    }

    TEST_CASE("a nothrow new above the limit returns null") {
        // Volatile, so the size is not a constant the compiler rejects.
        volatile std::size_t huge = std::size_t{1} << 62;
        char* p = new (std::nothrow) char[huge];
        CHECK(p == nullptr);
    }

    TEST_CASE("global new bytes are not counted again as external bytes") {
        const std::size_t external = Manager::get().stats().external;
        const std::size_t base = live();
        auto block = std::make_unique<std::vector<char>>(BIG_BYTES);
        CHECK(live() >= base + BIG_BYTES);
        CHECK(Manager::get().stats().external == external);
    }

    TEST_CASE("the debug mode names the code that called new") {
        char* block = plant();
        const auto start = reinterpret_cast<std::uintptr_t>(&plant);
        Found found;
        debug_top_sites(64, false, find_planted, &found);
        CHECK(found.live == PLANTED_BYTES);
        CHECK(found.site > start);
        CHECK(found.site < start + PLANTED_SPAN);
        delete[] block;
        live();
        found = {};
        debug_top_sites(64, true, find_planted, &found);
        CHECK(found.peak == PLANTED_BYTES);
        CHECK(found.live == 0);
    }

    TEST_CASE("the peak table holds the sites live at the peak") {
        char* early = plant_early();
        delete[] early;
        char* late = plant_late();
        AtPeak found;
        DebugPeak peak = debug_top_sites_at_peak(64, find_at_peak, &found);
        CHECK(found.late == LATE_BYTES);
        CHECK(found.early == 0);
        CHECK(peak.total >= LATE_BYTES);
        CHECK(peak.big >= LATE_BYTES);
        delete[] late;
        found = {};
        debug_top_sites_at_peak(64, find_at_peak, &found);
        CHECK(found.late == LATE_BYTES);
    }
}
