#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_INDEX_BATCH_WRITER_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_INDEX_BATCH_WRITER_H

#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/composites/dft/visitors/bloom_visitor.h>
#include <dftracer/utils/utilities/composites/dft/visitors/hash_table_visitor.h>
#include <dftracer/utils/utilities/composites/dft/visitors/manifest_visitor.h>
#include <dftracer/utils/utilities/indexer/index_batch_sink.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/gzip_indexer.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::indexer::internal {

using composites::dft::visitors::BloomVisitor;
using composites::dft::visitors::HashTableVisitor;
using composites::dft::visitors::ManifestVisitor;

struct ParsedIndexJob {
    int file_id = 0;
    std::string file_path;
    gzip::GzipBuildArtifacts artifacts;
    std::unique_ptr<BloomVisitor> bloom_visitor;
    std::unique_ptr<HashTableVisitor> hash_table_visitor;
    std::unique_ptr<ManifestVisitor> manifest_visitor;
    bool success = true;
    std::string error_message;
};

struct BatchWriterMetrics {
    std::atomic<std::uint64_t> write_ns{0};
    std::atomic<std::size_t> files_written{0};
    std::atomic<std::size_t> batches_committed{0};
};

/// Drain `channel`, group `ParsedIndexJob`s into batches of `batch_size`,
/// and commit each batch through a fresh `IndexBatchSink` produced by
/// `make_sink()`. The caller-provided `commit_sink(sink)` finalises the
/// batch: for RocksDB-backed sinks it calls `.commit()`; for SST-backed
/// sinks it flushes to disk and routes `Artifacts` to a registry.
///
/// `MakeSink` must be invocable as `() -> std::unique_ptr<IndexBatchSink>`
/// (or any subclass thereof). `CommitSink` must be invocable as
/// `(IndexBatchSink&) -> void`.
template <typename MakeSink, typename CommitSink>
inline coro::CoroTask<void> index_batch_write_worker(
    coro::Channel<ParsedIndexJob>* channel, std::size_t batch_size,
    BatchWriterMetrics* metrics, MakeSink make_sink, CommitSink commit_sink) {
    std::vector<ParsedIndexJob> batch;
    batch.reserve(batch_size);

    auto flush = [&]() {
        if (batch.empty()) return;
        auto start = std::chrono::steady_clock::now();

        auto sink_owned = make_sink();
        IndexBatchSink& sink = *sink_owned;
        for (auto& job : batch) {
            if (!job.success) continue;
            try {
                for (const auto& checkpoint : job.artifacts.checkpoints) {
                    sink.insert_checkpoint(job.file_id, checkpoint);
                }
                sink.insert_file_metadata(
                    job.file_id, job.artifacts.checkpoint_size,
                    job.artifacts.total_lines, job.artifacts.total_uc_size);
                if (job.bloom_visitor) {
                    job.bloom_visitor->finalize_sink_only(sink, job.file_id);
                }
                if (job.hash_table_visitor) {
                    job.hash_table_visitor->finalize(sink, job.file_id);
                }
                if (job.manifest_visitor) {
                    job.manifest_visitor->finalize(sink, job.file_id);
                }
            } catch (const std::exception& e) {
                job.success = false;
                job.error_message = e.what();
            }
        }
        commit_sink(sink);

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

    while (auto item = co_await channel->receive()) {
        batch.push_back(std::move(*item));
        if (batch.size() >= batch_size) {
            flush();
        }
    }
    flush();
    co_return;
}

}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_INDEX_BATCH_WRITER_H
