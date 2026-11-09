// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>

namespace dftracer::utils::common {

void throw_error(const dstd::Error& error) { throw DFTUtilsException(error); }

}  // namespace dftracer::utils::common
