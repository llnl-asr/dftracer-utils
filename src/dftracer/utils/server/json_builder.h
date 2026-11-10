// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_SERVER_JSON_BUILDER_H
#define DFTRACER_UTILS_SERVER_JSON_BUILDER_H

#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/string.h>
#include <simdjson.h>

namespace dftracer::utils::server {

/// Callers must not co_await between acquiring this and reading the result out.
inline simdjson::builder::string_builder& scratch_json_builder() {
    thread_local simdjson::builder::string_builder builder;
    builder.clear();
    return builder;
}

/// The text written to `builder`. The builder fails only when its buffer cannot
/// grow, which ends the process.
inline dstd::StringView json_view(
    const simdjson::builder::string_builder& builder) {
    dstd::StringView text;
    if (builder.view().get(text))
        dstd::fatal("viz: json builder out of memory");
    return text;
}

inline dstd::String json_string(
    const simdjson::builder::string_builder& builder) {
    return dstd::String(json_view(builder));
}

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_JSON_BUILDER_H
