#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/composites/dft/dft_event_dispatcher.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/visitors/bloom_visitor.h>
#include <dftracer/utils/utilities/composites/dft/visitors/hash_table_visitor.h>
#include <dftracer/utils/utilities/composites/dft/visitors/manifest_visitor.h>
#include <dftracer/utils/utilities/indexer/error.h>
#include <dftracer/utils/utilities/indexer/index_batch_sink.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/index_batch_writer.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/indexer/internal/transaction_scope.h>

#include <atomic>
#include <chrono>
#include <optional>

namespace dftracer::utils::utilities::indexer {

using composites::dft::internal::determine_index_path;
using composites::dft::visitors::BloomVisitor;
using composites::dft::visitors::HashTableVisitor;
using composites::dft::visitors::ManifestVisitor;

static coro::CoroTask<IndexBuildBatchResult> process_batch_per_file(
    CoroScope* scope, std::shared_ptr<IndexBuildBatchConfig> shared_config) {
    auto results = std::make_shared<std::vector<IndexBuildResult>>(
        shared_config->file_paths.size());
    auto indexed = std::make_shared<std::atomic<std::size_t>>(0);
    auto skipped = std::make_shared<std::atomic<std::size_t>>(0);
    auto failed = std::make_shared<std::atomic<std::size_t>>(0);
    auto total_events = std::make_shared<std::atomic<std::uint64_t>>(0);

    auto file_paths = std::make_shared<std::vector<std::string>>(
        std::move(shared_config->file_paths));
    const auto parallelism = shared_config->parallelism;

    co_await scope->scope([file_paths, results, indexed, skipped, failed,
                           total_events, shared_config, parallelism](
                              CoroScope& child) -> coro::CoroTask<void> {
        auto file_chan = coro::make_channel<std::size_t>(parallelism * 2);

        child.spawn(
            [ch = file_chan->producer(), num_files = file_paths->size()](
                CoroScope&) mutable -> coro::CoroTask<void> {
                auto guard = ch.guard();
                for (std::size_t i = 0; i < num_files; ++i) {
                    if (!co_await ch.send(i)) co_return;
                }
                co_return;
            });

        for (std::size_t w = 0; w < parallelism; ++w) {
            child.spawn([file_chan, file_paths, results, shared_config, indexed,
                         skipped, failed,
                         total_events](CoroScope&) -> coro::CoroTask<void> {
                while (auto idx_opt = co_await file_chan->receive()) {
                    std::size_t idx = *idx_opt;
                    IndexBuilderUtility builder;
                    auto file_config =
                        IndexBuildConfig::for_file((*file_paths)[idx])
                            .with_index_dir(shared_config->index_dir)
                            .with_checkpoint_size(
                                shared_config->checkpoint_size)
                            .with_force_rebuild(shared_config->force_rebuild)
                            .with_manifest(shared_config->build_manifest)
                            .with_bloom_config(shared_config->bloom_config)
                            .with_bloom_dimensions(
                                shared_config->bloom_dimensions);

                    auto result = co_await builder.process(file_config);

                    if (result.was_skipped) {
                        skipped->fetch_add(1, std::memory_order_relaxed);
                    } else if (result.success) {
                        indexed->fetch_add(1, std::memory_order_relaxed);
                        total_events->fetch_add(result.events_processed,
                                                std::memory_order_relaxed);
                    } else {
                        failed->fetch_add(1, std::memory_order_relaxed);
                    }
                    (*results)[idx] = std::move(result);
                }
                co_return;
            });
        }
        co_return;
    });

    IndexBuildBatchResult batch_result;
    batch_result.results = std::move(*results);
    batch_result.indexed = indexed->load(std::memory_order_relaxed);
    batch_result.skipped = skipped->load(std::memory_order_relaxed);
    batch_result.failed = failed->load(std::memory_order_relaxed);
    batch_result.total_events = total_events->load(std::memory_order_relaxed);
    co_return batch_result;
}

namespace {

struct PreparedFile {
    std::size_t index;
    std::string file_path;
    std::string logical_path;
    std::string index_path;
    std::uint64_t file_hash = 0;
    int file_id = 0;
    IndexBuildBatchConfig::FileSlice slice;
};

struct ParsedBloomJob {
    PreparedFile identity;
    IndexBuildResult result;
    internal::gzip::GzipBuildArtifacts artifacts;
    std::unique_ptr<BloomVisitor> bloom_visitor;
    std::unique_ptr<HashTableVisitor> hash_table_visitor;
    std::unique_ptr<ManifestVisitor> manifest_visitor;
    std::vector<std::unique_ptr<composites::dft::DftEventVisitor>>
        extra_visitors;
};

std::vector<PreparedFile> prepare_file_identities(
    const std::string& index_path, const std::vector<std::string>& file_paths,
    bool build_manifest) {
    IndexDatabase db(index_path);
    auto writer = db.begin_write();

    std::vector<PreparedFile> prepared;
    prepared.reserve(file_paths.size());
    for (std::size_t i = 0; i < file_paths.size(); ++i) {
        PreparedFile pf;
        pf.index = i;
        pf.file_path = file_paths[i];
        pf.logical_path = internal::get_logical_path(file_paths[i]);
        pf.index_path = index_path;
        pf.file_hash = internal::calculate_file_hash(file_paths[i]);
        IndexFileEntryCapability caps =
            IndexFileEntryCapability::BLOOM |
            IndexFileEntryCapability::CHECKPOINTS |
            IndexFileEntryCapability::FILE_SUMMARY |
            IndexFileEntryCapability::INDEXING_COMPLETE;
        if (build_manifest) {
            caps |= IndexFileEntryCapability::MANIFEST;
        }
        pf.file_id = writer->get_or_create_file_info(pf.logical_path,
                                                     pf.file_hash, caps);
        prepared.push_back(std::move(pf));
    }
    writer->commit();
    return prepared;
}

}  // namespace

struct BatchWriteState {
    std::shared_ptr<std::vector<IndexBuildResult>> results;
    std::shared_ptr<std::vector<std::optional<ParsedBloomJob>>> parsed_jobs;
    std::shared_ptr<std::vector<PreparedFile>> prepared;
    std::shared_ptr<std::vector<std::string>> bloom_dims;
    std::string index_path;
    IndexBuildBatchMetrics metrics;
    composites::dft::indexing::ChunkIndexerConfig bloom_config;
    std::size_t num_files = 0;
    std::size_t parallelism = 0;
    std::size_t checkpoint_size = 0;
    bool build_manifest = false;
    IndexBuildBatchConfig::DftVisitorFactory visitor_factory;
    IndexBuildBatchConfig::SinkFactory sink_factory;
    IndexBuildBatchConfig::SinkCommitFn sink_commit;
};

// Parse one file at a time (work-stealing via atomic next_index), and stream
// the resulting bloom/hash/manifest payload directly to the write channel so
// write workers can begin committing before the parse phase finishes. The
// extra_visitors and result are left in parsed_jobs[idx] for
// finalize_batch_result; the channel item only carries what the write phase
// needs.
static coro::CoroTask<void> parse_and_emit_worker(
    CoroScope* scope, std::atomic<std::size_t>* next_index_ptr,
    std::vector<IndexBuildResult>* results_ptr,
    std::vector<std::optional<ParsedBloomJob>>* parsed_jobs_ptr,
    std::vector<PreparedFile>* prepared_ptr, std::size_t checkpoint_size,
    composites::dft::indexing::ChunkIndexerConfig bloom_config,
    const std::vector<std::string>* bloom_dims_ptr,
    std::atomic<std::uint64_t>* parse_ns_ptr,
    const IndexBuildBatchConfig::DftVisitorFactory* visitor_factory_ptr,
    bool build_manifest, coro::ChannelProducer<internal::ParsedIndexJob> ch) {
    namespace gzip_indexer = internal::gzip;
    auto guard = ch.guard();

    while (true) {
        const auto idx =
            next_index_ptr->fetch_add(1, std::memory_order_relaxed);
        if (idx >= prepared_ptr->size()) break;

        const auto& pf = (*prepared_ptr)[idx];
        IndexBuildResult result;
        result.file_path = pf.file_path;
        result.index_path = pf.index_path;
        auto t0 = std::chrono::steady_clock::now();

        ParsedBloomJob job;
        job.identity = pf;
        bool parse_ok = false;
        try {
            composites::dft::DftEventDispatcher::VisitorList dft_vis;
            // Built-in file-scoped visitors are skipped for sliced files
            // where file-scoped writes are disabled (non-first slice of a
            // cross-rank-split file). BloomVisitor::ensure_chunk would also
            // resize chunks_ with a large checkpoint_idx_base.
            if (!pf.slice.skip_file_scoped_writes) {
                job.bloom_visitor = std::make_unique<BloomVisitor>(
                    bloom_config, *bloom_dims_ptr);
                job.hash_table_visitor = std::make_unique<HashTableVisitor>();
                dft_vis.emplace_back(*job.bloom_visitor);
                dft_vis.emplace_back(*job.hash_table_visitor);
                if (build_manifest) {
                    job.manifest_visitor = std::make_unique<ManifestVisitor>();
                    dft_vis.emplace_back(*job.manifest_visitor);
                }
            }
            if (visitor_factory_ptr && *visitor_factory_ptr) {
                job.extra_visitors = (*visitor_factory_ptr)(pf.file_path);
                for (auto& v : job.extra_visitors) {
                    dft_vis.emplace_back(*v);
                }
            }

            composites::dft::DftEventDispatcher batch_dispatcher(
                std::move(dft_vis));
            internal::Indexer::VisitorList visitors;
            visitors.emplace_back(batch_dispatcher);

            gzip_indexer::GzipMemberSlice slice_arg;
            const gzip_indexer::GzipMemberSlice* slice_ptr = nullptr;
            if (pf.slice.members != nullptr &&
                pf.slice.member_end > pf.slice.member_begin) {
                slice_arg.members = pf.slice.members;
                slice_arg.member_begin = pf.slice.member_begin;
                slice_arg.member_end = pf.slice.member_end;
                slice_arg.checkpoint_idx_base = pf.slice.checkpoint_idx_base;
                slice_ptr = &slice_arg;
            }
            auto arts = co_await gzip_indexer::build_gzip_index_artifacts(
                pf.file_path, checkpoint_size, visitors, scope, slice_ptr);
            if (!arts) {
                result.error_message = "Failed to build gzip index artifacts";
            } else {
                job.artifacts = std::move(*arts);
                result.total_lines =
                    static_cast<std::size_t>(job.artifacts.total_lines);
                result.chunks_processed = job.artifacts.checkpoints.size();
                if (job.bloom_visitor) {
                    result.events_processed = static_cast<std::size_t>(
                        job.bloom_visitor->total_events());
                }
                result.index_created = true;
                result.success = true;
                job.result = result;
                for (auto& v : job.extra_visitors) {
                    co_await v->on_file_complete();
                }
                parse_ok = true;
            }
        } catch (const std::exception& e) {
            result.error_message = e.what();
        }

        auto t1 = std::chrono::steady_clock::now();
        parse_ns_ptr->fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count()),
            std::memory_order_relaxed);

