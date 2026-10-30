#ifndef DFTRACER_UTILS_INDEX_BUILD_INDEX_BATCH_WRITER_H
#define DFTRACER_UTILS_INDEX_BUILD_INDEX_BATCH_WRITER_H

#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/plugin_extension.h>
#include <dftracer/utils/index/extensions/rowset_fold.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
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
    /// The staged writes the bloom and aggregation folds left. The caller
    /// publishes them after write_index_job returns.
    std::vector<std::unique_ptr<index::store::IndexDatabaseWrite>> staged;
};

struct BatchWriterMetrics {
    std::atomic<std::uint64_t> write_ns{0};
    std::atomic<std::uint64_t> schema_ns{0};
    std::atomic<std::size_t> files_written{0};
    std::atomic<std::size_t> batches_committed{0};
};

/// Write `job` into `sink`, which the caller commits. A file whose bloom fold
/// spilled or whose aggregation fold wrote through writes that data into
/// `job.staged`, which the caller publishes before the next file. Returns
/// false, with `job.error_message` set, when the write failed. What was already
/// written for the file is left in the sink.
inline bool write_index_job(index::store::IndexWrite& sink,
                            ParsedIndexJob& job) {
    sink.set_fresh_file(job.fresh_id ? job.file_id : -1);
    const bool spills = job.bloom_fold && job.bloom_fold->writes_runs();
    try {
        auto agg_staged =
            job.agg_fold ? job.agg_fold->finish_through() : nullptr;
        if (!job.tier_only) {
            if (!job.logical_path.empty())
                index::store::records::put_file_record(sink, job.logical_path,
                                                       job.record);
            index::gzip::persist_gzip_index_artifacts(sink, job.file_id,
                                                      job.artifacts);
            // New members renumber the chunks, so tier data the bloom fold does
            // not rewrite would describe old ones. A spilled tier clears in its
            // own staged write, as do rows the aggregation fold wrote through.
            // A fresh id has nothing to clear.
            for (auto ext : index::store::ALL_EXTENSIONS)
                if (!spills && ext != index::store::IndexExtension::Members &&
                    !(agg_staged && ext == index::store::IndexExtension::Agg))
                    index::store::records::clear_file(sink, ext, job.file_id);
            // Plugin data is never in a spill run.
            index::store::records::clear_file(
                sink, index::store::IndexExtension::Plugin, job.file_id);
            // After the clears, which cover core.profile too.
            if (job.schema)
                index::store::records::put_profile(sink, job.file_id,
                                                   job.schema->id,
                                                   job.schema->params_hash());
        }
        if (spills) {
            auto staged = job.bloom_fold->finish_staged(job.file_id);
            if (!job.logical_path.empty())
                index::store::records::put_file_record(
                    *staged, job.logical_path, job.record);
            job.staged.push_back(std::move(staged));
        } else if (job.bloom_fold)
            job.bloom_fold->write(sink, job.file_id);
        if (job.plugin_builders) job.plugin_builders->write(sink, job.file_id);
        if (job.rowset_fold) job.rowset_fold->write(sink, job.file_id);
        if (agg_staged) {
            if (!job.logical_path.empty())
                index::store::records::put_file_record(
                    *agg_staged, job.logical_path, job.record);
            job.staged.push_back(std::move(agg_staged));
        } else if (job.agg_fold)
            job.agg_fold->write(sink);
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
