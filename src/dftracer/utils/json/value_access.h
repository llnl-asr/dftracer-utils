// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_JSON_VALUE_ACCESS_H
#define DFTRACER_UTILS_JSON_VALUE_ACCESS_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string_arena.h>
#include <dftracer/utils/json/value.h>
#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dftracer::utils::json::detail {

/// Private access to @ref Value and @ref Document for the json library and for
/// duql::Query::matches.
struct EventView {
    simdjson::ondemand::document* doc = nullptr;
    dstd::StringView text;
    dstd::StringArena* strings = nullptr;
};

/// The members of an object for one forward walk with simdjson's own iteration.
struct ObjectWalk {
    simdjson::ondemand::object object;
    /// One past the last byte of the line, the limit for @ref value_extent.
    const char* end = nullptr;
    /// The string arena of the owning @ref Document, for @ref read_key and
    /// @ref read_string.
    dstd::StringArena* strings = nullptr;
};

/// The root value for one forward read through simdjson's own types.
struct ValueWalk {
    simdjson::ondemand::value value;
    /// One past the last byte of the line, the limit for @ref value_extent.
    const char* end = nullptr;
    /// The string arena of the owning @ref Document, for @ref read_key and
    /// @ref read_string.
    dstd::StringArena* strings = nullptr;
};

struct ValueAccess {
    static Value make(DocumentState* owner, simdjson::ondemand::value v,
                      bool root);
    static simdjson::ondemand::value& inner(Value& v);
    static const simdjson::ondemand::value& inner(const Value& v);

    /// The on-demand document that holds `v` and its text, for an evaluator.
    /// `doc` is the document of the whole line for the root value, which the
    /// call leaves restartable, or a document of the text of `v` for a child
    /// value. `strings` is the string arena of the owning @ref Document. False
    /// when `v` has no document or its text is not JSON. The document lives
    /// until the next call for the same Document or the next
    /// @ref Document::parse.
    static bool event(const Value& v, EventView& out);

    /// Whether a read of this value's line hit a syntax error. After that every
    /// read of the line is empty until the next @ref Document::parse.
    static bool broken(const Value& v);
    static void mark_broken(const Value& v);

    /// Starts a forward walk of the members of the object `v`, restarting the
    /// root as any read of `v` would. False when `v` is not an object, its line
    /// is broken or a walk is running; an error in the object marks the line
    /// broken. After the call `v` is spent. The caller reads the members
    /// through `out`, reports a read error with @ref mark_broken, and asks
    /// @ref end_object whether the line is still readable.
    static bool begin_object(Value& v, ObjectWalk& out);

    /// Starts a read of the root value `v`, restarting it as any read would,
    /// whatever its type. False when `v` is not readable (broken line or a walk
    /// is running). After the call `v` is spent; a second call restarts it
    /// again, without a parse. The caller reports a read error with
    /// @ref mark_broken and asks @ref end_object whether the line is still
    /// readable.
    static bool begin_value(Value& v, ValueWalk& out);

    /// Whether the line is readable after a walk that began with
    /// @ref begin_object, which a skip over a malformed part can make false
    /// without any read reporting an error.
    static bool end_object(const Value& v);

    /// Makes the root value readable from its start again, as a new parse
    /// would, without parsing. Does nothing for a child value.
    static void rewind(Value& v);
};

/// The text of the JSON value that starts at `start`, without surrounding
/// whitespace, found by scanning and never past `end`.
dstd::StringView value_extent(const char* start, const char* end);

/// `text`, the inside of a JSON string, with escapes decoded: a view of `text`
/// when it has no backslash, else a copy in `arena`, which never moves a copy
/// made earlier. A view of `text` itself, undecoded, on an invalid escape.
dstd::Optional<dstd::StringView> decode_string(dstd::StringView text,
                                               dstd::StringArena& arena);

/// The double that `token`, a JSON number that simdjson refused, reads as: a
/// signed infinity when its magnitude is too large for a double, a signed zero
/// when it is too small. Empty when `token` is not a JSON number. Ask only
/// after simdjson reports an error, so a valid line costs nothing.
dstd::Optional<double> refused_number(dstd::StringView token);

/// Whether simdjson refuses a line at parse time for a byte that
/// @ref repair_line can replace: bad UTF-8 or a raw control byte in a string.
inline bool is_repairable(simdjson::error_code code) {
    return code == simdjson::UTF8_ERROR || code == simdjson::UNESCAPED_CHARS;
}

/// Replaces, in place and without changing the length, each invalid UTF-8 byte
/// and each raw control byte inside a string of the JSON text `data` with `?`.
/// Bytes outside strings stay, so a line that is broken there stays broken.
/// Call it on a private copy, only after parsing `data` failed with
/// @ref is_repairable.
void repair_line(char* data, std::size_t size);

