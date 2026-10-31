#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/memory_budget.h>
#include <dftracer/utils/common/scoped_fd.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/file_admission.h>
#include <dftracer/utils/index/build/file_group.h>
#include <dftracer/utils/index/build/index_batch_writer.h>
#include <dftracer/utils/index/build/index_fold_driver.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/rowset_fold.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/source.h>
#include <dftracer/utils/index/store/error.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace dftracer::utils::index::build {

using trace::internal::determine_index_path;

namespace {

struct PreparedFile {
    std::size_t index;
    std::string file_path;
    std::string logical_path;
    std::string index_path;
    std::uint64_t file_hash = 0;
    std::uint64_t file_mtime = 0;
    std::int64_t file_mtime_ns = 0;
    std::uint64_t file_size = 0;
    int file_id = 0;
    /// The parse worker takes the caller's open descriptor, if it passed one.
    dstd::SharedPtr<ScopedFd> input_fd;
    /// The id was reserved by this build, so its tiers need no clearing.
    bool fresh_id = false;
};

struct ParsedBloomJob {
    PreparedFile identity;
    IndexBuildResult result;
    index::gzip::GzipBuildArtifacts artifacts;
    // The intern feeds the folds during the parse only; write reads the
    // already-resolved chunk/dictionary state, so the folds outlive it.
    std::unique_ptr<dftracer::utils::dstd::StringIntern> intern;
    std::unique_ptr<index::extensions::BloomFold> bloom_fold;
    std::unique_ptr<index::extensions::RowSetFold> rowset_fold;
    std::unique_ptr<index::schemas::dft::agg::AggregationFold> agg_fold;
    std::unique_ptr<index::extensions::PluginBuilders> plugin_builders;
    std::optional<index::schemas::dft::agg::AggFoldOutput> agg_output;
    const index::RecordSchema* schema = nullptr;
};

PreparedFile prepare_file(std::size_t i, const std::string& file_path,
                          const std::string& index_path, int file_id) {
    PreparedFile pf;
    pf.index = i;
    pf.file_path = file_path;
    pf.logical_path = index::store::internal::get_logical_path(file_path);
    pf.index_path = index_path;
    pf.file_id = file_id;
    return pf;
}

// Ids only: a file's registry record is written with its data.
std::vector<PreparedFile> prepare_file_identities(
    const std::string& index_path, const std::vector<std::string>& file_paths) {
    index::store::IndexDatabase db(index_path);
    // Ids come from a counter that only grows, so one at or past it was never
    // used in this index.
    const int first_fresh = db.reserve_file_id_range(0);
    const auto ids = db.assign_file_ids(file_paths);
    std::vector<PreparedFile> prepared;
    prepared.reserve(file_paths.size());
    for (std::size_t i = 0; i < file_paths.size(); ++i) {
        prepared.push_back(prepare_file(i, file_paths[i], index_path, ids[i]));
        prepared.back().fresh_id = ids[i] >= first_fresh;
    }
    return prepared;
}

}  // namespace

struct Admission {
    Admission(std::uint64_t budget, std::size_t workers,
              std::uint64_t sink_bytes_each, std::size_t sinks,
              std::size_t file_count)
        : files(budget, workers, sink_bytes_each * sinks, file_count) {}

    FileAdmission files;
    bool spill = false;
    /// RecordSchema id for every file; empty detects each file's.
    std::string schema;
    std::atomic<std::size_t> spilled{0};
    std::atomic<std::size_t> max_spill_runs{0};
    FoldBufferPool buffers;
};

struct BatchWriteState {
    std::shared_ptr<std::vector<IndexBuildResult>> results;
    std::shared_ptr<std::vector<std::optional<ParsedBloomJob>>> parsed_jobs;
    std::shared_ptr<std::vector<PreparedFile>> prepared;
    std::shared_ptr<std::vector<std::string>> bloom_dims;
    std::string index_path;
    IndexBuildBatchMetrics metrics;
    index::build::ChunkIndexerConfig bloom_config;
    std::size_t num_files = 0;
    std::vector<std::uint64_t> file_costs;
    std::size_t parallelism = 0;
    std::size_t checkpoint_size = 0;
    bool build_bloom = true;
    bool tier_only = false;
    std::uint64_t memory_budget = 0;
    std::string schema;
    IndexBuildBatchConfig::AggFoldFactory agg_fold_factory;
    IndexBuildBatchConfig::ProgressFn progress;
};

