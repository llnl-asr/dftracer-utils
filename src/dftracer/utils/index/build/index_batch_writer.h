#ifndef DFTRACER_UTILS_INDEX_BUILD_INDEX_BATCH_WRITER_H
#define DFTRACER_UTILS_INDEX_BUILD_INDEX_BATCH_WRITER_H

#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/plugin_extension.h>
#include <dftracer/utils/index/extensions/rowset_fold.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/store/index_database_sst_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::index::build {

using index::extensions::BloomFold;
using index::extensions::RowSetFold;
using index::schemas::dft::agg::AggregationFold;

struct ParsedIndexJob {
    int file_id = 0;
    /// The index holds no data for `file_id` yet, so nothing is cleared.
    bool fresh_id = false;
    std::string file_path;
    std::string logical_path;
    /// Written with the file's data, so a file is registered only once its
    /// index commits.
    index::store::layout::FileRecord record;
    index::gzip::GzipBuildArtifacts artifacts;
    // Owns the folds' intern so it outlives them through the channel; declared
    // first so it is destroyed last.
    std::unique_ptr<dftracer::utils::dstd::StringIntern> intern;
    std::unique_ptr<BloomFold> bloom_fold;
    std::unique_ptr<RowSetFold> rowset_fold;
    std::unique_ptr<AggregationFold> agg_fold;
    std::unique_ptr<index::extensions::PluginBuilders> plugin_builders;
    bool success = true;
    /// Write only the bloom fold's tier; see IndexBuildBatchConfig::tier_only.
    bool tier_only = false;
    /// The schema the file was decoded with; written with its members.
    const index::RecordSchema* schema = nullptr;
    std::string error_message;
};

struct BatchWriterMetrics {
    std::atomic<std::uint64_t> write_ns{0};
    std::atomic<std::size_t> files_written{0};
    std::atomic<std::size_t> batches_committed{0};
};

/// The spill runs of one file, committed as that file's own ingest once the
/// sink they were written beside has committed.
struct SpilledFile {
    std::string file_path;
    std::vector<index::store::IndexDatabaseSstWriterContext::Artifacts> runs;
};

/// Write `job` into `sink`, which the caller commits. A file whose bloom fold
/// spilled writes its tier as runs instead: they are appended to `spilled`.
/// Returns false, with `job.error_message` set, when the write failed; what
/// was already written for the file is left in the sink.
inline bool write_index_job(index::store::IndexWrite& sink, ParsedIndexJob& job,
                            std::vector<SpilledFile>& spilled) {
    sink.set_fresh_file(job.fresh_id ? job.file_id : -1);
    // Only a file that spilled while parsed writes its tier as runs.
    const bool spills = job.bloom_fold && job.bloom_fold->writes_runs();
    try {
        if (!job.tier_only) {
            if (!job.logical_path.empty())
                index::store::records::put_file_record(sink, job.logical_path,
                                                       job.record);
            index::gzip::persist_gzip_index_artifacts(sink, job.file_id,
                                                      job.artifacts);
            // New members renumber the chunks, so tier data the bloom fold
            // does not rewrite would describe old ones. A spilled tier clears
            // in its own first run instead: this sink may commit after that
            // ingest. A fresh id has no old data, so its clears write nothing.
            for (auto ext : index::store::ALL_EXTENSIONS)
                if (!spills && ext != index::store::IndexExtension::Members)
                    index::store::records::clear_file(sink, ext, job.file_id);
            // Plugin data is never in a spill run, so it clears here.
            index::store::records::clear_file(
                sink, index::store::IndexExtension::Plugin, job.file_id);
            // After the clears, which cover core.profile too.
            if (job.schema)
                index::store::records::put_profile(sink, job.file_id,
                                                   job.schema->id,
                                                   job.schema->params_hash());
        }
        if (spills)
            spilled.push_back(
                {job.file_path, job.bloom_fold->finish_spilled(job.file_id)});
        else if (job.bloom_fold)
            job.bloom_fold->write(sink, job.file_id);
        if (job.plugin_builders) job.plugin_builders->write(sink, job.file_id);
        if (job.rowset_fold) job.rowset_fold->write(sink, job.file_id);
        if (job.agg_fold) job.agg_fold->write(sink);
    } catch (const std::exception& e) {
        job.success = false;
        job.error_message = e.what();
        DFTRACER_UTILS_LOG_ERROR(
            "Failed to write index for %s: %s; file dropped from index",
            job.file_path.c_str(), e.what());
        return false;
    }
    return true;
}

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_INDEX_BATCH_WRITER_H
