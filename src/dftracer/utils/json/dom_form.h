// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_JSON_DOM_FORM_H
#define DFTRACER_UTILS_JSON_DOM_FORM_H

#include <dftracer/utils/dstd/string.h>
#include <simdjson.h>

namespace dftracer::utils::json {

/// Writes the JSON line `line` to `out` as a line the DOM parser accepts, for a
/// reader whose DOM parse of `line` failed. It reads `line` on demand and
/// writes what it reads: a number too large for a double becomes the largest
/// finite double of its sign, an integer wider than 64 bits a double, a string
/// with an invalid escape its raw text, a bad UTF-8 or control byte `?`, and a
/// part nested past 128 levels a string of its raw text. Unlike the on-demand
/// readers, it gives a number past double range as the largest finite double
/// (not infinity) and a 20 digit or longer integer as a double (not exact
/// digits). Members keep their order, and a key that repeats stays repeated.
/// `out` has no padding after the text, so parse it with a copy. Returns false,
/// leaving `out` empty, when the line cannot be read in full, such as a cropped
/// one or one with text after the first record, so a reader that counts counts
/// it. A bad atom such as `nul`, `+1` or `.5` reads as null, as in @ref
/// Document. Not for a line the DOM parser accepts, which it would only copy.
/// Uses a thread local document, so it must not be called from a coroutine that
/// suspends inside the call.
bool dom_form(dstd::StringView line, dstd::String& out);

/// The parse of `line` by `parser` after `parser.parse` of it failed, for a
/// reader of the DOM: @ref dom_form of the line, parsed. The error TAPE_ERROR
/// when the line cannot be read in full. The element lasts until the next parse
/// by `parser`.
simdjson::simdjson_result<simdjson::dom::element> parse_refused(
    simdjson::dom::parser& parser, dstd::StringView line);

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_DOM_FORM_H
