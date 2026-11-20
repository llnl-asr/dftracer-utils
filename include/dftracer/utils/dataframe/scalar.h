// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_SCALAR_H
#define DFTRACER_UTILS_DATAFRAME_SCALAR_H

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdint>
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
inline dftu_scalar str(dstd::StringView v) noexcept {
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
        // A STR or ERR scalar has no numeric reading, and its union member is a
        // pointer: falling through to value.d would reinterpret it as a double.
        case DFTU_SCALAR_TAG_STR:
        case DFTU_SCALAR_TAG_ERR:
        case DFTU_SCALAR_TAG_NULL:
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
    dftu_scalar data{};

    Scalar() = default;
    Scalar(dftu_scalar s) noexcept : data(s) {}
    operator dftu_scalar() const noexcept { return data; }

    double f64() const { return scalar_value<double>(data); }
    std::int64_t i64() const { return scalar_value<std::int64_t>(data); }
    std::uint64_t u64() const { return scalar_value<std::uint64_t>(data); }
    bool boolean() const { return i64() != 0; }

    template <class T>
    T as() const {
        return scalar_value<T>(data);
    }

    dftu_scalar_tag tag() const noexcept {
        return static_cast<dftu_scalar_tag>(data.kind);
    }
    dftu_scalar_tag kind() const noexcept { return tag(); }
    dftu_scalar raw() const noexcept { return data; }

    /// The borrowed text of a STR-tagged scalar, empty for every other tag.
    /// The view points at whatever the scalar borrowed, so it is valid only as
    /// long as that is.
    dstd::StringView str() const noexcept {
        return data.kind == DFTU_SCALAR_TAG_STR && data.value.s != nullptr
                   ? dstd::StringView{data.value.s, data.len}
                   : dstd::StringView{};
    }
};

/// The domain an operand carries, one value per scalar tag: a signed integer,
/// an unsigned integer, a float, borrowed text, an error or no value. The tag
/// of a value does not name its column type; read a literal with @ref
/// ScalarView::as and cast it to the type the argument has.
enum class ScalarKind : std::int32_t {
    I64 = DFTU_SCALAR_TAG_I64,
    U64 = DFTU_SCALAR_TAG_U64,
    F64 = DFTU_SCALAR_TAG_F64,
    Str = DFTU_SCALAR_TAG_STR,
    Err = DFTU_SCALAR_TAG_ERR,
    Null = DFTU_SCALAR_TAG_NULL,
};

/// A borrowed view of an operand the caller owns, such as a constant argument a
/// query writes. It copies nothing, allocates nothing and holds no reference,
/// so it is valid only while the operand it views lives.
class ScalarView {
   public:
    ScalarView() noexcept = default;
    /// A view of @p scalar, which stays owned by the caller and must outlive
    /// the view. NULL views no operand.
    explicit ScalarView(const dftu_scalar* scalar) noexcept : scalar_(scalar) {}
    /// The domain of the operand.
    ScalarKind kind() const noexcept {
        return scalar_ != nullptr ? static_cast<ScalarKind>(scalar_->kind)
                                  : ScalarKind::Err;
    }
    /// Whether the operand is borrowed text, read with @ref str.
    bool is_str() const noexcept { return kind() == ScalarKind::Str; }

    /// The text of a Str operand, borrowed; empty for any other kind.
    dstd::StringView str() const noexcept {
        return is_str() && scalar_->value.s != nullptr
                   ? dstd::StringView{scalar_->value.s, scalar_->len}
                   : dstd::StringView{};
    }

    std::int64_t i64() const noexcept { return as<std::int64_t>(); }
    std::uint64_t u64() const noexcept { return as<std::uint64_t>(); }
    double f64() const noexcept { return as<double>(); }
    /// A Bool operand carried as 0/1, non-zero being true.
    bool boolean() const noexcept { return i64() != 0; }

    /// The operand as `T`, converting from its own domain; `T` for text and for
    /// an error operand.
    template <class T>
    T as() const noexcept {
        return scalar_ != nullptr ? scalar_value<T>(*scalar_) : T{};
    }

   private:
    const dftu_scalar* scalar_ = nullptr;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_SCALAR_H
