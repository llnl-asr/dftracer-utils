// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_PHASE_SEED_H
#define DFTRACER_UTILS_PLUGINS_HOST_PHASE_SEED_H

#include <dftracer/utils/common/test_probes.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/plugins/host/port_plan.h>
#include <dftracer/utils/trace/views/view.h>

namespace dftracer::utils::plugins::host {

class PortHub;

#if DFTRACER_UTILS_TEST_PROBES
/// Called after each scan of the host pass, before the files are checked again,
/// with the attempt number from 0. A test changes a file here.
inline void (*g_phase_seed_pass_hook)(int attempt) = nullptr;
#endif

/// Fills and seals every seeded generation of `phases` in `hub` before the
/// plugin scan starts, from the first source that is exact for the input files
/// of `plan`: their indexes, when every file has a current one; else the cache
/// of an earlier pass, when the files' stat and sample hash match it; else a
/// host pass over the files, whose result it then caches. The values cover
/// every non-metadata event of the files, whatever filter `plan` carries. A
/// stale or unknown source is never used. The cache needs a single index root
/// for the files and is skipped for a plan that pins a file.
coro::CoroTask<dstd::Result<SeedSource>> seed_phases(
    trace::views::detail::ScanPlan plan, dstd::Vector<PhaseDecl> phases,
    PortHub* hub);

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_PHASE_SEED_H
