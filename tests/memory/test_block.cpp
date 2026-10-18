#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/block.h>
#include <dftracer/utils/memory/manager.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

using namespace dftracer::utils::memory;

namespace {

constexpr std::size_t MIB = std::size_t{1} << 20;

Pool& root() { return Manager::get().root(); }

bool aligned(const void* p, std::size_t align) {
    return reinterpret_cast<std::uintptr_t>(p) % align == 0;
}

struct ReleaseLog {
    int calls = 0;
    void* context = nullptr;
    std::thread::id thread;
};

ReleaseLog g_log;

void record_release(void* context) {
    ++g_log.calls;
    g_log.context = context;
    g_log.thread = std::this_thread::get_id();
}

}  // namespace

static_assert(sizeof(BlockRef) == 8);
static_assert(sizeof(BufferView) == 24);

TEST_CASE(
    "an owned payload is 64-byte aligned with 64 writable bytes after it") {
    PoolHandle pool = root().child("aligned");
    REQUIRE(pool);
    Scope s{*pool};
    for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{100},
                             std::size_t{4096}, std::size_t{3} * MIB}) {
        BlockRef block = Block::allocate(size);
        REQUIRE(block);
        CHECK(block.size() == size);
        CHECK(block.kind() == Block::Kind::Owned);
        CHECK(aligned(block.data(), BLOCK_ALIGN));
        std::uint8_t* bytes = block.mutable_data();
        std::memset(bytes, 0xAB, size);
        std::memset(bytes + size, 0xCD, BLOCK_PADDING);
        for (std::size_t i = 0; i < size; i += 997) CHECK(bytes[i] == 0xAB);
        CHECK(pool->live() >= size + BLOCK_PADDING);
        CHECK(pool->blocks() == 1);
    }
    CHECK(pool->live() == 0);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("a larger alignment is kept") {
    BlockRef block = Block::allocate(100, 4096);
    CHECK(aligned(block.data(), 4096));
}