        if (!parse_ok) {
            (*results_ptr)[idx] = std::move(result);
            continue;
        }

        // Sliced rank with member_begin > 0: skip file-scoped channel send;
        // aggregation SSTs already produced via extra visitors are kept in
        // parsed_jobs[idx] for downstream collection.
        if (pf.slice.skip_file_scoped_writes) {
            (*results_ptr)[idx] = std::move(result);
            (*parsed_jobs_ptr)[idx] = std::move(job);
            continue;
        }

        // Build the channel-bound payload (move bloom/hash/manifest into it),
        // and leave extra_visitors + result behind in parsed_jobs[idx].
        internal::ParsedIndexJob send_job;
        send_job.file_id = pf.file_id;
        send_job.file_path = pf.file_path;
        send_job.artifacts = std::move(job.artifacts);
        send_job.bloom_visitor = std::move(job.bloom_visitor);
        send_job.hash_table_visitor = std::move(job.hash_table_visitor);
        send_job.manifest_visitor = std::move(job.manifest_visitor);
        send_job.success = true;

        ParsedBloomJob holder;
        holder.identity = pf;
        holder.extra_visitors = std::move(job.extra_visitors);
        holder.result = result;
        (*parsed_jobs_ptr)[idx] = std::move(holder);
        (*results_ptr)[idx] = std::move(result);

