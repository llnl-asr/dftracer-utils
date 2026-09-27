#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/internal/compare_simd.h>
#include <dftracer/utils/dataframe/internal/numeric_dispatch.h>
#include <dftracer/utils/dataframe/internal/scalar.h>
#include <dftracer/utils/dataframe/kernels/comparison.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/series.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace dftracer::utils::dataframe {
namespace {

// Compare in the column's own type domain (no double round-trip), so 64-bit
// integers compare exactly. The scalar is converted to the column type T.
// `op` is validated at the ABI boundary (dftu_series_compare) before dispatch.
template <class T>
void compare_impl(const void* data, std::int64_t length, std::int32_t op,
                  dftu_scalar rhs, std::uint8_t* out) {
    const T* p = static_cast<const T*>(data);
    const T r = scalar_as<T>(rhs);
    for (std::int64_t i = 0; i < length; ++i) {
        bool res = false;
        switch (static_cast<CmpOp>(op)) {
            case CmpOp::Gt:
                res = p[i] > r;
                break;
            case CmpOp::Ge:
                res = p[i] >= r;
                break;
            case CmpOp::Lt:
                res = p[i] < r;
                break;
            case CmpOp::Le:
                res = p[i] <= r;
                break;
            case CmpOp::Eq:
                res = p[i] == r;
                break;
            case CmpOp::Ne:
                res = p[i] != r;
                break;
        }
        if (res) out[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
}

// An integer as sign and magnitude, so int64 and uint64 bounds mix exactly.
struct Wide {
    bool neg = false;
    std::uint64_t mag = 0;
};

int wide_cmp(Wide a, Wide b) {
    if (a.neg != b.neg) return a.neg ? -1 : 1;
    if (a.mag == b.mag) return 0;
    return (a.mag < b.mag) != a.neg ? -1 : 1;
}

Wide wide_of(std::int64_t v) {
    return v < 0 ? Wide{true, std::uint64_t{0} - static_cast<std::uint64_t>(v)}
                 : Wide{false, static_cast<std::uint64_t>(v)};
}

dftu_scalar wide_scalar(Wide w) {
    dftu_scalar s{};
    if (w.neg) {
        s.kind = DFTU_SCALAR_TAG_I64;
        s.value.i = static_cast<std::int64_t>(std::uint64_t{0} - w.mag);
    } else {
        s.kind = DFTU_SCALAR_TAG_U64;
        s.value.u = w.mag;
    }
    return s;
}

dftu_scalar f64_scalar(double d) {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_F64;
    s.value.d = d;
    return s;
}

bool integer_bounds(TypeId t, Wide& lo, Wide& hi) {
    int bits = 0;
    bool is_signed = true;
    switch (t) {
        case TypeId::Int8:
            bits = 8;
            break;
        case TypeId::Int16:
            bits = 16;
            break;
        case TypeId::Int32:
            bits = 32;
            break;
        case TypeId::Int64:
            bits = 64;
            break;
        case TypeId::Uint8:
            bits = 8;
            is_signed = false;
            break;
        case TypeId::Uint16:
            bits = 16;
            is_signed = false;
            break;
        case TypeId::Uint32:
            bits = 32;
            is_signed = false;
            break;
        case TypeId::Uint64:
            bits = 64;
            is_signed = false;
            break;
        default:
            return false;
    }
    if (is_signed) {
        const std::uint64_t half = std::uint64_t{1} << (bits - 1);
        lo = {true, half};
        hi = {false, half - 1};
    } else {
        lo = {false, 0};
        hi = {false,
              bits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1};
    }
    return true;
}

// Every row satisfies `op` (`all`) or none does, as an op over the column's
// own range.
void constant_result(bool all, Wide hi, std::int32_t& op, dftu_scalar& rhs) {
    op = static_cast<std::int32_t>(all ? CmpOp::Le : CmpOp::Gt);
    rhs = wide_scalar(hi);
}

// Rewrites `x <op> rhs` over an integer column into an equivalent op against
// a value of the column's range, so converting rhs to the column type never
// rounds, truncates or wraps.
void exact_integer_rhs(TypeId t, std::int32_t& op, dftu_scalar& rhs) {
    Wide lo, hi;
    if (!integer_bounds(t, lo, hi)) return;
    const auto cmp = static_cast<CmpOp>(op);
    Wide w;
    bool frac = false;
    if (rhs.kind == DFTU_SCALAR_TAG_I64) {
        w = wide_of(rhs.value.i);
    } else if (rhs.kind == DFTU_SCALAR_TAG_U64) {
        w = {false, rhs.value.u};
    } else if (rhs.kind == DFTU_SCALAR_TAG_F64) {
        const double d = rhs.value.d;
        if (std::isnan(d))
            return constant_result(cmp == CmpOp::Ne, hi, op, rhs);
        if (d >= 18446744073709551616.0) {
            w = {false, ~std::uint64_t{0}};
            frac = true;
        } else if (d < -9223372036854775808.0) {
            w = {true, ~std::uint64_t{0}};
        } else {
            const double f = std::floor(d);
            w = f < 0 ? Wide{true, static_cast<std::uint64_t>(-f)}
                      : Wide{false, static_cast<std::uint64_t>(f)};
            frac = d != f;
        }
    } else {
        return;
    }
    const bool below = wide_cmp(w, lo) < 0;
    const bool above = frac ? wide_cmp(w, hi) >= 0 : wide_cmp(w, hi) > 0;
    if (below || above) {
        const bool all = cmp == CmpOp::Ne ||
                         (below && (cmp == CmpOp::Gt || cmp == CmpOp::Ge)) ||
                         (above && (cmp == CmpOp::Lt || cmp == CmpOp::Le));
        return constant_result(all, hi, op, rhs);
    }
    rhs = wide_scalar(w);
    if (!frac) return;
    switch (cmp) {
        case CmpOp::Lt:
        case CmpOp::Le:
            op = static_cast<std::int32_t>(CmpOp::Le);
            break;
        case CmpOp::Gt:
        case CmpOp::Ge:
            op = static_cast<std::int32_t>(CmpOp::Gt);
            break;
        case CmpOp::Eq:
        case CmpOp::Ne:
            constant_result(cmp == CmpOp::Ne, hi, op, rhs);
            break;
    }
}

// -1/0/1 for an integer against a double that is not NaN, exactly.
int int_vs_double(Wide k, double d) {
    if (d >= 18446744073709551616.0) return -1;
    if (d < -18446744073709551616.0) return 1;
    const double f = std::floor(d);
    const Wide fw = f < 0 ? Wide{true, static_cast<std::uint64_t>(-f)}
                          : Wide{false, static_cast<std::uint64_t>(f)};
    const int c = wide_cmp(k, fw);
    if (c != 0) return c;
    return d > f ? -1 : 0;
}

// Rewrites `x <op> rhs` over a Float32/Float64 column so rhs is a value of
// the column type: an integer past the type's exact range or a double
// between two floats becomes an op against its representable neighbour.
template <class F>
void exact_float_rhs(std::int32_t& op, dftu_scalar& rhs) {
    constexpr F INF = std::numeric_limits<F>::infinity();
    F near;
    int c;  // rhs against `near`
    if (rhs.kind == DFTU_SCALAR_TAG_I64 || rhs.kind == DFTU_SCALAR_TAG_U64) {
        const Wide k = rhs.kind == DFTU_SCALAR_TAG_I64
                           ? wide_of(rhs.value.i)
                           : Wide{false, rhs.value.u};
        near = rhs.kind == DFTU_SCALAR_TAG_I64 ? static_cast<F>(rhs.value.i)
                                               : static_cast<F>(rhs.value.u);
        c = int_vs_double(k, static_cast<double>(near));
    } else if (rhs.kind == DFTU_SCALAR_TAG_F64) {
        const double d = rhs.value.d;
        if (std::is_same_v<F, double> || std::isnan(d) || std::isinf(d)) return;
        constexpr double MAX =
            static_cast<double>(std::numeric_limits<F>::max());
        near = d > MAX    ? std::numeric_limits<F>::max()
               : d < -MAX ? -std::numeric_limits<F>::max()
                          : static_cast<F>(d);
        const double nd = static_cast<double>(near);
        c = d < nd ? -1 : (d > nd ? 1 : 0);
    } else {
        return;
    }
    if (c == 0) {
        rhs = f64_scalar(static_cast<double>(near));
        return;
    }
    const F lower = c > 0 ? near : std::nextafter(near, -INF);
    const F upper = c > 0 ? std::nextafter(near, INF) : near;
    switch (static_cast<CmpOp>(op)) {
        case CmpOp::Lt:
        case CmpOp::Le:
            op = static_cast<std::int32_t>(CmpOp::Le);
            rhs = f64_scalar(static_cast<double>(lower));
            break;
        case CmpOp::Gt:
        case CmpOp::Ge:
            op = static_cast<std::int32_t>(CmpOp::Ge);
            rhs = f64_scalar(static_cast<double>(upper));
            break;
        case CmpOp::Eq:
            op = static_cast<std::int32_t>(CmpOp::Lt);
            rhs = f64_scalar(-std::numeric_limits<double>::infinity());
            break;
        case CmpOp::Ne:
            rhs = f64_scalar(std::numeric_limits<double>::quiet_NaN());
            break;
    }
}

void exact_rhs(TypeId phys, std::int32_t& op, dftu_scalar& rhs) {
    if (phys == TypeId::Float64) return exact_float_rhs<double>(op, rhs);
    if (phys == TypeId::Float32) return exact_float_rhs<float>(op, rhs);
    exact_integer_rhs(phys, op, rhs);
}

}  // namespace
}  // namespace dftracer::utils::dataframe

dftu_series* dftu_series_compare(const dftu_series* v, dftu_cmp_op op,
                                 dftu_scalar rhs) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_compare(flat_v, op, rhs));

    using dftracer::utils::dataframe::Buffer;
    using dftracer::utils::dataframe::buffer_bytes;
    using dftracer::utils::dataframe::CmpOp;
    using dftracer::utils::dataframe::compare_impl;
    using dftracer::utils::dataframe::Encoding;
    using dftracer::utils::dataframe::TypeId;
    // A STR rhs is a string comparison, which is a different kernel: equality
    // only, and dictionary-aware (dftu_series_str_eq tests each dictionary
    // entry once and then compares codes, rather than resolving per row).
    if (rhs.kind == DFTU_SCALAR_TAG_STR) {
        if (op != static_cast<int32_t>(CmpOp::Eq) &&
            op != static_cast<int32_t>(CmpOp::Ne))
            return nullptr;
        dftu_series* eq =
            dftu_series_str_eq(v, rhs.value.s, static_cast<int32_t>(rhs.len));
        if (eq == nullptr || op == static_cast<int32_t>(CmpOp::Eq)) return eq;
        dftu_series* ne = dftu_series_logical_not(eq);
        dftu_series_free(eq);
        return ne;
    }

    if (v->encoding != Encoding::Flat) return nullptr;
    const TypeId phys = physical_type(v->type);
    if (!is_numeric_dispatchable(phys)) return nullptr;
    if (op < static_cast<int32_t>(CmpOp::Gt) ||
        op > static_cast<int32_t>(CmpOp::Ne))
        return nullptr;
    std::int32_t exact_op = op;
    dftracer::utils::dataframe::exact_rhs(phys, exact_op, rhs);
    op = static_cast<dftu_cmp_op>(exact_op);

    auto* out = new dftu_series();
    out->type = TypeId::Bool;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;  // shared: nulls propagate

    std::size_t bytes = buffer_bytes(TypeId::Bool, v->length);
    out->data = Buffer::allocate(bytes);
    std::memset(out->data->data(), 0, bytes);
    std::uint8_t* bits = out->data->data();
    const void* data = v->data->data();
    std::int64_t n = v->length;

    // Chunks of whole 64-bit words: each writes its own bytes of the bitmap,
    // so the SIMD kernel runs on every core over a disjoint row range.
    const std::size_t width = byte_width(phys).value_or(0);
    dftracer::utils::dataframe::parallel_for(
        n, std::int64_t{1} << 16, [&](std::int64_t b, std::int64_t e) {
            const auto* pd = static_cast<const std::uint8_t*>(data) +
                             static_cast<std::size_t>(b) * width;
            std::uint8_t* pb = bits + (b >> 3);
            if (!dftracer::utils::dataframe::compare(v->type, pd, e - b, op,
                                                     rhs, pb)) {
                DF_NUMERIC_DISPATCH(phys, compare_impl, pd, e - b, op, rhs, pb)
            }
        });
    return out;
}

