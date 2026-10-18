#ifndef DFTRACER_UTILS_MEMORY_BLOCK_H
#define DFTRACER_UTILS_MEMORY_BLOCK_H

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace dftracer::utils::memory {

inline constexpr std::size_t BLOCK_ALIGN = 64;
/// Bytes after the payload of an owned block that may be read and written,
/// so a SIMD loop can run past the last element.
inline constexpr std::size_t BLOCK_PADDING = 64;

class BlockRef;

/**
 * @brief A reference-counted buffer: memory from the C library (owned) or
 * foreign memory with a release function (borrowed).
 *
 * Reached only through BlockRef. An owned block is one C library allocation
 * that holds this header, the payload at the requested alignment and
 * BLOCK_PADDING bytes after it, charged to the scope pool of the allocating
 * thread by its whole size. A borrowed
 * block allocates only this header there; the foreign bytes are not charged.
 * The last reference frees the block or calls the release function on the
 * thread that drops it.
 */
class Block {
   public:
    enum class Kind : std::uint8_t { Owned, Borrowed };
    /// Runs once with the context of Block::borrow(). It must not throw.
    using Release = void (*)(void* context);

    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;

    /**
     * @brief An owned block of @p bytes whose payload is aligned to @p align.
     *
     * @p align must be a power of two. The payload and the padding are not
     * initialized. Throws std::bad_alloc when the memory is not available.
     */
    static BlockRef allocate(std::size_t bytes,
                             std::size_t align = BLOCK_ALIGN);

    /**
     * @brief A borrowed block over @p bytes at @p data.
     *
     * The memory must stay valid until @p release runs with @p context, which
     * is exactly once, when the last reference drops. @p release must not be
     * null. Throws std::bad_alloc when the header is not available, and then
     * @p release does not run.
     */
    static BlockRef borrow(void* data, std::size_t bytes, Release release,
                           void* context);

    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    Kind kind() const noexcept { return kind_; }

   private:
    friend class BlockRef;

    Block(std::uint8_t* data, std::size_t size, Release release_fn,
          void* context, Kind kind) noexcept
        : data_(data),
          size_(size),
          release_(release_fn),
          context_(context),
          kind_(kind) {}
    ~Block() = default;

    void retain() noexcept {
        [[maybe_unused]] std::uint32_t old =
            refs_.fetch_add(1, std::memory_order_relaxed);
        assert(old != std::numeric_limits<std::uint32_t>::max() &&
               "memory::Block reference count overflow");
    }

    // acq_rel: the thread that frees sees every write made through any
    // reference before it was dropped.
    void release() noexcept {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) destroy();
    }

    void destroy() noexcept;

    std::uint8_t* data_;
    std::size_t size_;
    Release release_;
    void* context_;
    std::atomic<std::uint32_t> refs_{1};
    Kind kind_;
    // The pool an owned block is charged to.
    std::uint16_t pool_ = 0;
};

/**
 * @brief An owning handle to one reference of a Block, or empty.
 *
 * Copy adds a reference, move transfers it, destruction drops it. Copies of
 * one block may be made and dropped on any threads at once; one BlockRef
 * object is not safe to change from two threads. More than 4294967295
 * references to one block is undefined; debug builds assert.
 */
class BlockRef {
   public:
    BlockRef() noexcept = default;
    BlockRef(const BlockRef& other) noexcept : block_(other.block_) {
        if (block_ != nullptr) block_->retain();
    }
    BlockRef(BlockRef&& other) noexcept
        : block_(std::exchange(other.block_, nullptr)) {}
    BlockRef& operator=(const BlockRef& other) noexcept {
        BlockRef(other).swap(*this);
        return *this;
    }
    BlockRef& operator=(BlockRef&& other) noexcept {
        BlockRef(std::move(other)).swap(*this);
        return *this;
    }
    ~BlockRef() {
        if (block_ != nullptr) block_->release();
    }

    void swap(BlockRef& other) noexcept { std::swap(block_, other.block_); }
    void reset() noexcept { BlockRef().swap(*this); }

    explicit operator bool() const noexcept { return block_ != nullptr; }
    /// The block, or null when empty.
    const Block* get() const noexcept { return block_; }

    /// The rest of these require a non-empty handle.
    const std::uint8_t* data() const noexcept { return block_->data_; }
    std::size_t size() const noexcept { return block_->size_; }
    Block::Kind kind() const noexcept { return block_->kind_; }

    /**
     * @brief The payload for writing, for the producer that fills a new block.
     *
     * The block must be owned and this must be its only reference (unique()),
     * so no reader can see the bytes change; debug builds assert both.
     */
    std::uint8_t* mutable_data() noexcept {
        assert(block_->kind_ == Block::Kind::Owned && unique());
        return block_->data_;
    }

    /// The count at some recent moment; exact only while no other thread
    /// copies or drops a reference.
    std::uint32_t use_count() const noexcept {
        return block_->refs_.load(std::memory_order_relaxed);
    }
    /// True when this is the only reference. Acquire, so writes made through
    /// references dropped before are visible.
    bool unique() const noexcept {
        return block_->refs_.load(std::memory_order_acquire) == 1;
    }

    friend bool operator==(const BlockRef& a, const BlockRef& b) noexcept {
        return a.block_ == b.block_;
    }
    friend bool operator!=(const BlockRef& a, const BlockRef& b) noexcept {
        return a.block_ != b.block_;
    }

   private:
    friend class Block;
    explicit BlockRef(Block* adopted) noexcept : block_(adopted) {}

    Block* block_ = nullptr;
};

static_assert(sizeof(BlockRef) == sizeof(void*));

/**
 * @brief A read-only byte range inside a Block that holds one reference to it.
 *
 * slice() copies the reference and allocates nothing. An empty view has a
 * null data() and no block.
 */
class BufferView {
   public:
    BufferView() noexcept = default;
    /// The whole payload of @p block; an empty @p block gives an empty view.
    explicit BufferView(BlockRef block) noexcept
        : data_(block ? block.data() : nullptr),
          size_(block ? block.size() : 0),
          block_(std::move(block)) {}
    /// Bytes [@p offset, @p offset + @p size) of a non-empty @p block, which
    /// must lie inside the payload; debug builds assert.
    BufferView(BlockRef block, std::size_t offset, std::size_t size) noexcept
        : data_(block.data() + offset), size_(size), block_(std::move(block)) {
        assert(offset <= block_.size() && size <= block_.size() - offset);
    }

    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    /// Bytes from the start of the payload; zero for an empty view.
    std::size_t offset() const noexcept {
        return block_ ? static_cast<std::size_t>(data_ - block_.data()) : 0;
    }
    const BlockRef& block() const noexcept { return block_; }

    /// Bytes [@p offset, @p offset + @p size) of this view, which must lie
    /// inside it; debug builds assert.
    BufferView slice(std::size_t offset, std::size_t size) const& noexcept {
        assert(offset <= size_ && size <= size_ - offset);
        return BufferView(Within{}, block_, data_ + offset, size);
    }
    BufferView slice(std::size_t offset, std::size_t size) && noexcept {
        assert(offset <= size_ && size <= size_ - offset);
        return BufferView(Within{}, std::move(block_), data_ + offset, size);
    }

   private:
    struct Within {};
    BufferView(Within, BlockRef block, const std::uint8_t* data,
               std::size_t size) noexcept
        : data_(data), size_(size), block_(std::move(block)) {}

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    BlockRef block_;
};

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_BLOCK_H
