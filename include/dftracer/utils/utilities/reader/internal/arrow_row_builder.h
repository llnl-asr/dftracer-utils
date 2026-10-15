#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_ARROW_ROW_BUILDER_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_ARROW_ROW_BUILDER_H

#include <dftracer/utils/common/config.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/common/string_arena.h>
#include <dftracer/utils/json/parser.h>
#include <dftracer/utils/trace/time_metric.h>
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <simdjson.h>

#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::reader::internal {

using trace::TimeScaleState;

// Opt-in tuning for the native (non-normalized) row build. Defaults reproduce
// the historical pass-through behaviour.
struct RowBuildOptions {
    // Top-level keys to keep (nullptr/empty = all). "args" gates whether the
    // args object is flattened at all, so dropping it skips that work entirely.
    const std::vector<std::string> *keep = nullptr;
    // Dictionary-encode string columns (big win for low-cardinality fields like
    // cat/name that repeat across most rows).
    bool dict_strings = false;
    // Multiply ts/dur/te by this to normalize the time unit (1.0 = none).
    double time_scale = 1.0;
};

// Build one Arrow row from a parsed JSON row. When `normalize` is true, the
// row is mapped into the semantic output schema (see normalize_row); otherwise
// every field is passed through with its native type. Returns false when the
// row should be skipped.
bool build_arrow_row(common::arrow::RecordBatchBuilder &builder,
                     json::JsonParser &parser, StringArena &arena,
                     bool normalize, TimeScaleState &time_scale,
                     const RowBuildOptions &opts = {});

// Flatten a simdjson object into "prefix.key" columns using native types.
bool process_json_line(common::arrow::RecordBatchBuilder &builder,
                       json::JsonParser &parser, StringArena &arena,
                       std::string_view content, bool normalize,
                       TimeScaleState &time_scale,
                       const RowBuildOptions &opts = {});

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_ENABLE_ARROW

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_ARROW_ROW_BUILDER_H
