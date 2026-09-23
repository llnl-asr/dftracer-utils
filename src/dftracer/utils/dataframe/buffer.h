#ifndef DFTRACER_UTILS_DATAFRAME_BUFFER_H
#define DFTRACER_UTILS_DATAFRAME_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <utility>

namespace dftracer::utils::dataframe {

/// Aligned, reference-counted byte buffer with a polymorphic owner. Owns memory
/// it allocates (freed on destruction) or wraps foreign memory (a release
/// callback runs instead), so the same type carries engine-allocated buffers
/// and zero-copy-imported Arrow buffers. Default alignment (64) suits SIMD and
/// Arrow.
class Buffer {
   public:
    static constexpr std::size_t DEFAULT_ALIGN = 64;

    // The allocator, not mmap: a mapped buffer leaves the resident set on
    // release but pays a page fault and a zero fill per fresh page on every
    // touch, which made a 10M-row sort 40% slower; the allocator hands back
    // warm pages.
    static std::shared_ptr<Buffer> allocate(std::size_t bytes,
                                            std::size_t align = DEFAULT_ALIGN) {
        void* p = nullptr;
        if (bytes != 0) {
            std::size_t rounded = (bytes + align - 1) & ~(align - 1);
            p = std::aligned_alloc(align, rounded);
            if (p == nullptr) throw std::bad_alloc();
        }
        return std::shared_ptr<Buffer>(
            new Buffer(static_cast<std::uint8_t*>(p), bytes,
                       [](void* q) { std::free(q); }));
    }

    /// Wrap foreign memory; `release` runs on destruction instead of free.
    static std::shared_ptr<Buffer> wrap(std::uint8_t* data, std::size_t bytes,
                                        std::function<void(void*)> release) {
        return std::shared_ptr<Buffer>(
            new Buffer(data, bytes, std::move(release)));
    }

    ~Buffer() {
        if (release_ && data_ != nullptr) release_(data_);
    }

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    std::uint8_t* data() noexcept { return data_; }
    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

   private:
    Buffer(std::uint8_t* data, std::size_t size,
           std::function<void(void*)> release)
        : data_(data), size_(size), release_(std::move(release)) {}

    std::uint8_t* data_;
    std::size_t size_;
    std::function<void(void*)> release_;
};

/// A scratch array of trivially copyable T over a Buffer: no value
/// initialization, the same allocation path as a column.
template <class T>
class Scratch {
   public:
    Scratch() = default;
    explicit Scratch(std::size_t n) { resize(n); }
    void resize(std::size_t n) {
        buf_ = Buffer::allocate(n * sizeof(T));
        n_ = n;
    }
    void reset() {
        buf_.reset();
        n_ = 0;
    }
    T* data() { return reinterpret_cast<T*>(buf_ ? buf_->data() : nullptr); }
    const T* data() const {
        return reinterpret_cast<const T*>(buf_ ? buf_->data() : nullptr);
    }
    std::size_t size() const { return n_; }
    T& operator[](std::size_t i) { return data()[i]; }
    const T& operator[](std::size_t i) const { return data()[i]; }
    void swap(Scratch& o) noexcept {
        buf_.swap(o.buf_);
        std::swap(n_, o.n_);
    }

   private:
    std::shared_ptr<Buffer> buf_;
    std::size_t n_ = 0;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_BUFFER_H