static std::string basename_of(const std::string& path) {
    auto pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

struct BlockWriter {
    const index::Index* index = nullptr;
    index::build::BatchWriterMetrics* metrics = nullptr;
};

static coro::CoroTask<void> commit_group(BlockWriter* writer,
                                         std::vector<IndexBuildResult>* results,
                                         index::build::FileGroup& group) {
    if (group.size() == 0) co_return;
    const auto w0 = std::chrono::steady_clock::now();
    auto result = co_await group.commit();
    for (const auto& failure : result.failures) {
        (*results)[failure.tag].success = false;
        (*results)[failure.tag].error_message = failure.message;
        DFTRACER_UTILS_LOG_ERROR(
            "Failed to write index for %s: %s; file dropped from index",
            failure.path.c_str(), failure.message.c_str());
    }
    writer->metrics->files_written.fetch_add(result.published,
                                             std::memory_order_relaxed);
    writer->metrics->batches_committed.fetch_add(result.publishes,
                                                 std::memory_order_relaxed);
    writer->metrics->write_ns.fetch_add(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - w0)
                .count()),
        std::memory_order_relaxed);
}

static coro::CoroTask<void> parse_and_write_worker(
    CoroScope* scope, std::atomic<std::size_t>* next_block_ptr,
    const dstd::Vector<std::size_t>* block_starts,
    std::atomic<std::size_t>* done_ptr,
    std::vector<IndexBuildResult>* results_ptr,
    std::vector<std::optional<ParsedBloomJob>>* parsed_jobs_ptr,
    std::vector<PreparedFile>* prepared_ptr, std::size_t checkpoint_size,
    index::build::ChunkIndexerConfig bloom_config,
    std::atomic<std::uint64_t>* parse_ns_ptr,
    const IndexBuildBatchConfig::AggFoldFactory* agg_fold_factory_ptr,
    bool build_bloom, bool tier_only,
    const std::vector<std::uint64_t>* costs_ptr,
    const IndexBuildBatchConfig::ProgressFn* progress_ptr, Admission* admission,
    BlockWriter* writer) {
    namespace gzip_indexer = index::gzip;

    index::build::FileGroup group;
    while (true) {
        const auto block =
            next_block_ptr->fetch_add(1, std::memory_order_relaxed);
        if (block + 1 >= block_starts->size()) break;
        const std::size_t block_begin = (*block_starts)[block];
        const std::size_t block_end = (*block_starts)[block + 1];

        for (std::size_t idx = block_begin; idx < block_end; ++idx) {
            auto& pf = (*prepared_ptr)[idx];
            dstd::SharedPtr<ScopedFd> input_fd = std::move(pf.input_fd);
            struct stat input_stat{};
            FileAdmission::Ticket ticket;
            bool admitted = false;
            if (!costs_ptr->empty()) {
                ticket = co_await admission->files.admit(
                    FileAdmission::plaintext_estimate(0, (*costs_ptr)[idx]));
                admitted = true;
            }
            if (!input_fd)
                input_fd = dstd::make_shared<ScopedFd>(
                    ::open(pf.file_path.c_str(), O_RDONLY));
            const int fd = input_fd->get();
            if (fd >= 0 && ::fstat(fd, &input_stat) == 0) {
                pf.file_size = static_cast<std::uint64_t>(input_stat.st_size);
                pf.file_mtime = static_cast<std::uint64_t>(input_stat.st_mtime);
                pf.file_mtime_ns = stat_mtime_ns(input_stat);
            }
            if (!admitted)
                ticket = co_await admission->files.admit(
                    FileAdmission::plaintext_estimate(0, pf.file_size));
            IndexBuildResult result;
            result.file_path = pf.file_path;
            result.index_path = pf.index_path;
            auto t0 = std::chrono::steady_clock::now();

            ParsedBloomJob job;
            dstd::Optional<index::FileBuild> build;
            bool parse_ok = false;
            try {
                if (fd < 0)
                    throw DFTUtilsException::cat(ErrorCode::Io, "cannot open ",
                                                 pf.file_path);
                pf.file_hash = index::store::internal::calculate_file_hash(
                    fd, pf.file_size);
                job.identity = pf;
                // The build exists before the folds, so one that spills
                // writes its runs into the file it is parsing.
                auto started = writer->index->build(
                    pf.logical_path,
                    index::SourceStat{pf.file_mtime, pf.file_size,
                                      pf.file_hash});
                if (!started) {
                    result.error_message =
                        std::string(started.error().message());
                } else {
                    build.emplace(std::move(*started));
                    namespace views = trace::views::detail;
                    index::gzip::CheckpointIndexer::VisitorList visitors;
                    const auto s0 = std::chrono::steady_clock::now();
                    const index::RecordSchema& schema =
                        admission->schema.empty()
                            ? index::detect_file_schema(pf.file_path, fd,
                                                        pf.file_size,
                                                        pf.file_mtime_ns)
                            : index::get_schema(admission->schema);
                    writer->metrics->schema_ns.fetch_add(
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<
                                std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - s0)
                                .count()),
                        std::memory_order_relaxed);
                    job.schema = &schema;
                    const auto file_config =
                        index::build::for_schema(bloom_config, schema);

                    std::vector<views::Fold*> fold_ptrs;
                    std::optional<index::build::IndexFoldDriver> driver;
                    {
                        job.intern = std::make_unique<
                            dftracer::utils::dstd::StringIntern>();
                        if (build_bloom) {
                            job.bloom_fold =
                                std::make_unique<index::extensions::BloomFold>(
                                    *job.intern, file_config);
                            job.bloom_fold->set_file_bytes(pf.file_size);
                            if (admission->spill)
                                job.bloom_fold->enable_spill(
                                    ticket.elastic_bytes(), *build);
                            fold_ptrs.push_back(job.bloom_fold.get());
                        }
                        if (!tier_only) {
                            job.rowset_fold =
                                std::make_unique<index::extensions::RowSetFold>(
                                    *job.intern,
                                    index::indexed_rowsets(schema));
                            fold_ptrs.push_back(job.rowset_fold.get());
                        }
                        if (agg_fold_factory_ptr && *agg_fold_factory_ptr) {
                            job.agg_fold = (*agg_fold_factory_ptr)(*job.intern,
                                                                   pf.file_id);
                            job.agg_fold->write_through(*build);
                            fold_ptrs.push_back(job.agg_fold.get());
                        }
                        driver.emplace(*job.intern, fold_ptrs, pf.file_path,
                                       pf.index_path);
                        driver->set_record_schema(schema);
                        driver->share_buffers(admission->buffers);
                        driver->set_parse_gate(admission->files.gate());
                        if (build_bloom) {
                            auto exts = index::extensions::plugin_extensions();
                            if (!exts.empty()) {
                                job.plugin_builders = std::make_unique<
                                    index::extensions::PluginBuilders>(
                                    std::move(exts));
                                driver->set_plugin_builders(
                                    job.plugin_builders.get());
                            }
                        }
                        visitors.emplace_back(*driver);
                    }

                    auto arts =
                        co_await gzip_indexer::build_gzip_index_artifacts(
                            fd, pf.file_path, checkpoint_size, visitors, scope,
                            nullptr, admission->files.inflate_ways());
                    if (!arts) {
                        result.error_message =
                            "Failed to build gzip index artifacts";
                    } else {
                        job.artifacts = std::move(*arts);
                        result.total_lines =
                            static_cast<std::size_t>(job.artifacts.total_lines);
                        result.chunks_processed = job.artifacts.members.size();
                        // Publish the dictionary's pending entries now that
                        // every member is parsed. Bloom needs none.
                        if (driver) driver->seal();
                        if (job.bloom_fold) {
                            result.events_processed = static_cast<std::size_t>(
                                job.bloom_fold->total_events());
                            if (job.bloom_fold->spill_runs() > 0) {
                                admission->spilled.fetch_add(
                                    1, std::memory_order_relaxed);
                                const std::size_t runs =
                                    job.bloom_fold->spill_runs();
                                std::size_t seen =
                                    admission->max_spill_runs.load(
                                        std::memory_order_relaxed);
                                while (runs > seen &&
                                       !admission->max_spill_runs
                                            .compare_exchange_weak(
                                                seen, runs,
                                                std::memory_order_relaxed)) {
                                }
                                DFTRACER_UTILS_LOG_INFO(
                                    "IndexBatch: %s spilled %zu runs",
                                    basename_of(pf.file_path).c_str(),
                                    job.bloom_fold->spill_runs());
                            }
                        }
                        if (job.agg_fold) {
                            index::schemas::dft::agg::AggFoldOutput ao;
                            ao.file_path = pf.file_path;
                            ao.min_time_bucket =
                                job.agg_fold->min_time_bucket();
                            ao.max_time_bucket =
                                job.agg_fold->max_time_bucket();
                            ao.events_processed =
                                job.agg_fold->events_processed();
                            job.agg_output = std::move(ao);
                        }
                        result.index_created = true;
                        result.success = true;
                        job.result = result;
                        parse_ok = true;
                    }
                }
            } catch (const std::exception& e) {
                result.error_message = e.what();
            }
            input_fd.reset();

            auto t1 = std::chrono::steady_clock::now();
            auto file_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count());
            parse_ns_ptr->fetch_add(file_ns, std::memory_order_relaxed);

            // Per-file detail stays at DEBUG. INFO gets a heartbeat every ~5%.
            const std::size_t done =
                done_ptr->fetch_add(1, std::memory_order_relaxed) + 1;
            const std::size_t total = prepared_ptr->size();
            DFTRACER_UTILS_LOG_DEBUG(
                "IndexBatch: parsed %zu/%zu %s (%.1fs, %zu events)", done,
                total, basename_of(pf.file_path).c_str(),
                static_cast<double>(file_ns) / 1e9, result.events_processed);
            const std::size_t step = total < 20 ? 1 : total / 20;
            if (done == total || done % step == 0) {
                DFTRACER_UTILS_LOG_INFO("IndexBatch: parsed %zu/%zu files",
                                        done, total);
            }
            // Bounds the per-file callback/GIL cost.
            if (progress_ptr) {
                const std::size_t pstep = total < 50 ? 1 : total / 50;
                if (done == total || done % pstep == 0) {
                    (*progress_ptr)(done, total);
                }
            }

            if (!parse_ok) {
                (*results_ptr)[idx] = std::move(result);
                continue;
            }

            const auto w0 = std::chrono::steady_clock::now();
            {
                index::build::ParsedIndexJob write_job;
                write_job.file_path = pf.file_path;
                write_job.fresh_id = pf.fresh_id;
                write_job.artifacts = std::move(job.artifacts);
                write_job.intern = std::move(job.intern);
                write_job.bloom_fold = std::move(job.bloom_fold);
                write_job.rowset_fold = std::move(job.rowset_fold);
                write_job.agg_fold = std::move(job.agg_fold);
                write_job.plugin_builders = std::move(job.plugin_builders);
                write_job.success = true;
                write_job.tier_only = tier_only;
                write_job.schema = job.schema;

                ParsedBloomJob holder;
                holder.identity = pf;
                holder.agg_output = std::move(job.agg_output);
                holder.result = result;
                (*parsed_jobs_ptr)[idx] = std::move(holder);
                (*results_ptr)[idx] = std::move(result);

                if (index::build::fill_index_job(*build, write_job, ticket)) {
                    write_job.error_message = co_await group.add(
                        std::move(*build), idx, pf.logical_path);
                    write_job.success = write_job.error_message.empty();
                }
                if (!write_job.success) {
                    (*results_ptr)[idx].success = false;
                    (*results_ptr)[idx].error_message = write_job.error_message;
                }
            }
            writer->metrics->write_ns.fetch_add(
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - w0)
                        .count()),
                std::memory_order_relaxed);
            ticket = {};
            if (group.full()) co_await commit_group(writer, results_ptr, group);
        }
        co_await commit_group(writer, results_ptr, group);
    }
    co_return;
}

