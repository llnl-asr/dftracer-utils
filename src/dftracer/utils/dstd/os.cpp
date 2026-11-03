// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/os.h>

#include <cerrno>
#include <cstring>

namespace dftracer::utils::dstd::internal {

Condition condition_from_errno(int error) noexcept {
    if (error == ENOENT || error == ENOTDIR) return Condition::NotFound;
    if (error == EINVAL || error == ENAMETOOLONG) {
        return Condition::InvalidArgument;
    }
    return Condition::Io;
}

String make_message(StringView first, StringView second, StringView text) {
    String message(first);
    if (!second.empty()) {
        message.append(" -> ");
        message.append(second);
    }
    message.append(": ");
    message.append(text);
    return message;
}

Error errno_error(int error, StringView first, StringView second) {
    return Error(condition_from_errno(error),
                 make_message(first, second, std::strerror(error)));
}

}  // namespace dftracer::utils::dstd::internal
