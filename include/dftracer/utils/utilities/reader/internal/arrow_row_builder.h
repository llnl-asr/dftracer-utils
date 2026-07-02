#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_ARROW_ROW_BUILDER_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_ARROW_ROW_BUILDER_H

#include <dftracer/utils/core/common/config.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/core/common/string_arena.h>
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <dftracer/utils/utilities/common/json/parser.h>
#include <simdjson.h>

#include <string_view>

namespace dftracer::utils::utilities::reader::internal {

// Build one Arrow row from a parsed JSON row. When `normalize` is true, the
// row is mapped into the semantic output schema (see normalize_row); otherwise
// every field is passed through with its native type. Returns false when the
// row should be skipped.
bool build_arrow_row(common::arrow::RecordBatchBuilder &builder,
                     common::json::JsonParser &parser, StringArena &arena,
                     bool normalize);

// Flatten a simdjson object into "prefix.key" columns using native types.
// On type mismatch (same key, different type across rows), appends null.
void flatten_object_into(common::arrow::RecordBatchBuilder &builder,
                         StringArena &arena, std::string_view prefix,
                         simdjson::ondemand::object obj);

bool process_json_line(common::arrow::RecordBatchBuilder &builder,
                       common::json::JsonParser &parser, StringArena &arena,
                       std::string_view content, bool normalize);

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_ENABLE_ARROW

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_ARROW_ROW_BUILDER_H