        if (!co_await ch.send(std::move(send_job))) co_return;
    }
    co_return;
}

// Streaming parse + write pipeline: parse-and-emit workers and write workers
// run concurrently inside one scope. Parse workers act as multiple producers
// on the write channel (each holds its own ProducerGuard); the channel closes
// for sends when all parse workers exit, after which write workers finish
// draining buffered items, do their final flush, and exit. Memory is bounded
// by the channel capacity (write_workers * WRITE_BATCH_SIZE) so peak heap
// stays bounded regardless of total file count.
static coro::CoroTask<void> run_streaming_pipeline(CoroScope* scope,
                                                   BatchWriteState* state) {
    static constexpr std::size_t WRITE_BATCH_SIZE = 64;
    const auto parse_workers = state->parallelism;
    // Write workers are decoupled from parse workers to control SST count.
    // Floor (parse_workers / 3) reflects the empirical write-vs-parse CPU
    // ratio for the bloom indexer (~3x). Ceiling (num_files / batch_size)
    // ensures large workloads, where total SST count is bounded by
    // ceil(num_files / batch_size) anyway, get full parallelism. Both are
    // capped at parse_workers and given a minimum of 4 for small workloads.
    const auto write_workers = std::min(
        parse_workers, std::max<std::size_t>(
                           4, std::max(parse_workers / 3,
                                       state->num_files / WRITE_BATCH_SIZE)));

    DFTRACER_UTILS_LOG_INFO(
        "IndexBatch: streaming pipeline begin (%zu files, parse_workers=%zu "
        "write_workers=%zu)",
        state->num_files, parse_workers, write_workers);

    // GCC 12 coroutine bug: capturing shared_ptr by value in coroutine
    // lambdas corrupts refcount. Keep shared_ptrs at this scope and pass
    // raw pointers to lambdas.
    auto write_chan = coro::make_channel<internal::ParsedIndexJob>(
        write_workers * WRITE_BATCH_SIZE);
    auto writer_metrics = std::make_shared<internal::BatchWriterMetrics>();

    // Only open the RocksDB-backed DB when no external sink factory is
    // provided. The distributed SST path routes writes through caller-owned
    // SstWriterContext instances and must not hold a process-exclusive
    // RocksDB handle on the target index dir.
    std::shared_ptr<IndexDatabase> writer_db;
    if (!state->sink_factory) {
        writer_db = std::make_shared<IndexDatabase>(state->index_path);
    }

    auto next_index = std::make_shared<std::atomic<std::size_t>>(0);
    auto parse_ns = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto bloom_config_holder =
        std::make_shared<composites::dft::indexing::ChunkIndexerConfig>(
            state->bloom_config);

    auto* next_index_ptr = next_index.get();
    auto* parse_ns_ptr = parse_ns.get();
    auto* results_ptr = state->results.get();
    auto* parsed_jobs_ptr = state->parsed_jobs.get();
    auto* prepared_ptr = state->prepared.get();
    auto* db_ptr = writer_db.get();
    auto* metrics_ptr = writer_metrics.get();
    auto* write_chan_ptr = write_chan.get();
    const auto* bloom_config_ptr = bloom_config_holder.get();
    const auto* bloom_dims_ptr = state->bloom_dims.get();
    const auto checkpoint_size = state->checkpoint_size;
    const bool build_manifest = state->build_manifest;
    const IndexBuildBatchConfig::DftVisitorFactory* visitor_factory_ptr =
        state->visitor_factory ? &state->visitor_factory : nullptr;
    auto* sink_factory_ptr = &state->sink_factory;
    auto* sink_commit_ptr = &state->sink_commit;

    co_await scope->scope([parse_workers, write_workers, next_index_ptr,
                           parse_ns_ptr, results_ptr, parsed_jobs_ptr,
                           prepared_ptr, checkpoint_size, bloom_config_ptr,
                           bloom_dims_ptr, visitor_factory_ptr, build_manifest,
                           write_chan_ptr, db_ptr, metrics_ptr,
                           sink_factory_ptr, sink_commit_ptr](
                              CoroScope& child) -> coro::CoroTask<void> {
        for (std::size_t w = 0; w < parse_workers; ++w) {
            child.spawn(
                [next_index_ptr, parse_ns_ptr, results_ptr, parsed_jobs_ptr,
                 prepared_ptr, checkpoint_size, bloom_config_ptr,
                 bloom_dims_ptr, visitor_factory_ptr, build_manifest,
                 ch = write_chan_ptr->producer()](
                    CoroScope& own_scope) mutable -> coro::CoroTask<void> {
                    co_await parse_and_emit_worker(
                        &own_scope, next_index_ptr, results_ptr,
                        parsed_jobs_ptr, prepared_ptr, checkpoint_size,
                        *bloom_config_ptr, bloom_dims_ptr, parse_ns_ptr,
                        visitor_factory_ptr, build_manifest, std::move(ch));
                });
        }

        for (std::size_t w = 0; w < write_workers; ++w) {
            child.spawn([write_chan_ptr, db_ptr, metrics_ptr, sink_factory_ptr,
                         sink_commit_ptr](CoroScope&) -> coro::CoroTask<void> {
                if (*sink_factory_ptr) {
                    co_await internal::index_batch_write_worker(
                        write_chan_ptr, WRITE_BATCH_SIZE, metrics_ptr,
                        *sink_factory_ptr, *sink_commit_ptr);
                } else {
                    co_await internal::index_batch_write_worker(
                        write_chan_ptr, WRITE_BATCH_SIZE, metrics_ptr,
                        [db_ptr] { return db_ptr->begin_write(); },
                        [](IndexBatchSink& sink) {
                            static_cast<IndexDatabaseWriterContext&>(sink)
                                .commit();
                        });
                }
            });
        }
        co_return;
    });

    state->metrics.parse_ns = parse_ns->load(std::memory_order_relaxed);
    state->metrics.files_parsed = state->num_files;
    state->metrics.write_ns =
        writer_metrics->write_ns.load(std::memory_order_relaxed);
    state->metrics.files_written =
        writer_metrics->files_written.load(std::memory_order_relaxed);
    DFTRACER_UTILS_LOG_INFO(
        "IndexBatch: streaming pipeline complete (parsed=%zu written=%zu)",
        state->num_files, state->metrics.files_written);
    co_return;
}

