// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_TABLE_TABLE_HEADER_H
#define DFTRACER_UTILS_TRACE_TABLE_TABLE_HEADER_H

#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/value.h>

#include <cstdint>

namespace dftracer::utils::trace::table::detail {

inline constexpr dstd::StringView FORMAT = "dftu.table";
inline constexpr std::int64_t FORMAT_VERSION = 1;

/// How the cells of a field are read from and written to JSON.
enum class Kind : std::uint8_t {
    Int,     ///< signed integer, read as Int64
    UInt,    ///< unsigned integer, read as Uint64
    Float,   ///< Float64; "NaN", "Infinity" and "-Infinity" are strings
    Bool,    ///< Bool
    String,  ///< a JSON string, read as String
    Raw,     ///< any JSON value, kept as its JSON text in a String column
};

struct Field {
    dstd::String name;
    Kind kind = Kind::String;
    unsigned bits = 64;    ///< width of an integer dtype
    bool nulls_ok = true;  ///< false for a numpy integer or bool dtype
};

struct Header {
    dstd::String table;
    std::int64_t nrows = 0;
    dstd::Vector<Field> fields;
};

/// Reads the args object of a header record. Refuses another format, an
/// unknown format_version, a missing nrows or fields, and a dtype the codec
/// does not carry. Ignores unknown keys.
dstd::Result<Header> parse_header(json::Value& args);

}  // namespace dftracer::utils::trace::table::detail

#endif  // DFTRACER_UTILS_TRACE_TABLE_TABLE_HEADER_H
