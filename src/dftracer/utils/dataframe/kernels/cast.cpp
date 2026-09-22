#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/decimal.h>
#include <dftracer/utils/dataframe/internal/float16.h>
#include <dftracer/utils/dataframe/internal/numeric_dispatch.h>
#include <dftracer/utils/dataframe/internal/varwidth_offsets.h>
#include <dftracer/utils/dataframe/kernels/cast.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace dftracer::utils::dataframe {
namespace {

template <class S>
void cast_from(const void* sv, std::int32_t dtype, void* dv, std::size_t n) {
    const S* s = static_cast<const S*>(sv);
#define DF_CAST_TO(D)                                                    \
    do {                                                                 \
        D* d = static_cast<D*>(dv);                                      \
        for (std::size_t i = 0; i < n; ++i) d[i] = static_cast<D>(s[i]); \
    } while (0)
    switch (static_cast<TypeId>(dtype)) {
        case TypeId::Int8:
            DF_CAST_TO(std::int8_t);
            break;
        case TypeId::Int16:
            DF_CAST_TO(std::int16_t);
            break;
        case TypeId::Int32:
            DF_CAST_TO(std::int32_t);
            break;
        case TypeId::Int64:
            DF_CAST_TO(std::int64_t);
            break;
        case TypeId::Uint8:
            DF_CAST_TO(std::uint8_t);
            break;
        case TypeId::Uint16:
            DF_CAST_TO(std::uint16_t);
            break;
        case TypeId::Uint32:
            DF_CAST_TO(std::uint32_t);
            break;
        case TypeId::Uint64:
            DF_CAST_TO(std::uint64_t);
            break;
        case TypeId::Float32:
            DF_CAST_TO(float);
            break;
        case TypeId::Float64:
            DF_CAST_TO(double);
            break;
        default:
            break;
    }
#undef DF_CAST_TO
}

// Float16 is physically uint16 bits, not an integer: it needs to be decoded
// (never reinterpreted) before any numeric op sees it. Decoding is exact, so
// this is a lossless stand-in source for the generic cast_from<float> path.
bool cast_from_float16(const void* sv, std::int32_t dtype, void* dv,
                       std::size_t n) {
    const auto* h = static_cast<const std::uint16_t*>(sv);
    std::vector<float> decoded(n);
    for (std::size_t i = 0; i < n; ++i) decoded[i] = half_to_float(h[i]);
    if (static_cast<TypeId>(dtype) == TypeId::Float16) {
        auto* d = static_cast<std::uint16_t*>(dv);
        for (std::size_t i = 0; i < n; ++i) d[i] = h[i];
        return true;
    }
    cast_from<float>(decoded.data(), dtype, dv, n);
    return true;
}

// Decimal128/256 are opaque fixed-width bytes to every numeric kernel; the
// only cast this engine defines for them is decoding to Float64 (lossy - see
// is_arithmetic_type's documentation). Casting INTO a decimal type would need
// a rounding policy this engine does not define yet, so that direction is
// refused (dftu_series_cast returns null).
bool cast_from_decimal(TypeId src, const void* sv, std::int32_t width,
                       std::int32_t scale, std::int32_t dtype, void* dv,
                       std::size_t n) {
    if (static_cast<TypeId>(dtype) != TypeId::Float64) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(sv);
    auto* d = static_cast<double*>(dv);
    for (std::size_t i = 0; i < n; ++i) {
        const void* row = bytes + i * static_cast<std::size_t>(width);
        d[i] = src == TypeId::Decimal128 ? decimal128_to_double(row, scale)
                                         : decimal256_to_double(row, scale);
    }
    return true;
}

// Encodes an already-computed Float32 buffer down to Float16 (round-to-
// nearest-even); used when the cast TARGET is Float16.
void encode_to_float16(const float* src, std::uint16_t* dst, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) dst[i] = float_to_half(src[i]);
}

bool is_numeric(TypeId t) { return is_arithmetic_type(t); }