static std::unique_ptr<BatchWriteState> init_batch_write_state(
    IndexBuildBatchConfig& config) {
    auto state = std::make_unique<BatchWriteState>();
    state->num_files = config.file_paths.size();
    state->parallelism = config.parallelism;
    state->checkpoint_size = config.checkpoint_size;
    state->bloom_config = config.bloom_config;
    state->build_manifest = config.build_manifest;
    state->bloom_dims = std::make_shared<std::vector<std::string>>(
        config.bloom_dimensions.empty()
            ? std::vector<std::string>(DEFAULT_BLOOM_DIMENSIONS.begin(),
                                       DEFAULT_BLOOM_DIMENSIONS.end())
            : std::move(config.bloom_dimensions));
    state->results =
        std::make_shared<std::vector<IndexBuildResult>>(state->num_files);
    state->index_path =
        determine_index_path(config.file_paths.front(), config.index_dir);
    if (!config.file_slices.empty() &&
        config.file_slices.size() != config.file_paths.size()) {
        throw IndexerError(
            IndexerError::Type::INVALID_ARGUMENT,
            "file_slices.size() must match file_paths.size() (or be empty)");
    }
    if (!config.preassigned_file_ids.empty()) {
        if (config.preassigned_file_ids.size() != config.file_paths.size()) {
            throw IndexerError(
                IndexerError::Type::INVALID_ARGUMENT,
                "preassigned_file_ids.size() must match file_paths.size()");
        }
        // Distributed path: coordinator has already registered files and
        // assigned ids. Skip the DEFAULT-CF registry open/write step.
        std::vector<PreparedFile> prepared;
        prepared.reserve(config.file_paths.size());
        for (std::size_t i = 0; i < config.file_paths.size(); ++i) {
            PreparedFile pf;
            pf.index = i;
            pf.file_path = config.file_paths[i];
            pf.logical_path = internal::get_logical_path(config.file_paths[i]);
            pf.index_path = state->index_path;
            pf.file_hash = internal::calculate_file_hash(config.file_paths[i]);
            pf.file_id = config.preassigned_file_ids[i];
            if (!config.file_slices.empty()) pf.slice = config.file_slices[i];
            prepared.push_back(std::move(pf));
        }
        state->prepared =
            std::make_shared<std::vector<PreparedFile>>(std::move(prepared));
    } else {
        state->prepared =
            std::make_shared<std::vector<PreparedFile>>(prepare_file_identities(
                state->index_path, config.file_paths, config.build_manifest));
        if (!config.file_slices.empty()) {
            auto& prepared = *state->prepared;
            for (std::size_t i = 0; i < prepared.size(); ++i) {
                prepared[i].slice = config.file_slices[i];
            }
        }
    }
    state->parsed_jobs =
        std::make_shared<std::vector<std::optional<ParsedBloomJob>>>(
            state->num_files);
    if (config.dft_visitor_factory) {
        state->visitor_factory = std::move(config.dft_visitor_factory);
    }
    state->sink_factory = std::move(config.sink_factory);
    state->sink_commit = std::move(config.sink_commit);
    if (static_cast<bool>(state->sink_factory) !=
        static_cast<bool>(state->sink_commit)) {
        throw IndexerError(
            IndexerError::Type::INVALID_ARGUMENT,
            "IndexBuildBatchConfig: sink_factory and sink_commit must be set "
            "together (either both null for the default RocksDB path, or "
            "both non-null for the distributed SST path).");
    }
    return state;
}

