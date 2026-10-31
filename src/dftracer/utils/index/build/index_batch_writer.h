#ifndef DFTRACER_UTILS_INDEX_BUILD_INDEX_BATCH_WRITER_H
#define DFTRACER_UTILS_INDEX_BUILD_INDEX_BATCH_WRITER_H

#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/index/build/file_admission.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/plugin_extension.h>
#include <dftracer/utils/index/extensions/rowset_fold.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/coro/task.h>

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
    /// The staged writes the bloom and aggregation folds left.
    /// write_index_job publishes them before the file's build.
    std::vector<std::unique_ptr<index::store::IndexDatabaseWrite>> staged;
};

struct BatchWriterMetrics {
    std::atomic<std::uint64_t> write_ns{0};
    std::atomic<std::uint64_t> schema_ns{0};
    std::atomic<std::size_t> files_written{0};
    std::atomic<std::size_t> batches_committed{0};
};

/// Puts every fold of `job` into `build`. What a spilled bloom fold or an
/// aggregation fold that wrote through left goes into `job.staged` instead.
inline void fill_file_build(index::FileBuild& build, ParsedIndexJob& job) {
    const bool spills = job.bloom_fold && job.bloom_fold->writes_runs();
    auto agg_staged = job.agg_fold ? job.agg_fold->finish_through() : nullptr;
    const bool agg_through = agg_staged != nullptr;
    if (!job.tier_only) {
        index::gzip::persist_gzip_index_artifacts(build, job.artifacts);
        if (job.schema) {
            index::put_profile(build, job.schema->id);
            build.finish(index::store::IndexExtension::Profile,
                         job.schema->params_hash());
        }
    }
    if (spills) {
        auto staged = job.bloom_fold->finish_staged(job.file_id);
        index::store::records::put_file_record(*staged, job.logical_path,
                                               job.record);
        job.staged.push_back(std::move(staged));
    } else if (job.bloom_fold)
        job.bloom_fold->write(build);
    if (job.plugin_builders) job.plugin_builders->write(build);
    if (job.rowset_fold) job.rowset_fold->write(build);
    if (agg_through) {
        index::store::records::put_file_record(*agg_staged, job.logical_path,
                                               job.record);
        job.staged.push_back(std::move(agg_staged));
    } else if (job.agg_fold)
        job.agg_fold->write(build);
    // New members renumber the chunks, so a copy the build does not write
    // would describe old ones. A spilled tier replaces its own copies in its
    // staged write, as do aggregation rows written through, so a spilled file
    // keeps every built-in copy and drops only the plugin ones. A fresh id has
    // nothing to clear.
    if (job.tier_only || job.fresh_id) return;
    using index::store::IndexExtension;
    const IndexExtension through[] = {IndexExtension::Agg};
    const auto keep = spills ? dstd::Span<const IndexExtension>(
                                   index::store::ALL_EXTENSIONS,
                                   std::size(index::store::ALL_EXTENSIONS))
                             : dstd::Span<const IndexExtension>(
                                   through, std::size_t{agg_through ? 1u : 0u});
    index::clear_unwritten(build, keep);
}

/// Writes `job` into one FileBuild. A staged write goes first and the build
/// commits last, so a reader never sees the file without its tier. The folds
/// and the ticket go once the build holds their data. Returns false, with
/// `job.error_message` set, when a write failed, and nothing is published.
inline coro::CoroTask<bool> write_index_job(const index::Index& index,
                                            ParsedIndexJob& job,
                                            FileAdmission::Ticket& ticket) {
    std::string failure;
    dstd::Optional<index::FileBuild> build;
    auto started = index.build(
        job.logical_path,
        index::SourceStat{job.record.mtime, job.record.size, job.record.hash});
    if (started)
        build.emplace(std::move(*started));
    else
        failure = std::string(started.error().message());
    if (build) {
        try {
            fill_file_build(*build, job);
        } catch (const std::exception& e) {
            failure = e.what();
        }
    }
    job.artifacts = {};
    job.bloom_fold.reset();
    job.rowset_fold.reset();
    job.agg_fold.reset();
    job.plugin_builders.reset();
    job.intern.reset();
    ticket = {};
    for (auto& staged : job.staged) {
        if (!failure.empty()) break;
        try {
            co_await staged->commit_async();
        } catch (const std::exception& e) {
            failure = e.what();
        }
        staged.reset();
    }
    if (failure.empty()) {
        auto committed = co_await build->commit();
        if (!committed) failure = std::string(committed.error().message());
    }
    job.staged.clear();
    if (!failure.empty()) {
        job.success = false;
        job.error_message = failure;
        DFTRACER_UTILS_LOG_ERROR(
            "Failed to write index for %s: %s; file dropped from index",
            job.file_path.c_str(), failure.c_str());
        co_return false;
    }
    co_return true;
}

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_INDEX_BATCH_WRITER_H
