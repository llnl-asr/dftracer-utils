#ifndef DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_DOC_GUARD_H
#define DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_DOC_GUARD_H

#include <yyjson.h>

namespace dftracer::utils::utilities::common::json {

/// RAII guard for yyjson_doc to prevent leaks on exceptions or
/// early co_return from coroutines.
struct JsonDocGuard {
    yyjson_doc* doc = nullptr;

    explicit JsonDocGuard(yyjson_doc* d) : doc(d) {}
    ~JsonDocGuard() {
        if (doc) yyjson_doc_free(doc);
    }

    JsonDocGuard(const JsonDocGuard&) = delete;
    JsonDocGuard& operator=(const JsonDocGuard&) = delete;
    JsonDocGuard(JsonDocGuard&& other) noexcept : doc(other.doc) {
        other.doc = nullptr;
    }
    JsonDocGuard& operator=(JsonDocGuard&& other) noexcept {
        if (this != &other) {
            if (doc) yyjson_doc_free(doc);
            doc = other.doc;
            other.doc = nullptr;
        }
        return *this;
    }

    explicit operator bool() const { return doc != nullptr; }
};

}  // namespace dftracer::utils::utilities::common::json

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_DOC_GUARD_H
