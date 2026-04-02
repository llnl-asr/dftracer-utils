#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/chunk_mapper_utility.h>

#include <algorithm>

namespace dftracer::utils::utilities::composites::dft::aggregators {

coro::CoroTask<FileChunkMapperOutput> FileChunkMapperUtility::process(
    const FileChunkMapperInput& input) {
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
        ChunkAggregatorInput chunk;
        chunk.with_file_path(meta.file_path)
            .with_idx_path(meta.idx_path)
            .with_byte_range(0, 0)
            .with_line_range(0, 0)
            .with_chunk_index(input.start_chunk_index)
            .with_config(input.config)
            .with_checkpoint_size(input.checkpoint_size)
            .with_batch_size(input.batch_size);
        chunk.query = input.query;
        chunks.push_back(std::move(chunk));
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

        ChunkAggregatorInput chunk;
        chunk.with_file_path(meta.file_path)
            .with_idx_path(meta.idx_path)
            .with_byte_range(start_byte, end_byte)
            .with_line_range(start_line, end_line)
            .with_chunk_index(input.start_chunk_index + static_cast<int>(i))
            .with_config(input.config)
            .with_checkpoint_size(input.checkpoint_size)
            .with_batch_size(input.batch_size);
        chunk.query = input.query;

        chunks.push_back(std::move(chunk));
    }

    co_return chunks;
}

coro::CoroTask<ChunkMapperOutput> ChunkMapperUtility::process(
    const ChunkMapperInput& input) {
    ChunkMapperOutput all_chunks;
    int global_chunk_index = 0;

    std::size_t target_chunk_bytes = input.target_chunk_size_mb * 1024 * 1024;

    DFTRACER_UTILS_LOG_INFO(
        "Creating chunk mappings with target size: %zu MB (%zu bytes)",
        input.target_chunk_size_mb, target_chunk_bytes);

    FileChunkMapperUtility file_mapper;

    for (const auto& meta : input.metadata) {
        auto file_input =
            FileChunkMapperInput::from_metadata(meta)
                .with_config(input.config)
                .with_checkpoint_size(input.checkpoint_size)
                .with_target_chunk_size(input.target_chunk_size_mb)
                .with_batch_size(input.batch_size)
                .with_start_chunk_index(global_chunk_index);
        file_input.query = input.query;

        auto file_chunks = co_await file_mapper.process(file_input);
        global_chunk_index += static_cast<int>(file_chunks.size());

        all_chunks.insert(all_chunks.end(),
                          std::make_move_iterator(file_chunks.begin()),
                          std::make_move_iterator(file_chunks.end()));
    }

    DFTRACER_UTILS_LOG_INFO(
        "Created %zu chunks from %zu files (avg %.1f chunks/file)",
        all_chunks.size(), input.metadata.size(),
        input.metadata.empty()
            ? 0.0
            : static_cast<double>(all_chunks.size()) /
                  static_cast<double>(input.metadata.size()));

    co_return all_chunks;
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
