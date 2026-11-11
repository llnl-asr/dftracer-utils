// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_PLAN_PREFILTER_KERNEL_H
#define DFTRACER_UTILS_TRACE_INDEX_PLAN_PREFILTER_KERNEL_H

#include <dftracer/utils/common/cpu_features.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/plan/prefilter.h>

namespace dftracer::utils::trace::index::plan {

/// The per-line code of @ref Prefilter in one build. The two builds are the
/// same source, the second assembled with branch padding against the Intel JCC
/// erratum, so a call through the table costs one indirect call per line.
struct PrefilterKernel {
    bool (*may_match)(const Prefilter::Parts& parts, dstd::StringView line);
    bool (*may_match_per_needle)(const Prefilter::Parts& parts,
                                 dstd::StringView line);
    /// The body of Prefilter::Gate::may_match.
    bool (*gate_may_match)(const Prefilter::Parts& parts,
                           Prefilter::GateState& state, dstd::StringView line);
};

// The kernels of each build. jcc::kernel exists only when the build defines
// DFTRACER_UTILS_HAS_JCC_VARIANT as 1.
namespace plain {
const PrefilterKernel& kernel();
}
namespace jcc {
const PrefilterKernel& kernel();
}

/// The build @p request names on a CPU that has the erratum when
/// @p cpu_affected. Jcc without that build logs a warning and gives plain.
const PrefilterKernel& resolve_prefilter_kernel(common::JccKernels request,
                                                bool cpu_affected);

/// The build this process uses: @ref resolve_prefilter_kernel of
/// @ref common::jcc_kernels_request and the CPU.
const PrefilterKernel& prefilter_kernel();

/// The build of @p kind, or null when it was not built (Jcc) or is Auto.
const PrefilterKernel* prefilter_kernel_variant(common::JccKernels kind);

}  // namespace dftracer::utils::trace::index::plan

#endif  // DFTRACER_UTILS_TRACE_INDEX_PLAN_PREFILTER_KERNEL_H