dstd::Vector<std::size_t> cut_blocks(std::size_t files,
                                     std::span<const std::uint64_t> costs,
                                     std::size_t workers) {
    dstd::Vector<std::size_t> starts{0};
    if (files == 0) return starts;
    const std::size_t blocks =
        std::min(files, std::max<std::size_t>(1, workers));
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < files; ++i)
        total += costs.empty() ? 1 : std::max<std::uint64_t>(costs[i], 1);
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < files; ++i) {
        const std::size_t open_for = i - starts.back();
        const bool share = seen * blocks >= total * starts.size();
        if (open_for > 0 && share) starts.push_back(i);
        seen += costs.empty() ? 1 : std::max<std::uint64_t>(costs[i], 1);
    }
    starts.push_back(files);
    return starts;
}

// A file's state is freed as soon as it is written, so memory grows with the
// workers, not the files.
static coro::CoroTask<void> run_streaming_pipeline(CoroScope* scope,
                                                   BatchWriteState* state) {
    std::size_t parse_workers = std::max<std::size_t>(1, state->parallelism);
    const auto block_starts = dstd::make_shared<dstd::Vector<std::size_t>>(
        cut_blocks(state->num_files, state->file_costs, parse_workers));
    DFTRACER_UTILS_LOG_INFO(
        "IndexBatch: streaming pipeline begin (%zu files, workers=%zu "
        "blocks=%zu)",
        state->num_files, parse_workers, block_starts->size() - 1);

    auto writer_metrics = std::make_shared<index::build::BatchWriterMetrics>();

    // A lookup per file stalled a build on Lustre for minutes.
    index::load_index_schemas(state->index_path);
    auto writer_index = index::Index::open(
        index::store::internal::normalize_index_root(state->index_path));
    if (!writer_index) {
        for (auto& result : *state->results) {
            result.success = false;
            result.error_message = std::string(writer_index.error().message());
        }
        co_return;
    }

    constexpr std::uint64_t NO_BUDGET = memory::NO_SPILL_BUDGET;
    const std::uint64_t budget =
        state->memory_budget == NO_BUDGET
            ? NO_BUDGET
            : memory::resolve_spill_budget(state->memory_budget);
    const auto plan = FileAdmission::plan_sinks(
        budget, parse_workers,
        g_max_sink_bytes.load(std::memory_order_relaxed));
    parse_workers = plan.workers;
    auto admission = std::make_shared<Admission>(
        budget, parse_workers, plan.sink_bytes,
        std::min(state->num_files, parse_workers), state->num_files);
    admission->spill = budget != NO_BUDGET;
    admission->schema = state->schema;
    auto* admission_ptr = admission.get();
    auto next_block = std::make_shared<std::atomic<std::size_t>>(0);
    auto files_done = std::make_shared<std::atomic<std::size_t>>(0);
    auto parse_ns = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto bloom_config_holder =
        std::make_shared<index::build::ChunkIndexerConfig>(state->bloom_config);

    BlockWriter block_writer;
    block_writer.metrics = writer_metrics.get();
    block_writer.index = &*writer_index;
    auto* writer_ptr = &block_writer;

    const auto* block_starts_ptr = block_starts.get();
    auto* next_block_ptr = next_block.get();
    auto* done_ptr = files_done.get();
    auto* parse_ns_ptr = parse_ns.get();
    auto* results_ptr = state->results.get();
    auto* parsed_jobs_ptr = state->parsed_jobs.get();
    auto* prepared_ptr = state->prepared.get();
    const auto* bloom_config_ptr = bloom_config_holder.get();
    const auto checkpoint_size = state->checkpoint_size;
    const bool build_bloom = state->build_bloom;
    const bool tier_only = state->tier_only;
    const IndexBuildBatchConfig::AggFoldFactory* agg_fold_factory_ptr =
        state->agg_fold_factory ? &state->agg_fold_factory : nullptr;
    const IndexBuildBatchConfig::ProgressFn* progress_ptr =
        state->progress ? &state->progress : nullptr;

    const auto* costs_ptr = &state->file_costs;
    auto pipeline_scope =
        [parse_workers, block_starts_ptr, next_block_ptr, done_ptr,
         parse_ns_ptr, results_ptr, parsed_jobs_ptr, prepared_ptr,
         checkpoint_size, bloom_config_ptr, agg_fold_factory_ptr, build_bloom,
         tier_only, costs_ptr, admission_ptr, writer_ptr,
         progress_ptr](CoroScope& child) -> coro::CoroTask<void> {
        for (std::size_t w = 0; w < parse_workers; ++w) {
            child.spawn(
                [block_starts_ptr, next_block_ptr, done_ptr, parse_ns_ptr,
                 results_ptr, parsed_jobs_ptr, prepared_ptr, checkpoint_size,
                 bloom_config_ptr, agg_fold_factory_ptr, build_bloom, tier_only,
                 costs_ptr, progress_ptr, admission_ptr,
                 writer_ptr](CoroScope& own_scope) -> coro::CoroTask<void> {
                    co_await parse_and_write_worker(
                        &own_scope, next_block_ptr, block_starts_ptr, done_ptr,
                        results_ptr, parsed_jobs_ptr, prepared_ptr,
                        checkpoint_size, *bloom_config_ptr, parse_ns_ptr,
                        agg_fold_factory_ptr, build_bloom, tier_only, costs_ptr,
                        progress_ptr, admission_ptr, writer_ptr);
                });
        }
        co_return;
    };
    co_await scope->scope(pipeline_scope);

    state->metrics.parse_ns = parse_ns->load(std::memory_order_relaxed);

    state->metrics.files_parsed = state->num_files;
    state->metrics.write_ns =
        writer_metrics->write_ns.load(std::memory_order_relaxed);
    state->metrics.schema_ns =
        writer_metrics->schema_ns.load(std::memory_order_relaxed);
    state->metrics.files_written =
        writer_metrics->files_written.load(std::memory_order_relaxed);
    state->metrics.max_files_in_flight = admission->files.max_in_flight();
    state->metrics.files_spilled =
        admission->spilled.load(std::memory_order_relaxed);
    state->metrics.max_spill_runs =
        admission->max_spill_runs.load(std::memory_order_relaxed);
    DFTRACER_UTILS_LOG_INFO(
        "IndexBatch: streaming pipeline complete (parsed=%zu written=%zu)",
        state->num_files, state->metrics.files_written);
    co_return;
}

