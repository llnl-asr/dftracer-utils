#ifndef DFTRACER_UTILS_BINARIES_JSON_CELL_PRINTER_H
#define DFTRACER_UTILS_BINARIES_JSON_CELL_PRINTER_H

#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/json/json_escape.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// The JSON cell printer for `dftracer_view --format json`. Split out from
// dftracer_view.cpp so its exhaustive TypeId switch can be exercised directly
// by tests without spawning the CLI.
namespace dftracer::utils::binaries {

namespace detail {

// Little-endian two's-complement bytes at `bytes` (16 for Decimal128, 32 for
// Decimal256) as an exact base-10 string, with the decimal point `scale`
// digits from the right. A JSON number cannot hold a precision-38 decimal
// exactly in a double, so the caller quotes this as a JSON string.
inline std::string decimal_to_exact_string(const std::uint8_t* bytes,
                                           std::size_t nbytes,
                                           std::int32_t scale) {
    const bool negative = (bytes[nbytes - 1] & 0x80) != 0;
    std::vector<std::uint64_t> words(nbytes / 8);
    for (std::size_t w = 0; w < words.size(); ++w) {
        std::uint64_t v;
        std::memcpy(&v, bytes + w * 8, 8);
        words[w] = v;
    }
    if (negative) {
        for (auto& w : words) w = ~w;
        std::uint64_t carry = 1;
        for (auto& w : words) {
            const std::uint64_t sum = w + carry;
            carry = (sum < w) ? 1 : 0;
            w = sum;
        }
    }

    std::string digits;
    bool nonzero = true;
    while (nonzero) {
        nonzero = false;
        std::uint64_t remainder = 0;
        for (auto it = words.rbegin(); it != words.rend(); ++it) {
            __extension__ unsigned __int128 cur =
                (static_cast<unsigned __int128>(remainder) << 64) | *it;
            *it = static_cast<std::uint64_t>(cur / 10);
            remainder = static_cast<std::uint64_t>(cur % 10);
            if (*it != 0) nonzero = true;
        }
        digits.push_back(static_cast<char>('0' + remainder));
    }
    std::reverse(digits.begin(), digits.end());

    std::string out;
    if (scale > 0) {
        if (static_cast<std::size_t>(scale) >= digits.size())
            digits.insert(
                0, static_cast<std::size_t>(scale) - digits.size() + 1, '0');
        const std::size_t point =
            digits.size() - static_cast<std::size_t>(scale);
        out = digits.substr(0, point) + "." + digits.substr(point);
    } else {
        out = digits;
    }
    if (negative && digits.find_first_not_of('0') != std::string::npos)
        out.insert(0, "-");
    return out;
}

}  // namespace detail

/// True for a TypeId with a per-row scalar value `append_cell_json` can
/// render. Nested types (List, LargeList, FixedSizeList, Struct, Map) have no
/// such value; no collect()/collect_typed() path builds one of those for a
/// CLI-facing column, so the caller filters them out before reaching
/// `append_cell_json`, which renders them as `null` as a defensive fallback.
inline bool cli_emittable(dataframe::TypeId t) {
    switch (t) {
        case dataframe::TypeId::Unknown:
        case dataframe::TypeId::Bool:
        case dataframe::TypeId::Int8:
        case dataframe::TypeId::Int16:
        case dataframe::TypeId::Int32:
        case dataframe::TypeId::Int64:
        case dataframe::TypeId::Uint8:
        case dataframe::TypeId::Uint16:
        case dataframe::TypeId::Uint32:
        case dataframe::TypeId::Uint64:
        case dataframe::TypeId::Float32:
        case dataframe::TypeId::Float64:
        case dataframe::TypeId::String:
        case dataframe::TypeId::Binary:
        case dataframe::TypeId::Float16:
        case dataframe::TypeId::Date32:
        case dataframe::TypeId::Date64:
        case dataframe::TypeId::Time32:
        case dataframe::TypeId::Time64:
        case dataframe::TypeId::Timestamp:
        case dataframe::TypeId::Duration:
        case dataframe::TypeId::Decimal128:
        case dataframe::TypeId::Decimal256:
        case dataframe::TypeId::FixedSizeBinary:
        case dataframe::TypeId::LargeString:
        case dataframe::TypeId::LargeBinary:
            return true;
        case dataframe::TypeId::List:
        case dataframe::TypeId::Struct:
        case dataframe::TypeId::LargeList:
        case dataframe::TypeId::FixedSizeList:
        case dataframe::TypeId::Map:
            return false;
    }
    return false;
}

/// Append one Batch cell as a JSON value: strings quoted and escaped,
/// numerics bare, decimals quoted (exact, see decimal_to_exact_string), dates
/// and durations as their raw underlying integer, nested types as `null`
/// (see cli_emittable).
///
/// Every TypeId has an explicit case, so -Wswitch flags a type added to the
/// engine without an entry here rather than letting it fall through to a
/// guessing default that reads past the column's buffer.
inline void append_cell_json(std::string& s, const dataframe::Series& c,
                             std::int64_t i) {
    using dataframe::TypeId;
    switch (c.type()) {
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
        case TypeId::FixedSizeBinary: {
            s += '"';
            json::append_json_escaped(s, dataframe::read_bytes(c, i));
            s += '"';
            break;
        }
        case TypeId::Bool:
            s += dataframe::read_i64(c, i) != 0 ? "true" : "false";
            break;
        case TypeId::Int8:
            s += std::to_string(c.data<std::int8_t>()[i]);
            break;
        case TypeId::Int16:
            s += std::to_string(c.data<std::int16_t>()[i]);
            break;
        case TypeId::Int32:
            s += std::to_string(c.data<std::int32_t>()[i]);
            break;
        case TypeId::Int64:
            s += std::to_string(c.data<std::int64_t>()[i]);
            break;
        case TypeId::Uint8:
            s += std::to_string(c.data<std::uint8_t>()[i]);
            break;
        case TypeId::Uint16:
            s += std::to_string(c.data<std::uint16_t>()[i]);
            break;
        case TypeId::Uint32:
            s += std::to_string(c.data<std::uint32_t>()[i]);
            break;
        case TypeId::Uint64:
            s += std::to_string(c.data<std::uint64_t>()[i]);
            break;
        case TypeId::Float16:
        case TypeId::Float32:
        case TypeId::Float64: {
            const double v = dataframe::read_f64(c, i);
            s += (v == static_cast<double>(static_cast<std::int64_t>(v)))
                     ? std::to_string(static_cast<std::int64_t>(v))
                     : std::to_string(v);
            break;
        }
        case TypeId::Date32:
        case TypeId::Date64:
        case TypeId::Time32:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
            s += std::to_string(dataframe::read_i64(c, i));
            break;
        case TypeId::Decimal128:
            s += '"';
            s += detail::decimal_to_exact_string(
                c.data<std::uint8_t>() + static_cast<std::size_t>(i) * 16, 16,
                c.data_type().decimal_scale);
            s += '"';
            break;
        case TypeId::Decimal256:
            s += '"';
            s += detail::decimal_to_exact_string(
                c.data<std::uint8_t>() + static_cast<std::size_t>(i) * 32, 32,
                c.data_type().decimal_scale);
            s += '"';
            break;
        case TypeId::List:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Struct:
        case TypeId::Map:
        case TypeId::Unknown:
            s += "null";
            break;
    }
}

}  // namespace dftracer::utils::binaries

#endif  // DFTRACER_UTILS_BINARIES_JSON_CELL_PRINTER_H
