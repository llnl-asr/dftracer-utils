#ifndef DFTRACER_UTILS_CORE_COMMON_BYTE_VIEW_H
#define DFTRACER_UTILS_CORE_COMMON_BYTE_VIEW_H

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils {

/**
 * @brief Non-owning view over a contiguous byte range.
 *
 * 16 bytes (pointer + size). All methods are trivial reinterpret_casts
 * inlined by the compiler -- zero overhead over raw pointer + length.
 */
class ByteView {
   public:
    ByteView() = default;

    ByteView(const std::byte* data, std::size_t len)
        : data_(data), size_(len) {}

    ByteView(const unsigned char* data, std::size_t len)
        : data_(reinterpret_cast<const std::byte*>(data)), size_(len) {}

    ByteView(const char* data, std::size_t len)
        : data_(reinterpret_cast<const std::byte*>(data)), size_(len) {}

    ByteView(std::string_view sv) : ByteView(sv.data(), sv.size()) {}

    ByteView(const std::vector<unsigned char>& v)
        : ByteView(v.data(), v.size()) {}

    ByteView(const std::vector<std::byte>& v) : ByteView(v.data(), v.size()) {}

    const std::byte* data() const { return data_; }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    template <typename T>
    const T* as() const {
        return reinterpret_cast<const T*>(data_);
    }

    std::string_view as_string_view() const { return {as<char>(), size_}; }

    ByteView subspan(std::size_t offset, std::size_t count) const {
        return {data_ + offset, count};
    }

    ByteView subspan(std::size_t offset) const {
        return {data_ + offset, size_ - offset};
    }

   private:
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

/**
 * @brief Non-owning mutable view over a contiguous byte range.
 *
 * For buffers that need to be written into (e.g., inflate output buffers).
 * Implicitly converts to ByteView for reading.
 */
class MutableByteView {
   public:
    MutableByteView() = default;

    MutableByteView(std::byte* data, std::size_t len)
        : data_(data), size_(len) {}

    MutableByteView(unsigned char* data, std::size_t len)
        : data_(reinterpret_cast<std::byte*>(data)), size_(len) {}

    MutableByteView(char* data, std::size_t len)
        : data_(reinterpret_cast<std::byte*>(data)), size_(len) {}

    MutableByteView(std::vector<unsigned char>& v)
        : MutableByteView(v.data(), v.size()) {}

    MutableByteView(std::string& s) : MutableByteView(s.data(), s.size()) {}

    std::byte* data() const { return data_; }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    template <typename T>
    T* as() const {
        return reinterpret_cast<T*>(data_);
    }

    operator ByteView() const { return {data_, size_}; }

   private:
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_BYTE_VIEW_H
