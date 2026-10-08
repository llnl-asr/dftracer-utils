#ifndef DFTRACER_UTILS_CORE_COMMON_SPILL_DIR_H
#define DFTRACER_UTILS_CORE_COMMON_SPILL_DIR_H

#include <dftracer/utils/core/common/error.h>

#include <string>

namespace dftracer::utils {

/// The directory every spill file goes to: DFTRACER_UTILS_SPILL_DIR when set;
/// else the node-local disk mount with the most free space that the user can
/// write (local_spill_root(), never RAM-backed or network); else the system
/// temp directory. Created if missing. A set variable that cannot be used is an
/// IO error naming the directory and the variable, with no fallback to another
/// directory.
Result<std::string> spill_dir();

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_SPILL_DIR_H
