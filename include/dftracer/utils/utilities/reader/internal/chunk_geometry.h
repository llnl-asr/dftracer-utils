#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_CHUNK_GEOMETRY_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_CHUNK_GEOMETRY_H

#include <dftracer/utils/core/common/config.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <cstddef>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::reader::internal {

// Byte-range work unit for checkpoint-level parallelism. Each unit covers
// one or more consecutive checkpoints from a single file. Decompression of
// a single gz file is sequential per gzip stream, so splitting at
// checkpoint-aligned byte offsets is what lets multiple workers share the
// decode work for one file.
struct ArrowWorkItem {
    std::string file_path;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
    bool start_at_checkpoint = false;
    bool end_at_checkpoint = false;
    // When true, every kept chunk for this byte range is uniform-matching
    // (dim_stats min == max == predicate literal for every AND-of-EQ leaf),
    // so per-event predicate eval is skippable.
    bool chunk_prune_only = false;
    // Line-range work items override byte ranges: the worker passes these
    // down as LINE_RANGE on the read, and the gzip stream resolves them to
    // byte offsets via the checkpoint index. 0 = no line constraint.
    std::size_t start_line = 0;
    std::size_t end_line = 0;
};

// Enumerate byte/line-range work items across `files`, pruning against the
// per-file index when a query is supplied. `max_workers` bounds the number of
// work ranges per file. The clip arguments apply a user byte/line window.
std::vector<ArrowWorkItem> enumerate_work_items(
    const std::vector<std::string> &files, const std::string &index_dir,
    const std::string &query_str, std::size_t max_workers,
    std::size_t clip_start_byte = 0, std::size_t clip_end_byte = 0,
    std::size_t clip_start_line = 0, std::size_t clip_end_line = 0);

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_ENABLE_ARROW

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_CHUNK_GEOMETRY_H
