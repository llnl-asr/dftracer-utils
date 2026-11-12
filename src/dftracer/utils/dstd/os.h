// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_OS_H
#define DFTRACER_UTILS_DSTD_OS_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <limits.h>
#include <sys/stat.h>

#include <cstring>

namespace dftracer::utils::dstd::internal {

/// ENOENT and ENOTDIR are NotFound, EINVAL and ENAMETOOLONG are
/// InvalidArgument, and every other errno is Io.
Condition condition_from_errno(int error) noexcept;

/// "first -> second: text", with the arrow part only when @p second is set.
String make_message(StringView first, StringView second, StringView text);

/// An error with the condition of @p error and its strerror text.
Error errno_error(int error, StringView first, StringView second = {});

fs::FileStat make_file_stat(const struct stat& info) noexcept;

/// A path as a syscall needs it, '\0'-terminated. A PathView promises no
/// terminator, so short paths are copied into one stack buffer and long ones
/// into one dstd::String.
class PathCString {
   public:
    explicit PathCString(StringView path) noexcept {
        if (path.size() < PATH_MAX) {
            if (!path.empty()) std::memcpy(stack_, path.data(), path.size());
            stack_[path.size()] = '\0';
        } else {
            heap_.assign(path);
        }
    }

    const char* c_str() const noexcept {
        return heap_.empty() ? stack_ : heap_.c_str();
    }

   private:
    String heap_;
    char stack_[PATH_MAX];
};

}  // namespace dftracer::utils::dstd::internal

#endif  // DFTRACER_UTILS_DSTD_OS_H
