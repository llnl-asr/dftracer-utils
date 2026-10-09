#ifndef DFTRACER_UTILS_DATAFRAME_LAZY_NUMERIC_H
#define DFTRACER_UTILS_DATAFRAME_LAZY_NUMERIC_H

#include <dftracer/utils/dataframe/lazy/common.h>

namespace dftracer::utils::dataframe::lazy_internal {

enum class NumClass { SIGNED, UNSIGNED, FLOAT };

inline bool num_class(TypeId t, NumClass& cls) {
    switch (t) {
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
            cls = NumClass::SIGNED;
            return true;
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
            cls = NumClass::UNSIGNED;
            return true;
        case TypeId::Float32:
        case TypeId::Float64:
            cls = NumClass::FLOAT;
            return true;
        default:
            return false;
    }
}

inline std::uint64_t double_bits(double d) {
    std::uint64_t u;
    std::memcpy(&u, &d, sizeof u);
    return u;
}
inline double bits_double(std::uint64_t u) {
    double d;
    std::memcpy(&d, &u, sizeof d);
    return d;
}

// The cells of a numeric column as 64-bit patterns (an int64, a uint64 or a
// double by the column's class) and whether each is present.
struct NumCells {
    std::vector<std::uint64_t> bits;
    std::vector<std::uint8_t> ok;
};

inline NumCells read_cells(const Series& col) {
    const Series s = col.is_flat() ? col.share() : col.materialize();
    const std::int64_t n = s.length();
    NumCells c;
    c.bits.resize(static_cast<std::size_t>(n));
    c.ok.assign(static_cast<std::size_t>(n), 1);
    auto fill = [&](auto at) {
        for (std::int64_t i = 0; i < n; ++i)
            c.bits[static_cast<std::size_t>(i)] = at(i);
    };
    switch (s.type()) {
        case TypeId::Int8:
            fill([&](std::int64_t i) {
                return static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(s.data<std::int8_t>()[i]));
            });
            break;
        case TypeId::Int16:
            fill([&](std::int64_t i) {
                return static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(s.data<std::int16_t>()[i]));
            });
            break;
        case TypeId::Int32:
            fill([&](std::int64_t i) {
                return static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(s.data<std::int32_t>()[i]));
            });
            break;
        case TypeId::Int64:
            fill([&](std::int64_t i) {
                return static_cast<std::uint64_t>(s.data<std::int64_t>()[i]);
            });
            break;
        case TypeId::Uint8:
            fill([&](std::int64_t i) {
                return std::uint64_t{s.data<std::uint8_t>()[i]};
            });
            break;
        case TypeId::Uint16:
            fill([&](std::int64_t i) {
                return std::uint64_t{s.data<std::uint16_t>()[i]};
            });
            break;
        case TypeId::Uint32:
            fill([&](std::int64_t i) {
                return std::uint64_t{s.data<std::uint32_t>()[i]};
            });
            break;
        case TypeId::Uint64:
            fill([&](std::int64_t i) { return s.data<std::uint64_t>()[i]; });
            break;
        case TypeId::Float32:
            fill([&](std::int64_t i) {
                return double_bits(static_cast<double>(s.data<float>()[i]));
            });
            break;
        case TypeId::Float64:
            fill([&](std::int64_t i) {
                return double_bits(s.data<double>()[i]);
            });
            break;
        default:
            throw std::logic_error("group transform: a non-numeric column");
    }
    if (s.null_count() > 0)
        for (std::int64_t i = 0; i < n; ++i)
            if (s.is_null(i)) c.ok[static_cast<std::size_t>(i)] = 0;
    return c;
}

// A column of 64-bit patterns of class `cls`, narrowed or widened to the type
// the plan declares, with a null where `ok` is zero.
inline Series make_numeric(const std::vector<std::uint64_t>& bits,
                           const std::vector<std::uint8_t>& ok, NumClass cls,
                           TypeId out) {
    const auto n = static_cast<std::int64_t>(bits.size());
    bool any_null = false;
    std::vector<std::uint8_t> bitmap((bits.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (ok[i])
            bitmap[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        else
            any_null = true;
    }
    const std::uint8_t* validity = any_null ? bitmap.data() : nullptr;
    auto as = [&](auto tag) {
        using T = decltype(tag);
        std::vector<T> v(bits.size());
        for (std::size_t i = 0; i < bits.size(); ++i) {
            switch (cls) {
                case NumClass::SIGNED:
                    v[i] = static_cast<T>(static_cast<std::int64_t>(bits[i]));
                    break;
                case NumClass::UNSIGNED:
                    v[i] = static_cast<T>(bits[i]);
                    break;
                case NumClass::FLOAT:
                    v[i] = static_cast<T>(bits_double(bits[i]));
                    break;
            }
        }
        return Series::flat(out, v.data(), n, validity);
    };
    switch (out) {
        case TypeId::Int8:
            return as(std::int8_t{});
        case TypeId::Int16:
            return as(std::int16_t{});
        case TypeId::Int32:
            return as(std::int32_t{});
        case TypeId::Int64:
            return as(std::int64_t{});
        case TypeId::Uint8:
            return as(std::uint8_t{});
        case TypeId::Uint16:
            return as(std::uint16_t{});
        case TypeId::Uint32:
            return as(std::uint32_t{});
        case TypeId::Uint64:
            return as(std::uint64_t{});
        case TypeId::Float32:
            return as(float{});
        case TypeId::Float64:
            return as(double{});
        default:
            throw std::logic_error("group transform: a non-numeric output");
    }
}

}  // namespace dftracer::utils::dataframe::lazy_internal

#endif  // DFTRACER_UTILS_DATAFRAME_LAZY_NUMERIC_H
