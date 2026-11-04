// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_JSON_VALUE_ACCESS_H
#define DFTRACER_UTILS_JSON_VALUE_ACCESS_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string_arena.h>
#include <dftracer/utils/json/value.h>
#include <simdjson.h>

namespace dftracer::utils::json::detail {

/// Private access to @ref Value and @ref Document for the json library and for
/// duql::Query::matches.
struct EventView {
    simdjson::ondemand::document* doc = nullptr;
    dstd::StringView text;
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

    /// The whole event of the root value as a DOM element, for a consumer
    /// that cannot read on demand. The first call after a parse parses the
    /// line a second time, and later calls share that tree, so it costs one
    /// extra parse per line however many consumers ask. Empty for a child
    /// value or text the DOM parser rejects. The element lives until the next
    /// @ref Document::parse.
    static dstd::Optional<simdjson::dom::element> dom(const Value& v);

    /// Makes the root value readable from its start again, as a new parse
    /// would, without parsing. Does nothing for a child value.
    static void rewind(Value& v);
};

/// The text of the JSON value that starts at `start`, without surrounding
/// whitespace, found by scanning and never past `end`.
dstd::StringView value_extent(const char* start, const char* end);

/// `text`, the inside of a JSON string, with escapes decoded: a view of `text`
/// when it has no backslash, else a copy in `arena`, which never moves a copy
/// made earlier. Empty on an invalid escape.
dstd::Optional<dstd::StringView> decode_string(dstd::StringView text,
                                               dstd::StringArena& arena);

/// The decoded string whose opening quote is the first byte of `token`, a
/// view of the line or a copy in `arena`. Empty when it is not a string.
dstd::Optional<dstd::StringView> read_string(dstd::StringView token,
                                             dstd::StringArena& arena);

/// The decoded name of `key`, the raw key of an object member, never past
/// `end`. It touches no simdjson string buffer, so it is safe to call any
/// number of times and after a rewind.
dstd::Optional<dstd::StringView> read_key(
    simdjson::ondemand::raw_json_string key, const char* end,
    dstd::StringArena& arena);

}  // namespace dftracer::utils::json::detail

#endif  // DFTRACER_UTILS_JSON_VALUE_ACCESS_H