TEST_CASE("copy adds a reference, move transfers it, the last drop frees") {
    PoolHandle pool = root().child("refs");
    REQUIRE(pool);
    Scope s{*pool};
    BlockRef a = Block::allocate(1000);
    CHECK(a.use_count() == 1);
    CHECK(a.unique());
    {
        BlockRef b = a;
        CHECK(a.use_count() == 2);
        CHECK(!a.unique());
        CHECK(b == a);
        BlockRef c = std::move(b);
        CHECK(!b);
        CHECK(a.use_count() == 2);
        BlockRef d;
        d = c;
        CHECK(a.use_count() == 3);
        d = std::move(c);
        CHECK(!c);
        CHECK(a.use_count() == 2);
        BlockRef& alias = d;
        d = alias;
        CHECK(a.use_count() == 2);
    }
    CHECK(a.use_count() == 1);
    CHECK(pool->blocks() == 1);
    a.reset();
    CHECK(!a);
    CHECK(pool->live() == 0);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("assigning over the last reference frees the old block") {
    PoolHandle pool = root().child("assign");
    REQUIRE(pool);
    Scope s{*pool};
    BlockRef a = Block::allocate(64);
    BlockRef b = Block::allocate(128);
    CHECK(pool->blocks() == 2);
    a = b;
    CHECK(pool->blocks() == 1);
    CHECK(a.size() == 128);
    CHECK(b.use_count() == 2);
}

TEST_CASE("a borrowed block calls its release once with its context") {
    g_log = {};
    std::vector<std::uint8_t> foreign(MIB, 7);
    int context = 0;
    PoolHandle pool = root().child("borrowed");
    REQUIRE(pool);
    {
        Scope s{*pool};
        BlockRef block = Block::borrow(foreign.data(), foreign.size(),
                                       record_release, &context);
        CHECK(block.kind() == Block::Kind::Borrowed);
        CHECK(block.data() == foreign.data());
        CHECK(block.size() == MIB);
        CHECK(pool->blocks() == 1);
        CHECK(pool->live() < 1024);
        BufferView view(block, 10, 100);
        CHECK(view.data() == foreign.data() + 10);
        BlockRef copy = block;
        block.reset();
        copy.reset();
        CHECK(g_log.calls == 0);
        CHECK(view.block().unique());
    }
    CHECK(g_log.calls == 1);
    CHECK(g_log.context == &context);
    CHECK(pool->live() == 0);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("slice shares the block and allocates nothing") {
    PoolHandle pool = root().child("slice");
    REQUIRE(pool);
    Scope s{*pool};
    BlockRef block = Block::allocate(1000);
    std::uint8_t* bytes = block.mutable_data();
    for (std::size_t i = 0; i < 1000; ++i)
        bytes[i] = static_cast<std::uint8_t>(i);
    BufferView whole(block);
    std::size_t live = pool->live();
    std::size_t blocks = pool->blocks();

    BufferView part = whole.slice(100, 500);
    BufferView inner = part.slice(50, 10);
    CHECK(pool->live() == live);
    CHECK(pool->blocks() == blocks);
    CHECK(block.use_count() == 4);
    CHECK(part.block() == block);
    CHECK(inner.offset() == 150);
    CHECK(inner.size() == 10);
    CHECK(inner.data() == block.data() + 150);
    CHECK(inner.data()[0] == 150);

    BufferView moved = std::move(part).slice(0, 1);
    CHECK(!part.block());
    CHECK(block.use_count() == 4);
    CHECK(moved.data() == block.data() + 100);

    block.reset();
    whole = BufferView();
    moved = BufferView();
    CHECK(pool->blocks() == 1);
    CHECK(inner.block().unique());
    CHECK(inner.data()[9] == 159);
    inner = BufferView();
    CHECK(pool->live() == 0);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("an empty view has no data and no block") {
    BufferView view;
    CHECK(view.data() == nullptr);
    CHECK(view.empty());
    CHECK(view.offset() == 0);
    CHECK(!view.block());
    BufferView from_empty{BlockRef()};
    CHECK(from_empty.data() == nullptr);
    CHECK(from_empty.size() == 0);
}

TEST_CASE("a block dropped last on another thread is freed") {
    PoolHandle pool = root().child("remote");
    REQUIRE(pool);
    for (std::size_t size :
         {std::size_t{48}, std::size_t{20000}, std::size_t{3} * MIB}) {
        BlockRef block;
        {
            Scope s{*pool};
            block = Block::allocate(size);
        }
        BlockRef copy = block;
        block.reset();
        std::thread([ref = std::move(copy)]() mutable { ref.reset(); }).join();
        CHECK(pool->live() == 0);
        CHECK(pool->blocks() == 0);
    }
}

TEST_CASE("a borrowed block released on another thread runs release there") {
    g_log = {};
    std::uint8_t foreign[256] = {};
    int context = 0;
    PoolHandle pool = root().child("remote_borrowed");
    REQUIRE(pool);
    BlockRef block;
    {
        Scope s{*pool};
        block =
            Block::borrow(foreign, sizeof(foreign), record_release, &context);
    }
    std::thread::id other;
    std::thread th([view = BufferView(std::move(block))]() mutable {
        view = BufferView();
    });
    other = th.get_id();
    th.join();
    CHECK(g_log.calls == 1);
    CHECK(g_log.context == &context);
    CHECK(g_log.thread == other);
    CHECK(pool->live() == 0);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("an owned block is charged its whole allocation") {
    const std::size_t header = BLOCK_ALIGN;
    for (std::size_t size :
         {std::size_t{0}, std::size_t{1}, std::size_t{256} << 10,
          (std::size_t{3} << 20) + 1}) {
        CAPTURE(size);
        // aligned_alloc takes a multiple of the alignment.
        const std::size_t expected =
            (header + size + BLOCK_PADDING + BLOCK_ALIGN - 1) / BLOCK_ALIGN *
            BLOCK_ALIGN;
        PoolHandle pool = root().child("charged");
        REQUIRE(pool);
        BlockRef block;
        {
            Scope s{*pool};
            block = Block::allocate(size);
        }
        CHECK(pool->live() == expected);
        CHECK(pool->blocks() == 1);
        CHECK(aligned(block.data(), BLOCK_ALIGN));
        std::uint8_t* bytes = block.mutable_data();
        std::memset(bytes, 0x5A, size + BLOCK_PADDING);
        CHECK(bytes[size + BLOCK_PADDING - 1] == 0x5A);
        BlockRef copy = block;
        block.reset();
        CHECK(pool->live() == expected);
        std::thread([ref = std::move(copy)]() mutable { ref.reset(); }).join();
        CHECK(pool->live() == 0);
        CHECK(pool->blocks() == 0);
    }
}
