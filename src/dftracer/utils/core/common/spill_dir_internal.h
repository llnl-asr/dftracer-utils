#ifndef DFTRACER_UTILS_CORE_COMMON_SPILL_DIR_INTERNAL_H
#define DFTRACER_UTILS_CORE_COMMON_SPILL_DIR_INTERNAL_H

#include <dftracer/utils/core/common/error.h>

#include <string>
#include <vector>

namespace dftracer::utils::detail {

/// spill_dir() with its inputs given: `env` is DFTRACER_UTILS_SPILL_DIR (empty
/// when unset), `local_root` is local_spill_root(), and `fallbacks` are tried
/// in order when both are empty. A fallback on a RAM-backed file system is
/// never used; a network one is.
Result<std::string> choose_spill_dir(const std::string& env,
                                     const std::string& local_root,
                                     const std::vector<std::string>& fallbacks);

}  // namespace dftracer::utils::detail

#endif  // DFTRACER_UTILS_CORE_COMMON_SPILL_DIR_INTERNAL_H
