#ifndef DFTRACER_UTILS_UTILITIES_DLIO_YAML_EMIT_H
#define DFTRACER_UTILS_UTILITIES_DLIO_YAML_EMIT_H

#include <dftracer/utils/utilities/common/statistics/mixture.h>

#include <iosfwd>
#include <string>

namespace dftracer::utils::utilities::dlio {

using BestModel = ::dftracer::utils::utilities::common::statistics::BestModel;

// Parameters needed to emit one timing block in the DLIO config.
struct DlioTimingBlock {
    BestModel model;
    double max_bound = 0.0;  // seconds
};

// Renders the DLIO config YAML:
//
//   train:
//     computation_time:
//       type: <distribution>
//       ...
//       max_bound: <seconds>
//   reader:
//     preprocess_time:
//       type: <distribution>
//       ...
//       max_bound: <seconds>
//
// Either or both blocks can be omitted by passing `nullptr`.
std::string render_dlio_yaml(const DlioTimingBlock* computation,
                             const DlioTimingBlock* preprocess);

// Writes the YAML to `out` (e.g. an ofstream). Returns true on success.
bool write_dlio_yaml(std::ostream& out, const DlioTimingBlock* computation,
                     const DlioTimingBlock* preprocess);

}  // namespace dftracer::utils::utilities::dlio

#endif
