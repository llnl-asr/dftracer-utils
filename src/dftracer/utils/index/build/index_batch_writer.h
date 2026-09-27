#ifndef DFTRACER_UTILS_INDEX_BUILD_INDEX_BATCH_WRITER_H
#define DFTRACER_UTILS_INDEX_BUILD_INDEX_BATCH_WRITER_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/plugin_extension.h>
#include <dftracer/utils/index/extensions/rowset_fold.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/store/index_write.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::index::build {

using index::extensions::BloomFold;
using index::extensions::RowSetFold;
using index::schemas::dft::agg::AggregationFold;

struct ParsedIndexJob {
    int file_id = 0;
    std::string file_path;
    std::string logical_path;
    /// Written with the file's data, so a file is registered only once its
    /// index commits.
    index::store::layout::FileRecord record;
    index::gzip::GzipBuildArtifacts artifacts;
    // Owns the folds' intern so it outlives them through the channel; declared
    // first so it is destroyed last.
    std::unique_ptr<dftracer::utils::StringIntern> intern;
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

/// Drain `channel`, group `ParsedIndexJob`s into batches of `batch_size`,
/// and commit each batch through a fresh `IndexWrite` produced by
/// `make_sink()`. The caller-provided `commit_sink(sink)` finalises the
/// batch: for RocksDB-backed writes it calls `.commit()`; for SST-backed
/// writes it flushes to disk and routes `Artifacts` to a registry.
///
/// `MakeSink` must be invocable as `() -> std::unique_ptr<IndexWrite>`
/// (or any subclass thereof). `CommitSink` must be invocable as
/// `(IndexWrite&) -> void`.
///
/// A job whose bloom fold spilled writes its tier as runs instead: they go to
/// `commit_spill(runs)` after the batch commits, as that file's own ingest.
template <typename MakeSink, typename CommitSink, typename CommitSpill>
inline coro::CoroTask<void> index_batch_write_worker(
    coro::Channel<ParsedIndexJob>* channel, std::size_t batch_size,
    BatchWriterMetrics* metrics, MakeSink make_sink, CommitSink commit_sink,
    CommitSpill commit_spill) {
    std::vector<ParsedIndexJob> batch;
    batch.reserve(batch_size);

    auto flush = [&]() {
        if (batch.empty()) return;
        auto start = std::chrono::steady_clock::now();

        auto sink_owned = make_sink();
        index::store::IndexWrite& sink = *sink_owned;
        std::vector<std::pair<
            ParsedIndexJob*,
            std::vector<
                index::store::IndexDatabaseSstWriterContext::Artifacts>>>
            spilled;
        for (auto& job : batch) {
            if (!job.success) continue;
            const bool spills =
                job.bloom_fold && job.bloom_fold->spill_runs() > 0;
            try {
                if (!job.tier_only) {
                    if (!job.logical_path.empty())
                        index::store::records::put_file_record(
                            sink, job.logical_path, job.record);
                    index::gzip::persist_gzip_index_artifacts(sink, job.file_id,
                                                              job.artifacts);
                    // New members renumber the chunks, so tier data the
                    // bloom fold does not rewrite would describe old ones. A
                    // spilled tier clears in its own first run instead: this
                    // batch may commit after that ingest.
                    for (auto ext : index::store::ALL_EXTENSIONS)
                        if (!spills &&
                            ext != index::store::IndexExtension::MEMBERS)
                            index::store::records::clear_file(sink, ext,
                                                              job.file_id);
                    // Plugin data is never in a spill run, so it clears here.
                    index::store::records::clear_file(
                        sink, index::store::IndexExtension::PLUGIN,
                        job.file_id);
                    // After the clears, which cover core.profile too.
                    if (job.schema)
                        index::store::records::put_profile(
                            sink, job.file_id, job.schema->id,
                            job.schema->params_hash());
                }
                if (spills)
                    spilled.emplace_back(
                        &job, job.bloom_fold->finish_spilled(job.file_id));
                else if (job.bloom_fold)
                    job.bloom_fold->write(sink, job.file_id);
                if (job.plugin_builders)
                    job.plugin_builders->write(sink, job.file_id);
                if (job.rowset_fold) job.rowset_fold->write(sink, job.file_id);
                if (job.agg_fold) job.agg_fold->write(sink);
            } catch (const std::exception& e) {
                job.success = false;
                job.error_message = e.what();
                DFTRACER_UTILS_LOG_ERROR(
                    "Failed to write index for %s: %s; file dropped from index",
                    job.file_path.c_str(), e.what());
            }
        }
        commit_sink(sink);
        for (auto& [job, runs] : spilled) {
            try {
                commit_spill(std::move(runs));
            } catch (const std::exception& e) {
                job->success = false;
                job->error_message = e.what();
                DFTRACER_UTILS_LOG_ERROR(
                    "Failed to ingest the spilled index of %s: %s",
                    job->file_path.c_str(), e.what());
            }
        }

        auto end = std::chrono::steady_clock::now();
        if (metrics) {
            metrics->write_ns.fetch_add(
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(end -
                                                                         start)
                        .count()),
                std::memory_order_relaxed);
            std::size_t written = 0;
            for (const auto& job : batch) {
                if (job.success) ++written;
            }
            metrics->files_written.fetch_add(written,
                                             std::memory_order_relaxed);
            metrics->batches_committed.fetch_add(1, std::memory_order_relaxed);
        }
        batch.clear();
    };

    // Each job carries its file's hash table, so a batch of the nominal size
    // holds every entry of `batch_size` high-cardinality files at once. Flush
    // early once the accumulated entries cross a budget to bound peak heap.
    static constexpr std::size_t MAX_BATCH_HASH_ENTRIES = 2u * 1024 * 1024;
    std::size_t batch_hash_entries = 0;

    while (auto item = co_await channel->receive()) {
        std::size_t entries =
            item->rowset_fold ? item->rowset_fold->entry_count() : 0;
        batch.push_back(std::move(*item));
        batch_hash_entries += entries;
        if (batch.size() >= batch_size ||
            batch_hash_entries >= MAX_BATCH_HASH_ENTRIES) {
            flush();
            batch_hash_entries = 0;
        }
    }
    flush();
    co_return;
}

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_INDEX_BATCH_WRITER_H
