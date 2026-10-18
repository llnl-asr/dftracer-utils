#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_BUFFERS_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_BUFFERS_H

#include <dftracer/utils/memory/block.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace dftracer::utils::dataframe {

using memory::BufferView;

/// A new owned block of `bytes` on the memory heap, viewed whole. The bytes
/// are not initialized. Throws std::bad_alloc.
inline BufferView new_buffer(std::size_t bytes,
                             std::size_t align = memory::BLOCK_ALIGN) {
    return BufferView(memory::Block::allocate(bytes, align));
}

/// The bytes of `view` for writing while the column that holds it is being
/// built. The view must be the only reference to an owned block, so no
/// reader sees the bytes change; debug builds assert.
inline std::uint8_t* writable(BufferView& view) noexcept {
    assert(view.block() && view.block().kind() == memory::Block::Kind::Owned &&
           view.block().unique());
    return const_cast<std::uint8_t*>(view.data());
}

/// `bytes` foreign bytes at `data`, kept alive by a copy of `owner` that is
/// dropped when the last view of the block goes, on that thread. Throws
/// std::bad_alloc, and then the copy is dropped at once.
inline BufferView borrow_buffer(const void* data, std::size_t bytes,
                                std::shared_ptr<const void> owner) {
    auto* held = new std::shared_ptr<const void>(std::move(owner));
    try {
        return BufferView(memory::Block::borrow(
            const_cast<void*>(data), bytes,
            [](void* context) {
                delete static_cast<std::shared_ptr<const void>*>(context);
            },
            held));
    } catch (...) {
        delete held;
        throw;
    }
}

/// A scratch array of trivially copyable T in one owned block: no value
/// initialization, the same allocation path as a column. Move only.
template <class T>
class Scratch {
   public:
    Scratch() = default;
    explicit Scratch(std::size_t n) { resize(n); }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    Scratch(Scratch&&) noexcept = default;
    Scratch& operator=(Scratch&&) noexcept = default;

    void resize(std::size_t n) {
        buf_ = memory::Block::allocate(n * sizeof(T));
        n_ = n;
    }
    void reset() {
        buf_.reset();
        n_ = 0;
    }
    T* data() {
        return buf_ ? reinterpret_cast<T*>(buf_.mutable_data()) : nullptr;
    }
    const T* data() const {
        return buf_ ? reinterpret_cast<const T*>(buf_.data()) : nullptr;
    }
    std::size_t size() const { return n_; }
    T& operator[](std::size_t i) { return data()[i]; }
    const T& operator[](std::size_t i) const { return data()[i]; }
    void swap(Scratch& o) noexcept {
        buf_.swap(o.buf_);
        std::swap(n_, o.n_);
    }

   private:
    memory::BlockRef buf_;
    std::size_t n_ = 0;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_BUFFERS_H
