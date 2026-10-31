#ifndef DFTRACER_UTILS_DSTD_RAW_BUFFER_H
#define DFTRACER_UTILS_DSTD_RAW_BUFFER_H

#include <dftracer/utils/memory/heap.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace dftracer::utils::dstd {

/**
 * @brief A growable buffer of trivially copyable T that leaves new elements
 * uninitialized, for buffers the caller fills.
 *
 * Storage is 64-byte aligned and comes from memory::allocate and
 * memory::reallocate. Growth doubles the capacity, starting at 64 bytes. A
 * moved-from RawBuffer is empty. Not copyable: a copy of a large buffer should
 * be written out where it happens.
 */
template <class T>
class RawBuffer {
    static_assert(std::is_trivially_copyable_v<T>,
                  "RawBuffer holds only trivially copyable types");

   public:
    static constexpr std::size_t ALIGN = 64;

    using value_type = T;
    using size_type = std::size_t;
    using iterator = T*;
    using const_iterator = const T*;

    RawBuffer() noexcept = default;
    /// @p n elements, uninitialized.
    explicit RawBuffer(std::size_t n) { resize(n); }

    RawBuffer(RawBuffer&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, 0)) {}

    RawBuffer& operator=(RawBuffer&& other) noexcept {
        if (this != &other) {
            release();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            capacity_ = std::exchange(other.capacity_, 0);
        }
        return *this;
    }

    RawBuffer(const RawBuffer&) = delete;
    RawBuffer& operator=(const RawBuffer&) = delete;

    ~RawBuffer() { release(); }

    T* data() noexcept { return data_; }
    const T* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }
    bool empty() const noexcept { return size_ == 0; }

    T& operator[](std::size_t i) noexcept { return data_[i]; }
    const T& operator[](std::size_t i) const noexcept { return data_[i]; }

    T* begin() noexcept { return data_; }
    T* end() noexcept { return data_ + size_; }
    const T* begin() const noexcept { return data_; }
    const T* end() const noexcept { return data_ + size_; }

    /// Make room for @p n elements without changing the size. Exact: no
    /// rounding up. Throws std::bad_alloc and keeps the buffer on failure.
    void reserve(std::size_t n) {
        if (n > capacity_) set_capacity(n);
    }

    /// Change the size to @p n. Elements past the old size are not written.
    void resize(std::size_t n) {
        if (n > capacity_) grow(n);
        size_ = n;
    }

    void push_back(const T& value) {
        if (size_ == capacity_) {
            T copy = value;
            grow(size_ + 1);
            data_[size_++] = copy;
            return;
        }
        data_[size_++] = value;
    }

    /// Size zero; the capacity stays.
    void clear() noexcept { size_ = 0; }

    /// Free the storage; size and capacity become zero.
    void release() noexcept {
        memory::deallocate(data_, capacity_ * sizeof(T));
        data_ = nullptr;
        size_ = 0;
        capacity_ = 0;
    }

   private:
    static constexpr std::size_t MIN_CAPACITY =
        (ALIGN + sizeof(T) - 1) / sizeof(T);

    void grow(std::size_t n) {
        std::size_t doubled =
            capacity_ > max_size() / 2 ? max_size() : capacity_ * 2;
        set_capacity(std::max({n, doubled, MIN_CAPACITY}));
    }

    static constexpr std::size_t max_size() noexcept {
        return std::numeric_limits<std::size_t>::max() / sizeof(T);
    }

    void set_capacity(std::size_t n) {
        if (n > max_size()) throw std::bad_alloc();
        data_ = static_cast<T*>(memory::reallocate(data_, capacity_ * sizeof(T),
                                                   n * sizeof(T), ALIGN));
        capacity_ = n;
    }

    T* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_RAW_BUFFER_H
