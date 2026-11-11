// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DOMAINS_WORKLOAD_DLIO_YAML_EMIT_H
#define DFTRACER_UTILS_DOMAINS_WORKLOAD_DLIO_YAML_EMIT_H

#include <dftracer/utils/domains/workload/dlio/mixture.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::domains::workload::dlio {

/// Parameters needed to emit one timing block in the DLIO config.
struct DlioTimingBlock {
    BestModel model;
    double max_bound = 0.0;  ///< seconds
};

/// Renders the DLIO config YAML:
///
///   train:
///     computation_time:
///       type: {distribution}
///       ...
///       max_bound: {seconds}
///   reader:
///     preprocess_time:
///       type: {distribution}
///       ...
///       max_bound: {seconds}
///
/// Either or both blocks can be omitted by passing `nullptr`. Returns an
/// Internal error when yaml-cpp cannot build or emit the document.
dstd::Result<dstd::String> render_dlio_yaml(const DlioTimingBlock* computation,
                                            const DlioTimingBlock* preprocess);

}  // namespace dftracer::utils::domains::workload::dlio

#endif