static void finalize_batch_result(BatchWriteState* state,
                                  IndexBuildBatchResult* out) {
    out->results = std::move(*state->results);
    out->metrics = state->metrics;
    out->metrics.files_enqueued = state->num_files;

    out->extra_visitors.resize(state->num_files);
    for (std::size_t i = 0; i < state->num_files; ++i) {
        auto& job_opt = (*state->parsed_jobs)[i];
        if (job_opt && !job_opt->extra_visitors.empty()) {
            out->extra_visitors[i] = std::move(job_opt->extra_visitors);
        }
    }

    for (const auto& r : out->results) {
        if (r.was_skipped) {
            out->skipped++;
        } else if (r.success) {
            out->indexed++;
            out->total_events += r.events_processed;
        } else {
            out->failed++;
        }
    }
}

static void run_rebuild_root_summaries(const std::string& index_path) {
    IndexDatabase db(index_path);
    auto writer = db.begin_write();
    writer->rebuild_root_summaries();
    writer->commit();
}

static coro::CoroTask<IndexBuildBatchResult> run_single_batch(
    CoroScope* scope, IndexBuildBatchConfig chunk_config) {
    auto state = init_batch_write_state(chunk_config);
    co_await run_streaming_pipeline(scope, state.get());
    IndexBuildBatchResult partial;
    finalize_batch_result(state.get(), &partial);
    co_return partial;
}

