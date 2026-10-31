#include <dftracer/utils/memory/attrib.h>
#include <dftracer/utils/memory/block.h>
#include <dftracer/utils/memory/heap.h>

#include <cassert>
#include <cstdlib>
#include <limits>
#include <new>

namespace dftracer::utils::memory {

static_assert(sizeof(Block) == 40);

namespace {

std::size_t header_span(std::size_t align) noexcept {
    return (sizeof(Block) + align - 1) & ~(align - 1);
}

// aligned_alloc wants a multiple of the alignment, which is the header span.
std::size_t system_bytes(std::size_t head, std::size_t bytes) noexcept {
    return (head + bytes + BLOCK_PADDING + head - 1) / head * head;
}

}  // namespace

BlockRef Block::allocate(std::size_t bytes, std::size_t align) {
    if (align < BLOCK_ALIGN) align = BLOCK_ALIGN;
    std::size_t head = header_span(align);
    if (bytes >
        std::numeric_limits<std::size_t>::max() - 2 * head - BLOCK_PADDING) {
        throw std::bad_alloc();
    }
    std::size_t total = system_bytes(head, bytes);
    auto* base = static_cast<std::uint8_t*>(std::aligned_alloc(head, total));
    if (base == nullptr) throw std::bad_alloc();
    AttribCache& cache = attrib_local();
    attrib_charge_any(cache, total);
    auto* b =
        ::new (base) Block(base + head, bytes, nullptr, nullptr, Kind::Owned);
    b->pool_ = cache.scope;
    return BlockRef(b);
}

BlockRef Block::borrow(void* data, std::size_t bytes, Release release,
                       void* context) {
    assert(release != nullptr);
    void* p = memory::allocate(sizeof(Block), alignof(Block));
    return BlockRef(::new (p) Block(static_cast<std::uint8_t*>(data), bytes,
                                    release, context, Kind::Borrowed));
}

void Block::destroy() noexcept {
    if (kind_ == Kind::Owned) {
        auto* base = reinterpret_cast<std::uint8_t*>(this);
        auto head = static_cast<std::size_t>(data_ - base);
        std::uint16_t pool = pool_;
        std::size_t total = system_bytes(head, size_);
        this->~Block();
        std::free(base);
        attrib_credit_any(attrib_local(), pool, total);
        return;
    }
    Release release_fn = release_;
    void* context = context_;
    this->~Block();
    memory::deallocate(this, sizeof(Block));
    release_fn(context);
}

}  // namespace dftracer::utils::memory
