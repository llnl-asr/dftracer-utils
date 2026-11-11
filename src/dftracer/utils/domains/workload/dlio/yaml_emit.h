// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DOMAINS_WORKLOAD_DLIO_YAML_EMIT_H
#define DFTRACER_UTILS_DOMAINS_WORKLOAD_DLIO_YAML_EMIT_H

#include <dftracer/utils/domains/workload/dlio/mixture.h>
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
/// Either or both blocks can be omitted by passing `nullptr`; with both omitted
/// the text is empty. Every number is a float with a decimal point (`1.0`,
/// `1.0e-05`), in its shortest form that reads back, so a YAML reader takes it
/// for a float; the non-finite ones are `.inf`, `-.inf` and `.nan`, and
/// `n_components` is an integer. The text has no trailing newline.
dstd::String render_dlio_yaml(const DlioTimingBlock* computation,
                              const DlioTimingBlock* preprocess);

}  // namespace dftracer::utils::domains::workload::dlio

#endif