bool is_integral(TypeId t) {
    switch (t) {
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
            return true;
        default:
            return false;
    }
}

bool row_valid(const dftu_series& v, std::int64_t i) {
    if (!v.validity) return true;
    return (v.validity->data()[i >> 3] >> (i & 7)) & 1;
}

// Each row of a FLAT String column parsed as `mid` (Int64 or Float64): the
// whole row must be the number, surrounding spaces allowed; anything else
// (and a null) is null.
template <class Off>
dftu_series* parse_numbers_w(const dftu_series& v, TypeId mid) {
    const std::int64_t n = v.length;
    auto* out = new dftu_series();
    out->type = mid;
    out->encoding = Encoding::Flat;
    out->length = n;
    out->null_count = 0;
    out->data = Buffer::allocate(buffer_bytes(mid, n));
    out->validity = Buffer::allocate(buffer_bytes(TypeId::Bool, n));
    std::memset(out->validity->data(), 0, out->validity->size());
    const Off* off = reinterpret_cast<const Off*>(offsets_of<Off>(v)->data());
    const char* data = reinterpret_cast<const char*>(v.data->data());
    auto* oi = reinterpret_cast<std::int64_t*>(out->data->data());
    auto* od = reinterpret_cast<double*>(out->data->data());
    for (std::int64_t i = 0; i < n; ++i) {
        if (mid == TypeId::Int64)
            oi[i] = 0;
        else
            od[i] = 0.0;
        if (!row_valid(v, i)) {
            ++out->null_count;
            continue;
        }
        std::string text(data + off[i],
                         static_cast<std::size_t>(off[i + 1] - off[i]));
        std::size_t used = 0;
        bool ok = false;
        try {
            if (mid == TypeId::Int64) {
                const long long x = std::stoll(text, &used);
                oi[i] = static_cast<std::int64_t>(x);
            } else {
                od[i] = std::stod(text, &used);
            }
            while (used < text.size() &&
                   std::isspace(static_cast<unsigned char>(text[used])))
                ++used;
            ok = used == text.size();
        } catch (const std::exception&) {
            ok = false;
        }
        if (ok)
            out->validity->data()[i >> 3] |=
                static_cast<std::uint8_t>(1u << (i & 7));
        else
            ++out->null_count;
    }
    if (out->null_count == 0) out->validity = nullptr;
    return out;
}

dftu_series* parse_numbers(const dftu_series& v, TypeId mid) {
    if (v.encoding != Encoding::Flat) return nullptr;
    return is_wide_offset_type(v.type) ? parse_numbers_w<std::int64_t>(v, mid)
                                       : parse_numbers_w<std::int32_t>(v, mid);
}

}  // namespace

Series cast(const Series& v, TypeId target) {
    return Series{
        dftu_series_cast(v.handle(), static_cast<dftu_dtype>(target))};
}

}  // namespace dftracer::utils::dataframe

