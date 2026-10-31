#ifndef DFTRACER_UTILS_COMMON_FIELD_REF_H
#define DFTRACER_UTILS_COMMON_FIELD_REF_H

#include <string_view>

namespace dftracer::utils {

/// The explicit prefix that addresses a value inside the event's "args" object.
inline constexpr std::string_view ARGS_PREFIX = "args.";

/// Strip a leading "args." so a flat arg key is matched by its stored name.
/// Trace args are stored flat: a key that itself contains dots (e.g.
/// "cqe.raw_ns") is one member, not a nested object. Callers resolving a field
/// against the args map use this to reach such a key whether the query wrote it
/// bare ("cqe.raw_ns") or prefixed ("args.cqe.raw_ns"). A field that is exactly
/// "args." (no remainder) is left unchanged.
inline std::string_view strip_args_prefix(std::string_view field) {
    if (field.size() > ARGS_PREFIX.size() &&
        field.substr(0, ARGS_PREFIX.size()) == ARGS_PREFIX)
        return field.substr(ARGS_PREFIX.size());
    return field;
}

/// True when `field` carries the explicit "args." prefix (and a non-empty
/// remainder).
inline bool has_args_prefix(std::string_view field) {
    return strip_args_prefix(field) != field;
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_COMMON_FIELD_REF_H
