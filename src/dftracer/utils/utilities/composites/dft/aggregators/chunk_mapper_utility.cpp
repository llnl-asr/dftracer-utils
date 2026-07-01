#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/chunk_mapper_utility.h>

#include <algorithm>

namespace dftracer::utils::utilities::composites::dft::aggregators {

namespace {
// Build a ChunkAggregatorInput for one chunk; all fields but the byte/line
// range and chunk index come from the file mapper input.
ChunkAggregatorInput make_chunk_input(const FileChunkMapperInput& input,
                                      int chunk_index, std::size_t start_byte,
                                      std::size_t end_byte,
                                      std::size_t start_line,
                                      std::size_t end_line) {
    const auto& meta = input.metadata;
    ChunkAggregatorInput chunk;
    chunk.with_file_path(meta.file_path)
        .with_index_path(meta.index_path)
        .with_byte_range(start_byte, end_byte)
        .with_line_range(start_line, end_line)
        .with_chunk_index(chunk_index)
        .with_config(input.config)
        .with_checkpoint_size(input.checkpoint_size)
        .with_batch_size(input.batch_size);
    chunk.query = input.query;
    return chunk;
}
}  // namespace

coro::CoroTask<FileChunkMapperOutput> FileChunkMapperUtility::process(
    const FileChunkMapperInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("map file chunks");
    const auto& meta = input.metadata;
    if (!meta.success) {
        DFTRACER_UTILS_LOG_WARN("Skipping unsuccessful file: %s",
                                meta.file_path.c_str());
        co_return {};
    }

    std::size_t target_chunk_bytes = input.target_chunk_size_mb * 1024 * 1024;
    std::size_t file_size = meta.uncompressed_size;
    std::size_t num_lines = meta.valid_events;

    // When file size or line count is unknown (e.g. gzip without index),
    // treat the whole file as a single chunk and let the reader handle bounds.
    if (file_size == 0 || num_lines == 0) {
        DFTRACER_UTILS_LOG_DEBUG(
            "File %s has unknown size/lines, using single chunk",
            meta.file_path.c_str());

        FileChunkMapperOutput chunks;
        chunks.push_back(
            make_chunk_input(input, input.start_chunk_index, 0, 0, 0, 0));
        co_return chunks;
    }

    if (target_chunk_bytes == 0) target_chunk_bytes = 1;
    std::size_t num_chunks =
        (file_size + target_chunk_bytes - 1) / target_chunk_bytes;
    if (num_chunks == 0) num_chunks = 1;

    DFTRACER_UTILS_LOG_DEBUG(
        "Splitting file %s (%zu bytes, %zu lines) into %zu chunks",
        meta.file_path.c_str(), file_size, num_lines, num_chunks);

    FileChunkMapperOutput chunks;
    chunks.reserve(num_chunks);

    for (std::size_t i = 0; i < num_chunks; i++) {
        std::size_t start_byte = i * target_chunk_bytes;
        std::size_t end_byte =
            std::min((i + 1) * target_chunk_bytes, file_size);

        std::size_t start_line = (num_lines * start_byte) / file_size;
        std::size_t end_line = (num_lines * end_byte) / file_size;

        if (start_line == 0) start_line = 1;
        if (end_line == 0) end_line = num_lines;

        chunks.push_back(make_chunk_input(
            input, input.start_chunk_index + static_cast<int>(i), start_byte,
            end_byte, start_line, end_line));
    }

    co_return chunks;
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
