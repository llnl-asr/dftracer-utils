#ifndef DFTRACER_UTILS_DATAFRAME_SCALAR_H
#define DFTRACER_UTILS_DATAFRAME_SCALAR_H

#include <dftracer/utils/dataframe/abi.h>

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace dftracer::utils::dataframe {

/// Build an I64-tagged scalar operand for the scalar-taking column kernels.
inline dftu_scalar i64(std::int64_t v) noexcept {
    dftu_scalar s;
    s.kind = DFTU_SCALAR_TAG_I64;
    s.value.i = v;
    return s;
}

/// Build a U64-tagged scalar operand.
inline dftu_scalar u64(std::uint64_t v) noexcept {
    dftu_scalar s;
    s.kind = DFTU_SCALAR_TAG_U64;
    s.value.u = v;
    return s;
}

/// Build an F64-tagged scalar operand.
inline dftu_scalar f64(double v) noexcept {
    dftu_scalar s;
    s.kind = DFTU_SCALAR_TAG_F64;
    s.value.d = v;
    return s;
}

/// Build a scalar for a boolean, carried as I64 0/1 (the kernels have no Bool
/// tag).
inline dftu_scalar boolean(bool v) noexcept { return i64(v ? 1 : 0); }

/// Build a STR-tagged scalar BORROWING `v`. `v` must outlive every call the
/// scalar is passed to; nothing stores a dftu_scalar past the call.
inline dftu_scalar str(std::string_view v) noexcept {
    dftu_scalar s;
    s.kind = DFTU_SCALAR_TAG_STR;
    s.len = static_cast<std::uint32_t>(v.size());
    s.value.s = v.data();
    return s;
}

/// Wrap any numeric value as a dftu_scalar tagged by its domain (signed -> I64,
/// unsigned -> U64, float -> F64), lossless. The seam C++ callers use instead
/// of populating a raw dftu_scalar.
template <class T>
inline dftu_scalar to_scalar(T v) {
    if constexpr (std::is_floating_point_v<T>) {
        return f64(static_cast<double>(v));
    } else if constexpr (std::is_unsigned_v<T>) {
        return u64(static_cast<std::uint64_t>(v));
    } else {
        return i64(static_cast<std::int64_t>(v));
    }
}

/// Read a dftu_scalar as `T`, converting from its tagged domain. The inverse
/// of to_scalar.
template <class T>
inline T scalar_value(dftu_scalar s) {
    switch (s.kind) {
        case DFTU_SCALAR_TAG_I64:
            return static_cast<T>(s.value.i);
        case DFTU_SCALAR_TAG_U64:
            return static_cast<T>(s.value.u);
        // A STR scalar has no numeric reading, and its union member is a
        // pointer: falling through to value.d would reinterpret it as a double.
        case DFTU_SCALAR_TAG_STR:
            return T{};
        default:
            return static_cast<T>(s.value.d);
    }
}

/// Ergonomic typed wrapper over a `dftu_scalar` result. Converts implicitly to
/// and from `dftu_scalar`, so a Scalar still passes to any API taking a
/// `dftu_scalar` and code assigning a reducer result to one keeps compiling.
/// The typed accessors delegate to scalar_value<T>().
struct Scalar {
    dftu_scalar raw_{};

    Scalar() = default;
    Scalar(dftu_scalar s) noexcept : raw_(s) {}
    operator dftu_scalar() const noexcept { return raw_; }

    double f64() const { return scalar_value<double>(raw_); }
    std::int64_t i64() const { return scalar_value<std::int64_t>(raw_); }
    std::uint64_t u64() const { return scalar_value<std::uint64_t>(raw_); }
    bool boolean() const { return i64() != 0; }

    template <class T>
    T as() const {
        return scalar_value<T>(raw_);
    }

    dftu_scalar_tag tag() const noexcept {
        return static_cast<dftu_scalar_tag>(raw_.kind);
    }
    dftu_scalar_tag kind() const noexcept { return tag(); }
    dftu_scalar raw() const noexcept { return raw_; }

    /// The borrowed text of a STR-tagged scalar, empty for every other tag.
    /// The view points at whatever the scalar borrowed, so it is valid only as
    /// long as that is.
    std::string_view str() const noexcept {
        return raw_.kind == DFTU_SCALAR_TAG_STR && raw_.value.s != nullptr
                   ? std::string_view{raw_.value.s, raw_.len}
                   : std::string_view{};
    }
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_SCALAR_H