static std::unique_ptr<BatchWriteState> init_batch_write_state(
    IndexBuildBatchConfig& config) {
    auto state = std::make_unique<BatchWriteState>();
    state->num_files = config.file_paths.size();
    state->file_costs = config.file_costs;
    state->parallelism = config.parallelism;
    state->checkpoint_size = config.checkpoint_size;
    state->bloom_config = config.bloom_config;
    state->build_bloom = config.build_bloom;
    state->tier_only = config.tier_only;
    state->memory_budget = config.memory_budget;
    state->schema = config.schema;
    state->progress = config.progress;
    state->bloom_dims = std::make_shared<std::vector<std::string>>(
        config.bloom_dimensions.empty()
            ? std::vector<std::string>(DEFAULT_BLOOM_DIMENSIONS.begin(),
                                       DEFAULT_BLOOM_DIMENSIONS.end())
            : std::move(config.bloom_dimensions));
    state->results =
        std::make_shared<std::vector<IndexBuildResult>>(state->num_files);
    state->index_path =
        determine_index_path(config.file_paths.front(), config.index_dir);
    if (!config.preassigned_file_ids.empty()) {
        if (config.preassigned_file_ids.size() != config.file_paths.size()) {
            throw index::store::IndexerError(
                index::store::IndexerError::Type::InvalidArgument,
                "preassigned_file_ids.size() must match file_paths.size()");
        }
        // Distributed path: the coordinator assigned the ids.
        std::vector<PreparedFile> prepared;
        prepared.reserve(config.file_paths.size());
        for (std::size_t i = 0; i < config.file_paths.size(); ++i) {
            auto pf = prepare_file(i, config.file_paths[i], state->index_path,
                                   config.preassigned_file_ids[i]);
            pf.fresh_id =
                config.preassigned_file_ids[i] >= config.first_fresh_file_id;
            if (!config.input_fds.empty())
                pf.input_fd = std::move(config.input_fds[i]);
            prepared.push_back(std::move(pf));
        }
        state->prepared =
            std::make_shared<std::vector<PreparedFile>>(std::move(prepared));
    } else {
        state->prepared = std::make_shared<std::vector<PreparedFile>>(
            prepare_file_identities(state->index_path, config.file_paths));
        if (!config.input_fds.empty()) {
            auto& prepared = *state->prepared;
            for (std::size_t i = 0; i < prepared.size(); ++i)
                prepared[i].input_fd = std::move(config.input_fds[i]);
        }
    }
    state->parsed_jobs =
        std::make_shared<std::vector<std::optional<ParsedBloomJob>>>(
            state->num_files);
    if (config.agg_fold_factory) {
        state->agg_fold_factory = std::move(config.agg_fold_factory);
    }
    return state;
}

