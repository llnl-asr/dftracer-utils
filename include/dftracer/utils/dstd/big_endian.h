#ifndef DFTRACER_UTILS_DSTD_BIG_ENDIAN_H
#define DFTRACER_UTILS_DSTD_BIG_ENDIAN_H

#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/varint.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dftracer::utils {

// Big-endian fixed-width byte codec, the mirror of little_endian.h. The read_*
// and write_* functions do not check bounds: the caller owns the buffer.
// BeReader is the checked form. On a little-endian host each function compiles
// to one load or store plus a byte swap.

template <typename T>
inline T read_be(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    if constexpr (std::endian::native == std::endian::little) v = byte_swap(v);
    return v;
}

template <typename T>
inline void write_be(std::uint8_t* p, T v) {
    if constexpr (std::endian::native == std::endian::little) v = byte_swap(v);
    std::memcpy(p, &v, sizeof v);
}

template <typename T, typename Buf>
inline void append_be(Buf& out, T v) {
    const std::size_t at = out.size();
    out.resize(at + sizeof v);
    write_be<T>(reinterpret_cast<std::uint8_t*>(out.data()) + at, v);
}

inline std::uint16_t read_u16_be(const std::uint8_t* p) {
    return read_be<std::uint16_t>(p);
}
inline std::uint32_t read_u32_be(const std::uint8_t* p) {
    return read_be<std::uint32_t>(p);
}
inline std::uint64_t read_u64_be(const std::uint8_t* p) {
    return read_be<std::uint64_t>(p);
}
/// Reads the bit pattern of a double, NaN payloads included.
inline double read_f64_be(const std::uint8_t* p) {
    return bits_double(read_u64_be(p));
}

inline void write_u16_be(std::uint8_t* p, std::uint16_t v) {
    write_be<std::uint16_t>(p, v);
}
inline void write_u32_be(std::uint8_t* p, std::uint32_t v) {
    write_be<std::uint32_t>(p, v);
}
inline void write_u64_be(std::uint8_t* p, std::uint64_t v) {
    write_be<std::uint64_t>(p, v);
}
inline void write_f64_be(std::uint8_t* p, double v) {
    write_u64_be(p, double_bits(v));
}

/// Appends to any byte vector (std::string, dstd::Vector<std::uint8_t>).
template <typename Buf>
inline void append_u16_be(Buf& out, std::uint16_t v) {
    append_be<std::uint16_t>(out, v);
}
template <typename Buf>
inline void append_u32_be(Buf& out, std::uint32_t v) {
    append_be<std::uint32_t>(out, v);
}
template <typename Buf>
inline void append_u64_be(Buf& out, std::uint64_t v) {
    append_be<std::uint64_t>(out, v);
}
template <typename Buf>
inline void append_f64_be(Buf& out, double v) {
    append_be<std::uint64_t>(out, double_bits(v));
}

/// Bounds-checked cursor over a byte range. A read that does not fit returns
/// false, writes nothing and leaves the cursor where it was.
class BeReader {
   public:
    BeReader(const std::uint8_t* data, std::size_t size)
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
    bool varint(std::uint64_t& out) { return read_varint(p_, end_, out); }

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
        out = read_be<T>(p_);
        p_ += sizeof(T);
        return true;
    }

    const std::uint8_t* p_;
    const std::uint8_t* end_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_DSTD_BIG_ENDIAN_H
