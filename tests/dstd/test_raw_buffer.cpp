#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/raw_buffer.h>
#include <dftracer/utils/memory/manager.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

using namespace dftracer::utils;

namespace {

memory::Pool& root() { return memory::Manager::get().root(); }

bool aligned(const void* p) {
    return reinterpret_cast<std::uintptr_t>(p) % 64 == 0;
}

}  // namespace

static_assert(sizeof(dstd::RawBuffer<char>) == 3 * sizeof(void*));
static_assert(!std::is_copy_constructible_v<dstd::RawBuffer<char>>);
static_assert(std::is_nothrow_move_constructible_v<dstd::RawBuffer<char>>);

TEST_CASE("growth doubles from 64 bytes and keeps the contents") {
    dstd::RawBuffer<std::uint32_t> buf;
    CHECK(buf.data() == nullptr);
    buf.push_back(0);
    CHECK(buf.capacity() == 16);
    std::size_t capacity = buf.capacity();
    for (std::uint32_t i = 1; i < 100000; ++i) {
        buf.push_back(i);
        if (buf.capacity() != capacity) {
            CHECK(buf.capacity() == 2 * capacity);
            capacity = buf.capacity();
            CHECK(aligned(buf.data()));
        }
    }
    CHECK(buf.size() == 100000);
    bool intact = true;
    for (std::uint32_t i = 0; i < 100000; ++i) intact &= buf[i] == i;
    CHECK(intact);
    buf.resize(10 * capacity);
    CHECK(buf.capacity() == 10 * capacity);
    intact = true;
    for (std::uint32_t i = 0; i < 100000; ++i) intact &= buf[i] == i;
    CHECK(intact);
}

TEST_CASE("storage is 64-byte aligned at every size") {
    for (std::size_t n : {1, 7, 64, 1000, 40000, 1 << 21}) {
        CAPTURE(n);
        dstd::RawBuffer<char> buf(n);
        CHECK(buf.size() == n);
        CHECK(aligned(buf.data()));
        buf.reserve(n * 3);
        CHECK(buf.capacity() >= n * 3);
        CHECK(aligned(buf.data()));
    }
}

TEST_CASE("resize leaves new elements unwritten") {
    dstd::RawBuffer<unsigned char> buf;
    buf.reserve(256);
    buf.resize(256);
    for (std::size_t i = 0; i < 256; ++i) {
        buf[i] = static_cast<unsigned char>(i);
    }
    buf.resize(10);
    CHECK(buf.size() == 10);
    buf.resize(256);
    bool kept = true;
    for (std::size_t i = 0; i < 256; ++i) {
        kept &= buf[i] == static_cast<unsigned char>(i);
    }
    CHECK(kept);
    CHECK(buf.capacity() == 256);
}

TEST_CASE("allocations are charged to the scope pool and freed") {
    memory::PoolHandle pool = root().child("raw");
    memory::Scope s{*pool};
    {
        dstd::RawBuffer<char> buf;
        CHECK(pool->live() == 0);
        buf.resize(5000);
        CHECK(pool->blocks() == 1);
        CHECK(pool->live() >= 5000);
        buf.clear();
        CHECK(buf.empty());
        CHECK(buf.capacity() >= 5000);
        CHECK(pool->blocks() == 1);
        buf.release();
        CHECK(buf.capacity() == 0);
        CHECK(pool->live() == 0);
        buf.push_back('x');
        CHECK(pool->blocks() == 1);
    }
    CHECK(pool->live() == 0);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("move leaves the source empty and frees once") {
    memory::PoolHandle pool = root().child("moved");
    memory::Scope s{*pool};
    {
        dstd::RawBuffer<int> a(100);
        a[99] = 7;
        const int* data = a.data();
        dstd::RawBuffer<int> b = std::move(a);
        CHECK(a.data() == nullptr);
        CHECK(a.size() == 0);
        CHECK(a.capacity() == 0);
        CHECK(b.data() == data);
        CHECK(b[99] == 7);
        dstd::RawBuffer<int> c(10);
        c = std::move(b);
        CHECK(c.data() == data);
        CHECK(pool->blocks() == 1);
    }
    CHECK(pool->live() == 0);
    CHECK(pool->blocks() == 0);
}

namespace {

struct Three {
    char bytes[3];
};

struct TwentyFour {
    std::uint64_t words[3];
};

}  // namespace

TEST_CASE("the first growth covers at least 64 bytes") {
    dstd::RawBuffer<Three> three;
    three.push_back({});
    CHECK(three.capacity() * sizeof(Three) >= 64);
    CHECK(three.capacity() == 22);
    dstd::RawBuffer<TwentyFour> wide;
    wide.push_back({});
    CHECK(wide.capacity() == 3);
}

TEST_CASE("a failed growth throws and keeps the buffer") {
    dstd::RawBuffer<std::uint64_t> buf(10);
    buf[9] = 99;
    const std::uint64_t* data = buf.data();
    CHECK_THROWS_AS(buf.reserve(SIZE_MAX / 4), std::bad_alloc);
    CHECK(buf.data() == data);
    CHECK(buf.size() == 10);
    CHECK(buf[9] == 99);
}
