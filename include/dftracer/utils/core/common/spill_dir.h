#ifndef DFTRACER_UTILS_CORE_COMMON_SPILL_DIR_H
#define DFTRACER_UTILS_CORE_COMMON_SPILL_DIR_H

#include <dftracer/utils/core/common/error.h>

#include <string>
#include <string_view>
#include <utility>

namespace dftracer::utils {

/// The directory every spill file goes to: DFTRACER_UTILS_SPILL_DIR when set;
/// else the node-local disk mount with the most free space that the user can
/// write (local_spill_root(), never RAM-backed or network); else the system
/// temp directory. Created if missing. A set variable that cannot be used is an
/// IO error naming the directory and the variable, with no fallback to another
/// directory.
Result<std::string> spill_dir();

/// A directory under spill_dir() that one owner fills with its spill files,
/// named `<prefix>_<host>_<pid>_<n>` and removed with everything in it on
/// destruction. The first one a process makes for a prefix also removes the
/// directories of that prefix left by processes that are gone (a crash or a
/// kill never runs a destructor), so they do not pile up.
class ScopedSpillSubdir {
   public:
    static Result<ScopedSpillSubdir> create(std::string_view prefix);

    ScopedSpillSubdir(ScopedSpillSubdir&& other) noexcept;
    ScopedSpillSubdir& operator=(ScopedSpillSubdir&& other) noexcept;
    ScopedSpillSubdir(const ScopedSpillSubdir&) = delete;
    ScopedSpillSubdir& operator=(const ScopedSpillSubdir&) = delete;
    ~ScopedSpillSubdir();

    const std::string& path() const noexcept { return path_; }

   private:
    explicit ScopedSpillSubdir(std::string path) : path_(std::move(path)) {}

    std::string path_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_SPILL_DIR_H
