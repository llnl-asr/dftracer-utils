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
    bool fresh_id = false;
    std::string file_path;
    index::gzip::GzipBuildArtifacts artifacts;
    // The folds' intern, destroyed after them.
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
    std::atomic<std::uint64_t> schema_ns{0};
    std::atomic<std::size_t> files_written{0};
    std::atomic<std::size_t> batches_committed{0};
};

/// Puts every fold of `job` into `build`, the one it spilled into.
inline void fill_file_build(index::FileBuild& build, ParsedIndexJob& job) {
    if (!job.tier_only) {
        index::gzip::persist_gzip_index_artifacts(build, job.artifacts);
        if (job.schema) {
            index::put_profile(build, job.schema->id);
            build.finish(index::store::IndexExtension::Profile,
                         job.schema->params_hash());
        }
    }
    if (job.bloom_fold) job.bloom_fold->write(build);
    if (job.plugin_builders) job.plugin_builders->write(build);
    if (job.rowset_fold) job.rowset_fold->write(build);
    if (job.agg_fold) job.agg_fold->write(build);
    // New members renumber the chunks, so a copy the build does not write
    // would describe old ones. A fresh id has nothing to clear.
    if (job.tier_only || job.fresh_id) return;
    index::clear_unwritten(build);
}

/// Writes `job` into `build`, the one its folds spilled into, and commits it.
/// One commit publishes the members, the tier and the aggregation rows, so a
/// reader never sees the file without its tier. The folds and the ticket go
/// once the build holds their data. Returns false, with `job.error_message`
/// set, when a write failed, and nothing is published.
inline coro::CoroTask<bool> write_index_job(index::FileBuild& build,
                                            ParsedIndexJob& job,
                                            FileAdmission::Ticket& ticket) {
    std::string failure;
    try {
        fill_file_build(build, job);
    } catch (const std::exception& e) {
        failure = e.what();
    }
    job.artifacts = {};
    job.bloom_fold.reset();
    job.rowset_fold.reset();
    job.agg_fold.reset();
    job.plugin_builders.reset();
    job.intern.reset();
    ticket = {};
    if (failure.empty()) {
        auto committed = co_await build.commit();
        if (!committed) failure = std::string(committed.error().message());
    }
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
