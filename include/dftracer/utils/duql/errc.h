#ifndef DFTRACER_UTILS_DUQL_ERRC_H
#define DFTRACER_UTILS_DUQL_ERRC_H

#include <dftracer/utils/dstd/error.h>

#include <cstdint>

namespace dftracer::utils::duql {

/// The query library's error domain.
inline constexpr dstd::ErrorDomain ERROR_DOMAIN =
    dstd::make_error_domain("dftracer.utils.duql");

/// duql error codes; portable conditions come from error_info.
enum class DuqlErrc : std::int32_t {
    Parse,        ///< syntax / tokenize failure
    Pattern,      ///< invalid match pattern (like / regex)
    Unsupported,  ///< predicate has no columnar (vec-mask) lowering
};

inline constexpr dstd::ErrorCodeInfo DUQL_ERROR_INFOS[] = {
    {&ERROR_DOMAIN, static_cast<std::int32_t>(DuqlErrc::Parse),
     dstd::Condition::Parse, "PARSE"},
    {&ERROR_DOMAIN, static_cast<std::int32_t>(DuqlErrc::Pattern),
     dstd::Condition::InvalidArgument, "PATTERN"},
    {&ERROR_DOMAIN, static_cast<std::int32_t>(DuqlErrc::Unsupported),
     dstd::Condition::Unsupported, "UNSUPPORTED"},
};

constexpr const dstd::ErrorCodeInfo& error_info(DuqlErrc code) noexcept {
    const auto at = static_cast<std::size_t>(code);
    return at < std::size(DUQL_ERROR_INFOS) ? DUQL_ERROR_INFOS[at]
                                            : DUQL_ERROR_INFOS[0];
}

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_ERRC_H
