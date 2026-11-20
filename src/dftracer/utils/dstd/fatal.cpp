// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fatal.h>

#include <cstdio>
#include <cstdlib>

#if __has_include(<execinfo.h>)
#include <execinfo.h>
#include <unistd.h>
#define DFTRACER_UTILS_DSTD_HAS_BACKTRACE 1
#endif

namespace dftracer::utils::dstd {

namespace {
constexpr int FRAME_LIMIT = 64;
}  // namespace

void fatal(StringView what, StringView detail) noexcept {
    const int what_size = static_cast<int>(what.size());
    if (!detail.empty()) {
        std::fprintf(stderr, "dftracer-utils fatal: %.*s: %.*s\n", what_size,
                     what.data(), static_cast<int>(detail.size()),
                     detail.data());
    } else {
        std::fprintf(stderr, "dftracer-utils fatal: %.*s\n", what_size,
                     what.data());
    }
    std::fflush(stderr);
#ifdef DFTRACER_UTILS_DSTD_HAS_BACKTRACE
    void* frames[FRAME_LIMIT];
    const int count = ::backtrace(frames, FRAME_LIMIT);
    ::backtrace_symbols_fd(frames, count, STDERR_FILENO);
#endif
    std::abort();
}

}  // namespace dftracer::utils::dstd
