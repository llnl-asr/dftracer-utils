// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_BUILD_INDEX_BATCH_WRITER_H
#define DFTRACER_UTILS_INDEX_BUILD_INDEX_BATCH_WRITER_H

#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/build/file_admission.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/plugin_extension.h>
#include <dftracer/utils/index/extensions/rowset_fold.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/tables.h>

#include <atomic>
#include <cstddef>

namespace dftracer::utils::index::build {

using index::extensions::BloomFold;
using index::extensions::RowSetFold;
using index::schemas::dft::agg::AggregationFold;

struct ParsedIndexJob {
    /// The file was not in the index before, so it has no old tier to clear.
    bool fresh = false;
    /// These are new members of a file the index holds: the keys add to it.
    bool appended = false;
    dstd::String file_path;
    index::gzip::GzipBuildArtifacts artifacts;
    // The folds' intern, destroyed after them.
    dstd::UniquePtr<dftracer::utils::dstd::StringIntern> intern;
    dstd::UniquePtr<BloomFold> bloom_fold;
    dstd::UniquePtr<RowSetFold> rowset_fold;
    dstd::UniquePtr<AggregationFold> agg_fold;
    dstd::UniquePtr<index::extensions::PluginBuilders> plugin_builders;
    bool success = true;
    /// Write only the bloom fold's tier; see IndexBuildBatchConfig::tier_only.
    bool tier_only = false;
    /// The schema the file was decoded with; written with its members.
    const index::RecordSchema* schema = nullptr;
    dstd::String error_message;
};

struct BatchWriterMetrics {
    std::atomic<std::uint64_t> write_ns{0};
    std::atomic<std::uint64_t> schema_ns{0};
    std::atomic<std::size_t> files_written{0};
    std::atomic<std::size_t> batches_committed{0};
};

/// Puts every fold of `job` into `build`, the one it spilled into.
inline dstd::Result<void> fill_file_build(index::FileBuild& build,
                                          ParsedIndexJob& job) {
    if (!job.tier_only) {
        index::gzip::persist_gzip_index_artifacts(build, job.artifacts);
        if (job.schema) {
            index::put_profile(build, job.schema->id);
            build.finish(index::store::IndexExtension::Profile,
                         job.schema->params_hash());
        }
    }
    if (job.bloom_fold) {
        auto written = job.bloom_fold->write(build);
        if (!written) return written.error();
    }
    if (job.plugin_builders) job.plugin_builders->write(build);
    if (job.rowset_fold) {
        auto written = job.rowset_fold->write(build);
        if (!written) return written.error();
    }
    if (job.agg_fold) {
        auto written = job.agg_fold->write(build);
        if (!written) return written.error();
    }
    if (job.appended) {
        // A row set frame holds the whole file, and these members are not in
        // it. Queries scan until a rebuild writes it again.
        build.keep_older_copies();
        build.clear(index::store::IndexExtension::Rowset);
        return {};
    }
    // A fresh file has no older copies. A rebuild renumbers the chunks, so a
    // copy it does not write would describe old ones.
    if (job.fresh) build.keep_older_copies();
    if (job.tier_only || job.fresh) return {};
    index::clear_unwritten(build);
    return {};
}

/// Writes `job` into `build`, the one its folds spilled into, for the caller
/// to commit with the other builds of its group. The folds and the ticket go
/// once the build holds their data. Returns false, with `job.error_message`
/// set, when a write failed, and the build must not be committed.
inline bool fill_index_job(index::FileBuild& build, ParsedIndexJob& job,
                           FileAdmission::Ticket& ticket) {
    dstd::String failure;
    auto filled = fill_file_build(build, job);
    if (!filled) failure = dstd::String(filled.error().message());
    job.artifacts = {};
    job.bloom_fold.reset();
    job.rowset_fold.reset();
    job.agg_fold.reset();
    job.plugin_builders.reset();
    job.intern.reset();
    ticket = {};
    if (failure.empty()) return true;
    job.success = false;
    job.error_message = failure;
    DFTRACER_UTILS_LOG_ERROR(
        "Failed to write index for {}: {}; file dropped from index",
        job.file_path, failure);
    return false;
}

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_INDEX_BATCH_WRITER_H