/// Whether `token` is a JSON integer: no fraction or exponent. Ask for a token
/// simdjson refused as a number, where it means an integer wider than 64 bits
/// (simdjson reports the 20-digit ones above uint64 as NUMBER_ERROR, not
/// BIGINT_ERROR).
bool is_integer_number(dstd::StringView token);

/// `token`, the text up to the next structural byte, without the whitespace
/// that follows the value. Needs no scan, unlike @ref value_extent.
inline dstd::StringView trim_token(dstd::StringView token) {
    std::size_t size = token.size();
    while (size > 0) {
        const char c = token[size - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        --size;
    }
    return token.substr(0, size);
}

/// The decoded string whose opening quote is the first byte of `token`, a
/// view of the line or a copy in `arena`; the raw text between the quotes on an
/// invalid escape. Empty when it is not a string.
dstd::Optional<dstd::StringView> read_string(dstd::StringView token,
                                             dstd::StringArena& arena);

/// The decoded name of `key`, the raw key of an object member, never past
/// `end`. It touches no simdjson string buffer, so it is safe to call any
/// number of times and after a rewind.
dstd::Optional<dstd::StringView> read_key(
    simdjson::ondemand::raw_json_string key, const char* end,
    dstd::StringArena& arena);

/// Whether `text` holds a backslash, eight bytes at a time. A name is short, so
/// this beats a call to memchr.
inline bool has_backslash(dstd::StringView text) {
    constexpr std::uint64_t ONES = 0x0101010101010101ULL;
    constexpr std::uint64_t HIGH = 0x8080808080808080ULL;
    const char* p = text.data();
    std::size_t left = text.size();
    for (; left >= 8; p += 8, left -= 8) {
        std::uint64_t word;
        std::memcpy(&word, p, 8);
        word ^= ONES * static_cast<std::uint64_t>('\\');
        if ((word - ONES) & ~word & HIGH) return true;
    }
    for (; left > 0; ++p, --left)
        if (*p == '\\') return true;
    return false;
}

/// The decoded name of the member `field` of an object, found without scanning
/// the key. A view of the line when the name has no backslash, else a copy in
/// `arena`. The raw name on an invalid escape.
inline dstd::Optional<dstd::StringView> read_key(
    const simdjson::ondemand::field& field, dstd::StringArena& arena) {
    const dstd::StringView raw = field.escaped_key();
    if (!has_backslash(raw)) return raw;
    return decode_string(raw, arena);
}

/// The raw text between the quotes of the string that starts at `value`, for a
/// string whose escape simdjson rejected. Empty when `value` is no string.
dstd::Optional<dstd::StringView> raw_string(simdjson::ondemand::value& value);

/// The raw key text that starts at `begin`, the byte after the opening quote.
/// The closing quote is there because the document parsed.
dstd::StringView raw_key(const char* begin);

/// The string `value` as @ref simdjson::ondemand::value::get_string reads it,
/// or its raw text when simdjson rejects an escape in it. Empty when `value` is
/// no string.
inline dstd::Optional<dstd::StringView> get_string_or_raw(
    simdjson::ondemand::value& value) {
    auto result = value.get_string();
    if (!result.error()) return dstd::StringView(result.value_unsafe());
    if (result.error() != simdjson::STRING_ERROR) return dstd::nullopt;
    return raw_string(value);
}

/// The double `value` as @ref simdjson::ondemand::value::get_double reads it,
/// or the @ref refused_number of its text when simdjson refuses the magnitude.
inline dstd::Optional<double> get_double_or_refused(
    simdjson::ondemand::value& value) {
    auto result = value.get_double();
    if (!result.error()) return result.value_unsafe();
    if (result.error() != simdjson::NUMBER_ERROR) return dstd::nullopt;
    return refused_number(trim_token(value.raw_json_token()));
}

/// The name of `field`, a simdjson field or its result, as
/// @ref simdjson::ondemand::field::unescaped_key reads it, or its raw text when
/// simdjson rejects an escape in it.
template <class Field>
inline dstd::Optional<dstd::StringView> get_key_or_raw(Field&& field) {
    auto raw = field.key().raw();
    const char* begin = nullptr;
    if constexpr (requires { raw.error(); }) {
        if (raw.error()) return dstd::nullopt;
        begin = raw.value_unsafe();
    } else {
        begin = raw;
    }
    auto result = field.unescaped_key();
    if (!result.error()) return dstd::StringView(result.value_unsafe());
    if (result.error() != simdjson::STRING_ERROR) return dstd::nullopt;
    return raw_key(begin);
}

}  // namespace dftracer::utils::json::detail

#endif  // DFTRACER_UTILS_JSON_VALUE_ACCESS_H
