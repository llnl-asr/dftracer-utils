#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_GROUP_WRITER_TASK_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_GROUP_WRITER_TASK_H

#include <concurrentqueue.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/organize_visitor.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <dftracer/utils/utilities/indexer/index_database_sst_writer_context.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::reorganize {

struct GroupWriterConfig {
    std::string group_name;
    std::string group_query;
    std::string output_dir;
    std::size_t chunk_size_bytes = 256 * 1024 * 1024;
    bool compress = true;
    int compression_level = -1;  // Z_DEFAULT_COMPRESSION (level 6)
    std::shared_ptr<coro::Channel<std::shared_ptr<LineBatch>>> input_channel;
    const std::vector<SourceFileInfo>* source_files = nullptr;
    bool build_output_index = true;

    std::string index_dir;
    bool with_aggregation = false;
    double agg_time_interval_us = 5'000'000.0;
    std::vector<std::string> bloom_dimensions;
    indexing::ChunkIndexerConfig bloom_config;
    std::string staging_root;
    std::shared_ptr<moodycamel::ConcurrentQueue<
        indexer::IndexDatabaseSstWriterContext::Artifacts>>
        artifacts_queue;
    std::shared_ptr<std::atomic<std::size_t>> batch_counter;
};

struct ChunkMemberLayout {
    std::string path;
    std::vector<fileio::parallel::ParallelWriter::MemberSpan> members;
};

struct GroupWriterResult {
    std::string group_name;
    std::size_t events_written = 0;
    std::size_t bytes_written = 0;
    std::size_t chunks_created = 0;
    std::vector<std::string> output_files;
    /// Per-chunk-file gzip-member layout captured directly from the writer.
    /// Lets downstream indexing skip the post-write gzip header re-scan.
    std::vector<ChunkMemberLayout> chunk_layouts;
    bool indexed_inline = false;
};

coro::CoroTask<Result<GroupWriterResult>> run_group_writer(
    CoroScope* scope, GroupWriterConfig config);

}  // namespace dftracer::utils::utilities::composites::dft::reorganize

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_GROUP_WRITER_TASK_H