static void finalize_batch_result(BatchWriteState* state,
                                  IndexBuildBatchResult* out) {
    out->results = std::move(*state->results);
    out->metrics = state->metrics;
    out->metrics.files_enqueued = state->num_files;

    for (std::size_t i = 0; i < state->num_files; ++i) {
        auto& job_opt = (*state->parsed_jobs)[i];
        if (job_opt && job_opt->agg_output) {
            out->agg_outputs.push_back(std::move(*job_opt->agg_output));
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

static coro::CoroTask<IndexBuildBatchResult> run_single_batch(
    CoroScope* scope, IndexBuildBatchConfig chunk_config) {
    auto state = init_batch_write_state(chunk_config);
    co_await run_streaming_pipeline(scope, state.get());
    IndexBuildBatchResult partial;
    finalize_batch_result(state.get(), &partial);
    co_return partial;
}

static coro::CoroTask<IndexBuildBatchResult> run_batch_write_pipeline(
    CoroScope* scope, std::shared_ptr<IndexBuildBatchConfig> config_ptr) {
    IndexBuildBatchConfig config = std::move(*config_ptr);
    config_ptr.reset();
    auto result = co_await run_single_batch(scope, std::move(config));
    co_return result;
}

coro::CoroTask<IndexBuildBatchResult> BatchBuilder::process(
    CoroScope* scope, std::shared_ptr<IndexBuildBatchConfig> config_ptr) {
    DFTRACER_UTILS_TRACE_SCOPE("build index batch");
    if (!config_ptr || config_ptr->file_paths.empty()) {
        co_return IndexBuildBatchResult{};
    }
    // The pipeline writes one index; files whose indexes live in different
    // roots build one root at a time, each into its own.
    const IndexBuildBatchConfig& config = *config_ptr;
    std::vector<std::string> roots;
    std::vector<std::size_t> root_of(config.file_paths.size());
    for (std::size_t i = 0; i < config.file_paths.size(); ++i) {
        const std::string root =
            determine_index_path(config.file_paths[i], config.index_dir);
        auto it = std::find(roots.begin(), roots.end(), root);
        root_of[i] = static_cast<std::size_t>(it - roots.begin());
        if (it == roots.end()) roots.push_back(root);
    }
    if (roots.size() <= 1)
        co_return co_await run_batch_write_pipeline(scope,
                                                    std::move(config_ptr));

    IndexBuildBatchResult out;
    out.results.resize(config.file_paths.size());
    for (std::size_t r = 0; r < roots.size(); ++r) {
        auto part = std::make_shared<IndexBuildBatchConfig>(config);
        part->file_paths.clear();
        part->preassigned_file_ids.clear();
        part->input_fds.clear();
        part->file_costs.clear();
        std::vector<std::size_t> at;
        for (std::size_t i = 0; i < config.file_paths.size(); ++i) {
            if (root_of[i] != r) continue;
            at.push_back(i);
            part->file_paths.push_back(config.file_paths[i]);
            if (!config.preassigned_file_ids.empty())
                part->preassigned_file_ids.push_back(
                    config.preassigned_file_ids[i]);
            if (!config.input_fds.empty())
                part->input_fds.push_back(std::move(config_ptr->input_fds[i]));
            if (!config.file_costs.empty())
                part->file_costs.push_back(config.file_costs[i]);
        }
        auto got = co_await run_batch_write_pipeline(scope, std::move(part));
        for (std::size_t k = 0; k < at.size() && k < got.results.size(); ++k)
            out.results[at[k]] = std::move(got.results[k]);
        out.indexed += got.indexed;
        out.skipped += got.skipped;
        out.failed += got.failed;
        out.total_events += got.total_events;
        auto& m = out.metrics;
        m.parse_ns += got.metrics.parse_ns;
        m.write_ns += got.metrics.write_ns;
        m.schema_ns += got.metrics.schema_ns;
        m.files_enqueued += got.metrics.files_enqueued;
        m.files_parsed += got.metrics.files_parsed;
        m.files_written += got.metrics.files_written;
        m.max_files_in_flight =
            std::max(m.max_files_in_flight, got.metrics.max_files_in_flight);
        m.files_spilled += got.metrics.files_spilled;
        m.max_spill_runs =
            std::max(m.max_spill_runs, got.metrics.max_spill_runs);
        for (auto& a : got.agg_outputs) out.agg_outputs.push_back(std::move(a));
    }
    co_return out;
}

}  // namespace dftracer::utils::index::build
