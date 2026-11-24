// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/process.h>
#include <signal.h>
#include <sys/types.h>

#include <cerrno>

namespace dftracer::utils::dstd {

bool process_is_alive(std::uint64_t pid) noexcept {
    if (pid == 0 || pid > INT32_MAX) return false;
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno != ESRCH;
}

}  // namespace dftracer::utils::dstd