namespace {

using dftracer::utils::dataframe::CmpOp;

template <class T>
bool cmp_values(std::int32_t op, T a, T b) {
    switch (static_cast<CmpOp>(op)) {
        case CmpOp::Gt:
            return a > b;
        case CmpOp::Ge:
            return a >= b;
        case CmpOp::Lt:
            return a < b;
        case CmpOp::Le:
            return a <= b;
        case CmpOp::Eq:
            return a == b;
        case CmpOp::Ne:
            return a != b;
    }
    return false;
}

// Row range [b, e) of two columns of one physical type T.
template <class T>
void compare_series_impl(const void* pa, const void* pb, std::int64_t b,
                         std::int64_t e, std::int32_t op, std::uint8_t* out) {
    const T* x = static_cast<const T*>(pa);
    const T* y = static_cast<const T*>(pb);
    for (std::int64_t i = b; i < e; ++i)
        if (cmp_values(op, x[i], y[i]))
            out[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
}

}  // namespace

dftu_series* dftu_series_compare_series(const dftu_series* a,
                                        const dftu_series* b, dftu_cmp_op op) {
    if (!a || !b || a->length != b->length) return nullptr;
    DFTU_FLAT_OPERAND(a, flat_a, dftu_series_compare_series(flat_a, b, op));
    DFTU_FLAT_OPERAND(b, flat_b, dftu_series_compare_series(a, flat_b, op));
    using dftracer::utils::dataframe::Buffer;
    using dftracer::utils::dataframe::buffer_bytes;
    using dftracer::utils::dataframe::Encoding;
    using dftracer::utils::dataframe::narrow_varwidth_type;
    using dftracer::utils::dataframe::read_bytes;
    using dftracer::utils::dataframe::read_f64;
    using dftracer::utils::dataframe::Series;
    using dftracer::utils::dataframe::TypeId;
    if (op < static_cast<int32_t>(CmpOp::Gt) ||
        op > static_cast<int32_t>(CmpOp::Ne))
        return nullptr;
    const std::int64_t n = a->length;
    const TypeId ta = physical_type(a->type), tb = physical_type(b->type);
    const bool bytes_a = narrow_varwidth_type(a->type) == TypeId::String ||
                         narrow_varwidth_type(a->type) == TypeId::Binary;
    const bool bytes_b = narrow_varwidth_type(b->type) == TypeId::String ||
                         narrow_varwidth_type(b->type) == TypeId::Binary;
    if (bytes_a != bytes_b) return nullptr;
    if (!bytes_a &&
        (!is_numeric_dispatchable(ta) || !is_numeric_dispatchable(tb)))
        return nullptr;

    auto* out = new dftu_series();
    out->type = TypeId::Bool;
    out->encoding = Encoding::Flat;
    out->length = n;
    const std::size_t bytes = buffer_bytes(TypeId::Bool, n);
    out->data = Buffer::allocate(bytes);
    std::memset(out->data->data(), 0, bytes);
    std::uint8_t* bits = out->data->data();

    // Null where either side is null: the AND of the two bitmaps.
    if (a->validity || b->validity) {
        out->validity = Buffer::allocate(bytes);
        std::uint8_t* v = out->validity->data();
        const std::uint8_t* va = a->validity ? a->validity->data() : nullptr;
        const std::uint8_t* vb = b->validity ? b->validity->data() : nullptr;
        std::int64_t nulls = 0;
        for (std::size_t i = 0; i < bytes; ++i)
            v[i] = static_cast<std::uint8_t>((va ? va[i] : 0xFF) &
                                             (vb ? vb[i] : 0xFF));
        for (std::int64_t i = 0; i < n; ++i)
            if (!((v[i >> 3] >> (i & 7)) & 1)) ++nulls;
        out->null_count = nulls;
    }

    constexpr std::int64_t GRAIN = std::int64_t{1} << 16;
    dftracer::utils::dataframe::parallel_for(
        n, GRAIN, [&](std::int64_t lo, std::int64_t hi) {
            if (bytes_a) {
                Series sa{const_cast<dftu_series*>(a)};
                Series sb{const_cast<dftu_series*>(b)};
                for (std::int64_t i = lo; i < hi; ++i)
                    if (cmp_values(op, read_bytes(sa, i), read_bytes(sb, i)))
                        bits[i >> 3] |=
                            static_cast<std::uint8_t>(1u << (i & 7));
                sa.release();
                sb.release();
            } else if (ta == tb) {
                const void* pa = a->data->data();
                const void* pb = b->data->data();
                DF_NUMERIC_DISPATCH(ta, compare_series_impl, pa, pb, lo, hi, op,
                                    bits)
            } else {
                Series sa{const_cast<dftu_series*>(a)};
                Series sb{const_cast<dftu_series*>(b)};
                for (std::int64_t i = lo; i < hi; ++i)
                    if (cmp_values(op, read_f64(sa, i), read_f64(sb, i)))
                        bits[i >> 3] |=
                            static_cast<std::uint8_t>(1u << (i & 7));
                sa.release();
                sb.release();
            }
        });
    return out;
}
