#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_SCALAR_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_SCALAR_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/types.h>

namespace dftracer::utils::dataframe {

/// Read a dftu_scalar as the column type T (converting from its tagged
/// domain). The inverse of to_scalar; used inside the kernels. A STR or ERR
/// scalar has no numeric reading (its union member is a pointer), so it reads
/// as T{}; an unknown tag is read as F64 (its widest domain).
template <class T>
inline T scalar_as(const dftu_scalar& s) {
    if (s.kind == static_cast<std::int32_t>(ScalarTag::Str) ||
        s.kind == static_cast<std::int32_t>(ScalarTag::Err))
        return T{};
    if (s.kind < static_cast<std::int32_t>(ScalarTag::I64) ||
        s.kind > static_cast<std::int32_t>(ScalarTag::F64))
        return static_cast<T>(s.value.d);
    switch (static_cast<ScalarTag>(s.kind)) {
        case ScalarTag::I64:
            return static_cast<T>(s.value.i);
        case ScalarTag::U64:
            return static_cast<T>(s.value.u);
        case ScalarTag::F64:
            return static_cast<T>(s.value.d);
        case ScalarTag::Str:
        case ScalarTag::Err:
            return T{};
    }
    return static_cast<T>(s.value.d);
}

/// A reducer that refuses its input returns an ERR scalar (see
/// dftu_series_reduce). This returns `s` unchanged when it is a value and
/// throws DFTUtilsException (INVALID_ARGUMENT) with the refusal's message when
/// it is an ERR scalar, so a C++ caller never reads a refusal as a number. It
/// lives here, not in the public scalar.h, because error.h pulls in a
/// dependency (tl/expected) the plugin SDK include directory does not ship.
inline dftu_scalar raise_if_error(dftu_scalar s) {
    if (s.kind == DFTU_SCALAR_TAG_ERR) {
        const char* msg = s.value.err != nullptr && s.value.err->message
                              ? s.value.err->message
                              : "reduction failed";
        throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT, msg);
    }
    return s;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_SCALAR_H