dftu_series* dftu_series_cast(const dftu_series* v, dftu_dtype target) {
    DFTU_FLAT_OPERAND(v, flat_v, dftu_series_cast(flat_v, target));

    using dftracer::utils::dataframe::Buffer;
    using dftracer::utils::dataframe::buffer_bytes;
    using dftracer::utils::dataframe::byte_width;
    using dftracer::utils::dataframe::Encoding;
    using dftracer::utils::dataframe::TypeId;
    TypeId src = v->type;
    TypeId dst = static_cast<TypeId>(target);
    if (v->encoding != Encoding::Flat) return nullptr;
    if (src == TypeId::Bool && dftracer::utils::dataframe::is_numeric(dst)) {
        // Unpack the bits to 0 / 1 as Int64, then widen or narrow from there.
        const std::size_t n = static_cast<std::size_t>(v->length);
        std::vector<std::int64_t> ones(n);
        const auto* bits = static_cast<const std::uint8_t*>(v->data->data());
        for (std::size_t i = 0; i < n; ++i)
            ones[i] = (bits[i >> 3] >> (i & 7)) & 1;
        auto* as_i64 = new dftu_series();
        as_i64->type = TypeId::Int64;
        as_i64->encoding = Encoding::Flat;
        as_i64->length = v->length;
        as_i64->null_count = v->null_count;
        as_i64->validity = v->validity;
        as_i64->data = Buffer::allocate(buffer_bytes(TypeId::Int64, v->length));
        std::memcpy(as_i64->data->data(), ones.data(),
                    n * sizeof(std::int64_t));
        if (dst == TypeId::Int64) return as_i64;
        dftu_series* out = dftu_series_cast(as_i64, target);
        dftu_series_free(as_i64);
        return out;
    }
    if ((src == TypeId::String || src == TypeId::LargeString) &&
        dftracer::utils::dataframe::is_numeric(dst)) {
        // Parse each row as a number (a whole-row integer or a float);
        // unparsable text is null. Through Float64 or Int64, then the
        // numeric cast narrows.
        const TypeId mid = dftracer::utils::dataframe::is_integral(dst)
                               ? TypeId::Int64
                               : TypeId::Float64;
        dftu_series* parsed =
            dftracer::utils::dataframe::parse_numbers(*v, mid);
        if (!parsed) return nullptr;
        if (dst == mid) return parsed;
        dftu_series* out = dftu_series_cast(parsed, target);
        dftu_series_free(parsed);
        return out;
    }
    // A temporal column casts to a number as its raw ticks (Int64 storage;
    // Date32 / Time32 are Int32), the unit dropped.
    switch (src) {
        case TypeId::Timestamp:
        case TypeId::Duration:
        case TypeId::Date64:
        case TypeId::Time64:
            src = TypeId::Int64;
            break;
        case TypeId::Date32:
        case TypeId::Time32:
            src = TypeId::Int32;
            break;
        default:
            break;
    }
    if (!dftracer::utils::dataframe::is_numeric(src) ||
        !dftracer::utils::dataframe::is_numeric(dst))
        return nullptr;
    if ((src == TypeId::Decimal128 || src == TypeId::Decimal256) &&
        dst != TypeId::Float64)
        return nullptr;
    if (dst == TypeId::Decimal128 || dst == TypeId::Decimal256) return nullptr;

    auto* out = new dftu_series();
    out->type = dst;
    out->encoding = Encoding::Flat;
    out->length = v->length;
    out->null_count = v->null_count;
    out->validity = v->validity;
    out->data = Buffer::allocate(buffer_bytes(dst, v->length));

    const void* sv = v->data->data();
    void* dv = out->data->data();
    std::size_t n = static_cast<std::size_t>(v->length);

    if (src == TypeId::Float16) {
        dftracer::utils::dataframe::cast_from_float16(
            sv, static_cast<std::int32_t>(dst), dv, n);
    } else if (src == TypeId::Decimal128 || src == TypeId::Decimal256) {
        dftracer::utils::dataframe::cast_from_decimal(
            src, sv, static_cast<std::int32_t>(byte_width(src).value_or(0)),
            v->decimal_scale, static_cast<std::int32_t>(dst), dv, n);
    } else if (dst == TypeId::Float16) {
        auto tmp = std::vector<float>(n);
        DF_NUMERIC_DISPATCH(src, dftracer::utils::dataframe::cast_from, sv,
                            static_cast<std::int32_t>(TypeId::Float32),
                            tmp.data(), n)
        dftracer::utils::dataframe::encode_to_float16(
            tmp.data(), static_cast<std::uint16_t*>(dv), n);
    } else if (!dftracer::utils::dataframe::cast_simd(
                   static_cast<std::int32_t>(src),
                   static_cast<std::int32_t>(dst), sv, dv, n)) {
        DF_NUMERIC_DISPATCH(src, dftracer::utils::dataframe::cast_from, sv,
                            target, dv, n)
    }
    return out;
}
