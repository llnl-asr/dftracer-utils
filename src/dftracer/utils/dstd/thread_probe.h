// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_THREAD_PROBE_H
#define DFTRACER_UTILS_DSTD_THREAD_PROBE_H

#if defined(DFTRACER_UTILS_TEST_PROBES) && DFTRACER_UTILS_TEST_PROBES
namespace dftracer::utils::dstd::detail {

/// The next `count` thread creations fail with EAGAIN, as they do at the
/// RLIMIT_NPROC or cgroup pids.max limit. Zero turns it off.
void fail_thread_creates(int count) noexcept;

}  // namespace dftracer::utils::dstd::detail
#endif

#endif  // DFTRACER_UTILS_DSTD_THREAD_PROBE_H
