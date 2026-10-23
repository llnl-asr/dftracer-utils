#ifndef DFTRACER_UTILS_DSTD_LITTLE_ENDIAN_H
#define DFTRACER_UTILS_DSTD_LITTLE_ENDIAN_H

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dftracer::utils {

// Little-endian fixed-width byte codec. The read_* and write_* functions do
// not check bounds: the caller owns the buffer. LeReader is the checked form.
// On a little-endian host each function compiles to one load or store.

inline std::uint16_t byte_swap(std::uint16_t v) {
    return static_cast<std::uint16_t>((v >> 8U) | (v << 8U));
}
inline std::uint32_t byte_swap(std::uint32_t v) {
    return ((v & 0x000000FFU) << 24U) | ((v & 0x0000FF00U) << 8U) |
           ((v & 0x00FF0000U) >> 8U) | ((v & 0xFF000000U) >> 24U);
}
inline std::uint64_t byte_swap(std::uint64_t v) {
    return (static_cast<std::uint64_t>(byte_swap(static_cast<std::uint32_t>(v)))
            << 32U) |
           byte_swap(static_cast<std::uint32_t>(v >> 32U));
}

template <typename T>
inline T read_le(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    if constexpr (std::endian::native == std::endian::big) v = byte_swap(v);
    return v;
}

template <typename T>
inline void write_le(std::uint8_t* p, T v) {
    if constexpr (std::endian::native == std::endian::big) v = byte_swap(v);
    std::memcpy(p, &v, sizeof v);
}

template <typename T, typename Buf>
inline void append_le(Buf& out, T v) {
    const std::size_t at = out.size();
    out.resize(at + sizeof v);
    write_le<T>(out.data() + at, v);
}

inline std::uint64_t double_bits(double d) {
    return std::bit_cast<std::uint64_t>(d);
}

inline double bits_double(std::uint64_t u) { return std::bit_cast<double>(u); }

inline std::uint16_t read_u16_le(const std::uint8_t* p) {
    return read_le<std::uint16_t>(p);
}
inline std::uint32_t read_u32_le(const std::uint8_t* p) {
    return read_le<std::uint32_t>(p);
}
inline std::uint64_t read_u64_le(const std::uint8_t* p) {
    return read_le<std::uint64_t>(p);
}
/// Reads the bit pattern of a double, NaN payloads included.
inline double read_f64_le(const std::uint8_t* p) {
    return bits_double(read_u64_le(p));
}

inline void write_u16_le(std::uint8_t* p, std::uint16_t v) {
    write_le<std::uint16_t>(p, v);
}
inline void write_u32_le(std::uint8_t* p, std::uint32_t v) {
    write_le<std::uint32_t>(p, v);
}
inline void write_u64_le(std::uint8_t* p, std::uint64_t v) {
    write_le<std::uint64_t>(p, v);
}
inline void write_f64_le(std::uint8_t* p, double v) {
    write_u64_le(p, double_bits(v));
}

/// Appends to any byte vector (dstd::Vector<std::uint8_t> or std::vector).
template <typename Buf>
inline void append_u16_le(Buf& out, std::uint16_t v) {
    append_le<std::uint16_t>(out, v);
}
template <typename Buf>
inline void append_u32_le(Buf& out, std::uint32_t v) {
    append_le<std::uint32_t>(out, v);
}
template <typename Buf>
inline void append_u64_le(Buf& out, std::uint64_t v) {
    append_le<std::uint64_t>(out, v);
}
template <typename Buf>
inline void append_f64_le(Buf& out, double v) {
    append_le<std::uint64_t>(out, double_bits(v));
}

/// Bounds-checked cursor over a byte range. A read that does not fit returns
/// false, writes nothing and leaves the cursor where it was.
class LeReader {
   public:
    LeReader(const std::uint8_t* data, std::size_t size)
        : p_(data), end_(data + size) {}

    std::size_t remaining() const {
        return static_cast<std::size_t>(end_ - p_);
    }
    const std::uint8_t* position() const { return p_; }

    bool u8(std::uint8_t& out) {
        if (remaining() < 1) return false;
        out = *p_++;
        return true;
    }
    bool u16(std::uint16_t& out) { return read(out); }
    bool u32(std::uint32_t& out) { return read(out); }
    bool u64(std::uint64_t& out) { return read(out); }
    bool f64(double& out) {
        std::uint64_t bits;
        if (!read(bits)) return false;
        out = bits_double(bits);
        return true;
    }
    bool i32(std::int32_t& out) {
        std::uint32_t bits;
        if (!read(bits)) return false;
        out = static_cast<std::int32_t>(bits);
        return true;
    }

    /// Returns the next n bytes and moves past them, or nullptr if fewer
    /// remain.
    const std::uint8_t* take(std::size_t n) {
        if (remaining() < n) return nullptr;
        const std::uint8_t* at = p_;
        p_ += n;
        return at;
    }

   private:
    template <typename T>
    bool read(T& out) {
        if (remaining() < sizeof(T)) return false;
        out = read_le<T>(p_);
        p_ += sizeof(T);
        return true;
    }

    const std::uint8_t* p_;
    const std::uint8_t* end_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_DSTD_LITTLE_ENDIAN_H
