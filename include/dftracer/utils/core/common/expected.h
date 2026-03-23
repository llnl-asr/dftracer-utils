#ifndef DFTRACER_UTILS_CORE_COMMON_EXPECTED_H
#define DFTRACER_UTILS_CORE_COMMON_EXPECTED_H

#if __cplusplus >= 202302L && __has_include(<expected>)

#include <expected>

namespace dftracer::utils {

using std::expected;
using std::unexpected;

}  // namespace dftracer::utils

#else

#include <tl/expected.hpp>

namespace dftracer::utils {

using tl::expected;
using tl::unexpected;

}  // namespace dftracer::utils

#endif

#endif  // DFTRACER_UTILS_CORE_COMMON_EXPECTED_H
