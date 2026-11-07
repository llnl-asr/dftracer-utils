// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_JSON_VALUE_PATH_H
#define DFTRACER_UTILS_JSON_VALUE_PATH_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/json/value.h>

#include <cstddef>

namespace dftracer::utils::json {

/// Calls `fn(value)` on the value at the dotted `path` under `cur`, read on
/// demand, and returns its result; false when the path is absent. A segment
/// is an object key, an array index `[n]`, or a number that indexes an array.
/// The value passed to `fn` is valid only inside that call.
template <typename Fn>
bool at_path(Value& cur, dstd::StringView path, Fn&& fn) {
    while (!path.empty() && path.front() == '.') path.remove_prefix(1);
    if (path.empty()) return fn(cur);
    const Value::Type type = cur.type();
    dstd::Optional<std::size_t> want;
    if (path.front() == '[') {
        const std::size_t close = path.find(']');
        const dstd::StringView digits = path.substr(
            1, close == dstd::StringView::npos ? dstd::StringView::npos
                                               : close - 1);
        path.remove_prefix(close == dstd::StringView::npos ? path.size()
                                                           : close + 1);
        want = dstd::from_chars<std::size_t>(digits);
        if (!want) return false;
    } else {
        const std::size_t end = path.find_first_of(".[");
        const dstd::StringView key = path.substr(0, end);
        path.remove_prefix(end == dstd::StringView::npos ? path.size() : end);
        if (type == Value::Type::Object) {
            auto next = cur.find(key);
            return next && at_path(*next, path, fn);
        }
        want = dstd::from_chars<std::size_t>(key);
        if (!want) return false;
    }
    if (type != Value::Type::Array) return false;
    bool found = false;
    std::size_t index = 0;
    cur.for_each_element([&](Value element) {
        if (index++ == *want) found = at_path(element, path, fn);
    });
    return found;
}

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_VALUE_PATH_H
