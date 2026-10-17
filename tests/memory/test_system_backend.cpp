#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/heap_system.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

using namespace dftracer::utils::memory;

namespace {

constexpr std::size_t MIB = 1024 * 1024;

const std::vector<std::size_t> SIZES = {
    0,  1,    7,    15,   16,      17,          63,      64,
    65, 4095, 4096, 4097, 1 * MIB, 1 * MIB + 1, 64 * MIB};

std::vector<std::size_t> alignments() {
    std::vector<std::size_t> out;
    for (std::size_t a = 1; a <= 4096; a *= 2) out.push_back(a);
    return out;
}

}  // namespace

TEST_CASE("the test runs on the system backend") {
    CHECK(config().backend == Backend::System);
}

TEST_CASE("every alignment from 1 to 4096 and size from 0 to 64 MiB") {
    for (std::size_t align : alignments()) {
        for (std::size_t size : SIZES) {
            CAPTURE(align);
            CAPTURE(size);
            void* p = allocate(size, align);
            REQUIRE(p != nullptr);
            CHECK(reinterpret_cast<std::uintptr_t>(p) % align == 0);
            if (size > 0) {
                std::memset(p, 0xab, size);
                CHECK(static_cast<unsigned char*>(p)[0] == 0xab);
                CHECK(static_cast<unsigned char*>(p)[size - 1] == 0xab);
            }
            if (size % 2 == 0) {
                deallocate(p, size);
            } else {
                deallocate(p);
            }
        }
    }
}

TEST_CASE("the header holds the size, pool id 0 and the offset") {
    for (std::size_t align : alignments()) {
        for (std::size_t size : SIZES) {
            CAPTURE(align);
            CAPTURE(size);
            void* p = allocate(size, align);
            const SystemHeader* h = system_header(p);
            CHECK(h->size == size);
            CHECK(h->pool_id == 0);
            std::size_t offset =
                align > SYSTEM_HEADER_SIZE ? align : SYSTEM_HEADER_SIZE;
            CHECK(h->offset == offset);
            auto base = reinterpret_cast<std::uintptr_t>(p) - h->offset;
            CHECK(base % offset == 0);
            if (size > 0) std::memset(p, 0, size);
            CHECK(system_header(p)->size == size);
            deallocate(p, size);
        }
    }
}

TEST_CASE("zero-size blocks are distinct") {
    std::set<void*> seen;
    std::vector<void*> blocks;
    for (int i = 0; i < 64; ++i) {
        void* p = allocate(0, 16);
        CHECK(seen.insert(p).second);
        blocks.push_back(p);
    }
    for (void* p : blocks) deallocate(p, 0);
}

TEST_CASE("null frees are no-ops") {
    deallocate(nullptr);
    deallocate(nullptr, 0);
}

// Run only by the ctest memory_asan_overrun, which expects ASan to report it.
TEST_CASE("planted overrun" * doctest::skip()) {
    REQUIRE(config().backend == Backend::System);
    constexpr std::size_t size = 24;
    auto* p = static_cast<volatile char*>(allocate(size, 16));
    p[size] = 1;
    deallocate(const_cast<char*>(p), size);
}
