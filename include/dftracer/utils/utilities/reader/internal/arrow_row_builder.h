#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_ARROW_ROW_BUILDER_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_ARROW_ROW_BUILDER_H

#include <dftracer/utils/core/common/config.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <dftracer/utils/utilities/common/json/parser.h>
#include <simdjson.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::reader::internal {

// Bump arena for string_views that must survive until builder.finish().
struct StringArena {
    static constexpr std::size_t BLOCK_SIZE = 64 * 1024;
    std::vector<std::vector<char>> blocks;
    std::size_t pos = 0;

    StringArena() { blocks.emplace_back(BLOCK_SIZE); }

    std::string_view push(const char *data, std::size_t len) {
        if (pos + len > blocks.back().size()) {
            blocks.emplace_back(std::max(BLOCK_SIZE, len));
            pos = 0;
        }
        char *dst = blocks.back().data() + pos;
        std::memcpy(dst, data, len);
        pos += len;
        return {dst, len};
    }

    void clear() {
        if (blocks.size() > 1) blocks.resize(1);
        pos = 0;
    }
};

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
