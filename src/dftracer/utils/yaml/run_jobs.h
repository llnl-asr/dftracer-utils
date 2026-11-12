// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_RUN_JOBS_H
#define DFTRACER_UTILS_YAML_RUN_JOBS_H

#include <dftracer/utils/dstd/function_ref.h>

#include <cstddef>

namespace dftracer::utils::yaml {

/// Runs `body(begin, end)` over ranges that cover `[0, n)` once each, on the
/// caller's threads, and returns when every range has run. The body is safe
/// to run on disjoint ranges at the same time.
using RunJobs = dstd::FunctionRef<void(
    std::size_t n,
    dstd::FunctionRef<void(std::size_t begin, std::size_t end)> body)>;

}  // namespace dftracer::utils::yaml

#endif  // DFTRACER_UTILS_YAML_RUN_JOBS_H
