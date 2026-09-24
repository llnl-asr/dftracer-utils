#ifndef DFTRACER_UTILS_JSON_CANONICAL_H
#define DFTRACER_UTILS_JSON_CANONICAL_H

#include <simdjson.h>

#include <string>
#include <string_view>

namespace dftracer::utils::json {

/// Appends `v` to `out` as canonical JSON: no whitespace, object keys in byte
/// order (repeated keys keep their order), integers exact and doubles in
/// their shortest round-trip form, so an integral double reads like the
/// integer. Two values with the same content give the same text.
void append_canonical_json(std::string& out, simdjson::dom::element v);

/// The canonical form of the JSON text `text`, or `text` itself when it does
/// not parse.
std::string canonical_json_text(std::string_view text);

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_CANONICAL_H
