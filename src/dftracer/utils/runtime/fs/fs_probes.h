// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_FS_FS_PROBES_H
#define DFTRACER_UTILS_RUNTIME_FS_FS_PROBES_H

#include <dftracer/utils/common/test_probes.h>

#include <cstddef>

namespace dftracer::utils::runtime::fs {

#if DFTRACER_UTILS_TEST_PROBES
/// The stat batches @ref find has run in this process. For tests.
std::size_t stat_batch_count() noexcept;
/// The threads that have run a stat batch since the last
/// @ref reset_stat_threads. For tests.
std::size_t stat_thread_count() noexcept;
/// Whether the calling thread has run a stat batch since the last
/// @ref reset_stat_threads. For tests.
bool has_run_stat_batch() noexcept;
void reset_stat_threads() noexcept;
#endif

}  // namespace dftracer::utils::runtime::fs

#endif  // DFTRACER_UTILS_RUNTIME_FS_FS_PROBES_H