static void merge_partial_into(IndexBuildBatchResult& out,
                               IndexBuildBatchResult partial) {
    for (auto& r : partial.results) {
        out.results.push_back(std::move(r));
    }
    out.indexed += partial.indexed;
    out.skipped += partial.skipped;
    out.failed += partial.failed;
    out.total_events += partial.total_events;
    out.metrics.parse_ns += partial.metrics.parse_ns;
    out.metrics.write_ns += partial.metrics.write_ns;
    out.metrics.files_enqueued += partial.metrics.files_enqueued;
    out.metrics.files_parsed += partial.metrics.files_parsed;
    out.metrics.files_written += partial.metrics.files_written;
    for (auto& ev : partial.extra_visitors) {
        out.extra_visitors.push_back(std::move(ev));
    }
}

static coro::CoroTask<IndexBuildBatchResult> run_batch_write_pipeline(
    CoroScope* scope, std::shared_ptr<IndexBuildBatchConfig> config_ptr) {
    const bool do_rebuild = config_ptr->rebuild_root_summaries;
    const std::size_t flush_every = config_ptr->flush_every_files;
    const std::size_t total = config_ptr->file_paths.size();
    const std::size_t chunk_size =
        (flush_every > 0 && flush_every < total) ? flush_every : total;

    IndexBuildBatchResult result;
    const auto index_path = determine_index_path(config_ptr->file_paths.front(),
                                                 config_ptr->index_dir);

    const std::size_t num_sub_batches = (total + chunk_size - 1) / chunk_size;
    std::size_t sub_batch_idx = 0;
    for (std::size_t start = 0; start < total; start += chunk_size) {
        const std::size_t end = std::min(start + chunk_size, total);
        DFTRACER_UTILS_LOG_INFO(
            "IndexBatch: sub-batch %zu/%zu begin (files %zu..%zu of %zu)",
            sub_batch_idx + 1, num_sub_batches, start, end - 1, total);
        IndexBuildBatchConfig chunk_config;
        chunk_config.file_paths.assign(
            config_ptr->file_paths.begin() + static_cast<std::ptrdiff_t>(start),
            config_ptr->file_paths.begin() + static_cast<std::ptrdiff_t>(end));
        if (!config_ptr->preassigned_file_ids.empty()) {
            chunk_config.preassigned_file_ids.assign(
                config_ptr->preassigned_file_ids.begin() +
                    static_cast<std::ptrdiff_t>(start),
                config_ptr->preassigned_file_ids.begin() +
                    static_cast<std::ptrdiff_t>(end));
        }
        if (!config_ptr->file_slices.empty()) {
            chunk_config.file_slices.assign(
                config_ptr->file_slices.begin() +
                    static_cast<std::ptrdiff_t>(start),
                config_ptr->file_slices.begin() +
                    static_cast<std::ptrdiff_t>(end));
        }
        chunk_config.sink_factory = config_ptr->sink_factory;
        chunk_config.sink_commit = config_ptr->sink_commit;
        chunk_config.index_dir = config_ptr->index_dir;
        chunk_config.checkpoint_size = config_ptr->checkpoint_size;
        chunk_config.parallelism = config_ptr->parallelism;
        chunk_config.force_rebuild = config_ptr->force_rebuild;
        chunk_config.build_manifest = config_ptr->build_manifest;
        chunk_config.bloom_config = config_ptr->bloom_config;
        chunk_config.bloom_dimensions = config_ptr->bloom_dimensions;
        chunk_config.use_batch_write = true;
        chunk_config.rebuild_root_summaries = false;
        chunk_config.dft_visitor_factory = config_ptr->dft_visitor_factory;

        auto partial =
            co_await run_single_batch(scope, std::move(chunk_config));
        if (config_ptr->extra_visitors_drain) {
            auto drained = std::move(partial.extra_visitors);
            partial.extra_visitors.clear();
            config_ptr->extra_visitors_drain(std::move(drained));
        }
        DFTRACER_UTILS_LOG_INFO(
            "IndexBatch: sub-batch %zu/%zu complete (indexed=%zu skipped=%zu "
            "failed=%zu)",
            sub_batch_idx + 1, num_sub_batches, partial.indexed,
            partial.skipped, partial.failed);
        merge_partial_into(result, std::move(partial));
        ++sub_batch_idx;
    }

    config_ptr.reset();

    if (do_rebuild) {
        run_rebuild_root_summaries(index_path);
    }

    co_return result;
}

coro::CoroTask<IndexBuildBatchResult> IndexBatchBuilderUtility::process(
    CoroScope* scope, std::shared_ptr<IndexBuildBatchConfig> config_ptr) {
    DFTRACER_UTILS_TRACE_SCOPE("build index batch");
    if (!config_ptr || config_ptr->file_paths.empty()) {
        co_return IndexBuildBatchResult{};
    }
    if (config_ptr->use_batch_write) {
        co_return co_await run_batch_write_pipeline(scope,
                                                    std::move(config_ptr));
    }
    co_return co_await process_batch_per_file(scope, std::move(config_ptr));
}

}  // namespace dftracer::utils::utilities::indexer
