#ifndef DFTRACER_UTILS_SERVER_JSON_BUILDER_H
#define DFTRACER_UTILS_SERVER_JSON_BUILDER_H

#include <simdjson.h>

namespace dftracer::utils::server {

// Callers must not co_await between acquiring this and reading the result out.
inline simdjson::builder::string_builder& scratch_json_builder() {
    thread_local simdjson::builder::string_builder builder;
    builder.clear();
    return builder;
}

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_JSON_BUILDER_H
