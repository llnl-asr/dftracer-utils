#ifndef DFTRACER_UTILS_DUQL_ERRC_H
#define DFTRACER_UTILS_DUQL_ERRC_H

#include <dftracer/utils/core/common/error.h>

#include <cstdint>

namespace dftracer::utils::duql {

/// The query library's error domain (domain #1 for the extensible error id).
inline constexpr dftracer::utils::ErrorDomain ERROR_DOMAIN =
    dftracer::utils::make_error_domain("dftracer.duql");

/// duql error codes; portable conditions come from error_condition.
enum class DuqlErrc : std::int32_t {
    Parse,        ///< syntax / tokenize failure
    Pattern,      ///< invalid match pattern (like / regex)
    Unsupported,  ///< predicate has no columnar (vec-mask) lowering
};

/// ADL opt-in: binds each code to the query domain and a portable condition, so
/// dftracer::utils::make_error(DuqlErrc::...) deduces both.
constexpr dftracer::utils::ErrorDomain error_domain(DuqlErrc) noexcept {
    return ERROR_DOMAIN;
}
constexpr dftracer::utils::Condition error_condition(DuqlErrc e) noexcept {
    switch (e) {
        case DuqlErrc::Parse:
            return dftracer::utils::Condition::Parse;
        case DuqlErrc::Pattern:
            return dftracer::utils::Condition::InvalidArgument;
        case DuqlErrc::Unsupported:
            return dftracer::utils::Condition::Unsupported;
    }
    return dftracer::utils::Condition::Unknown;
}

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_ERRC_H
