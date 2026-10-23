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
#include <dftracer/utils/index/build/index_batch_writer.h>
#include <dftracer/utils/index/build/index_fold_driver.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/rowset_fold.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/source.h>
#include <dftracer/utils/index/store/error.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
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
    /// The caller's open descriptor of the file, if it passed one. The parse
    /// worker takes it.
    dstd::SharedPtr<ScopedFd> input_fd;
    /// The id was reserved by this build, so the index holds no data for it
    /// and its tiers need no clearing.
    bool fresh_id = false;
    IndexBuildBatchConfig::FileSlice slice;
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
    // Ids come from a counter that only grows, so one at or past it before
    // the assignment was never used in this index.
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

// What the files of one build share: admission by the memory budget, the sink
// size, the spill location and the event buffers.
struct Admission {
    Admission(std::uint64_t budget, std::size_t workers,
              std::uint64_t sink_bytes_each, std::size_t sinks,
              std::size_t file_count)
        : sink_limit(sink_bytes_each),
          files(budget, workers, sink_bytes_each * sinks, file_count) {}

    /// Bytes a sink holds before its worker commits it.
    std::size_t sink_limit;
    FileAdmission files;
    /// Where a file's spill runs go; empty when files do not spill.
    std::string spill_root;
    /// RecordSchema id for every file; empty detects each file's.
    std::string schema;
    std::atomic<std::size_t> spilled{0};
    std::atomic<std::size_t> max_spill_runs{0};
    /// Event buffers shared by every file's fold driver, so a file starts
    /// with the capacity the files before it grew.
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
    std::string spill_dir;
    std::string schema;
    IndexBuildBatchConfig::AggFoldFactory agg_fold_factory;
    IndexBuildBatchConfig::SinkFactory sink_factory;
    IndexBuildBatchConfig::SinkCommitFn sink_commit;
    IndexBuildBatchConfig::SpillCommitFn spill_commit;
    IndexBuildBatchConfig::ProgressFn progress;
};

static std::string basename_of(const std::string& path) {
    auto pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

static fs::path spill_runs_dir(
    const std::vector<index::store::IndexDatabaseSstWriterContext::Artifacts>&
        runs) {
    for (const auto& run : runs)
        for (const auto& paths : run.sst)
            if (!paths.empty())
                return fs::path(paths.front()).parent_path().parent_path();
    return {};
}

void remove_spill_runs(
    const std::vector<index::store::IndexDatabaseSstWriterContext::Artifacts>&
        runs) {
    const fs::path dir = spill_runs_dir(runs);
    std::error_code ec;
    if (!dir.empty()) fs::remove_all(dir, ec);
}

void ingest_spill_runs(
    index::store::IndexDatabase& db,
    std::vector<index::store::IndexDatabaseSstWriterContext::Artifacts> runs) {
    const fs::path dir = spill_runs_dir(runs);
    struct Cleanup {
        const fs::path& dir;
        ~Cleanup() {
            std::error_code ec;
            if (!dir.empty()) fs::remove_all(dir, ec);
        }
    } cleanup{dir};
    index::store::SstArtifactRegistry registry;
    for (auto& run : runs) registry.append(std::move(run));
    db.bulk_ingest(registry);
}

// Where files are written: a sink per run of a worker's block, committed when
// the run ends so its SSTs cover its file ids and no other worker's.
struct BlockWriter {
    std::function<std::unique_ptr<index::store::IndexWrite>()> make_sink;
    std::function<void(index::store::IndexWrite&)> commit_sink;
    std::function<void(
        std::vector<index::store::IndexDatabaseSstWriterContext::Artifacts>)>
        commit_spill;
    index::build::BatchWriterMetrics* metrics = nullptr;
};

// The sink a worker is filling and what it holds: spill runs to ingest after
// it commits, and the files written into it.
struct OpenSink {
    std::unique_ptr<index::store::IndexWrite> sink;
    std::vector<index::build::SpilledFile> spilled;
    std::vector<std::size_t> written;
    std::uint64_t write_ns = 0;
};

static void commit_open_sink(BlockWriter* writer, OpenSink& open,
                             std::vector<IndexBuildResult>* results_ptr,
                             const std::vector<PreparedFile>* prepared_ptr) {
    const auto c0 = std::chrono::steady_clock::now();
    if (open.sink) {
        writer->commit_sink(*open.sink);
        open.sink.reset();
    }
    for (auto& file : open.spilled) {
        try {
            writer->commit_spill(std::move(file.runs));
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_ERROR(
                "Failed to ingest the spilled index of %s: %s",
                file.file_path.c_str(), e.what());
            for (auto idx : open.written)
                if ((*prepared_ptr)[idx].file_path == file.file_path) {
                    (*results_ptr)[idx].success = false;
                    (*results_ptr)[idx].error_message = e.what();
                }
        }
    }
    open.write_ns += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - c0)
            .count());
    writer->metrics->write_ns.fetch_add(open.write_ns,
                                        std::memory_order_relaxed);
    writer->metrics->files_written.fetch_add(open.written.size(),
                                             std::memory_order_relaxed);
    writer->metrics->batches_committed.fetch_add(1, std::memory_order_relaxed);
    open = {};
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

    while (true) {
        const auto block =
            next_block_ptr->fetch_add(1, std::memory_order_relaxed);
        if (block + 1 >= block_starts->size()) break;
        const std::size_t block_begin = (*block_starts)[block];
        const std::size_t block_end = (*block_starts)[block + 1];
        OpenSink open;

        for (std::size_t idx = block_begin; idx < block_end; ++idx) {
            auto& pf = (*prepared_ptr)[idx];
            // One open of the input serves the size and mtime, the hash, the
            // schema probe and the gzip index build.
            dstd::SharedPtr<ScopedFd> input_fd = std::move(pf.input_fd);
            // A file of known size takes its place in the budget before it is
            // opened; one of unknown size is opened to read its size first.
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
            bool parse_ok = false;
            try {
                if (fd < 0)
                    throw DFTUtilsException::cat(ErrorCode::Io, "cannot open ",
                                                 pf.file_path);
                pf.file_hash = index::store::internal::calculate_file_hash(
                    fd, pf.file_size);
                job.identity = pf;
                namespace views = trace::views::detail;
                index::gzip::CheckpointIndexer::VisitorList visitors;
                index::load_index_schemas(pf.index_path);
                const index::RecordSchema& schema =
                    admission->schema.empty()
                        ? index::detect_file_schema(
                              pf.file_path, fd, pf.file_size, pf.file_mtime_ns)
                        : index::get_schema(admission->schema);
                job.schema = &schema;
                const auto file_config =
                    index::build::for_schema(bloom_config, schema);

                // Bloom + hash are harvested by folds via an IndexFoldDriver.
                // The file-scoped folds are skipped for non-first slices of a
                // cross-rank split, exactly as the visitors were.
                std::vector<views::Fold*> fold_ptrs;
                std::optional<index::build::IndexFoldDriver> driver;
                if (!pf.slice.skip_file_scoped_writes) {
                    job.intern =
                        std::make_unique<dftracer::utils::dstd::StringIntern>();
                    if (build_bloom) {
                        job.bloom_fold =
                            std::make_unique<index::extensions::BloomFold>(
                                *job.intern, file_config);
                        job.bloom_fold->set_file_bytes(pf.file_size);
                        // A slice sees only part of the file's chunks, so it
                        // cannot tell which are finished.
                        if (!admission->spill_root.empty() &&
                            pf.slice.members == nullptr)
                            job.bloom_fold->enable_spill(
                                ticket.elastic_bytes(),
                                admission->spill_root + "/file_" +
                                    std::to_string(pf.file_id),
                                pf.file_id, pf.fresh_id);
                        fold_ptrs.push_back(job.bloom_fold.get());
                    }
                    if (!tier_only) {
                        job.rowset_fold =
                            std::make_unique<index::extensions::RowSetFold>(
                                *job.intern, index::indexed_rowsets(schema));
                        fold_ptrs.push_back(job.rowset_fold.get());
                    }
                    if (agg_fold_factory_ptr && *agg_fold_factory_ptr) {
                        job.agg_fold =
                            (*agg_fold_factory_ptr)(*job.intern, pf.file_id);
                        fold_ptrs.push_back(job.agg_fold.get());
                    }
                    driver.emplace(*job.intern, fold_ptrs, pf.file_path,
                                   pf.index_path);
                    driver->set_record_schema(schema);
                    driver->share_buffers(admission->buffers);
                    driver->set_parse_gate(admission->files.gate());
                    // A slice sees part of the chunks, and a file payload built
                    // from them could rule out the whole file.
                    if (build_bloom && pf.slice.members == nullptr) {
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

                gzip_indexer::GzipMemberSlice slice_arg;
                const gzip_indexer::GzipMemberSlice* slice_ptr = nullptr;
                if (pf.slice.members != nullptr &&
                    pf.slice.member_end > pf.slice.member_begin) {
                    slice_arg.members = pf.slice.members;
                    slice_arg.member_begin = pf.slice.member_begin;
                    slice_arg.member_end = pf.slice.member_end;
                    slice_ptr = &slice_arg;
                }
                auto arts = co_await gzip_indexer::build_gzip_index_artifacts(
                    fd, pf.file_path, checkpoint_size, visitors, scope,
                    slice_ptr, admission->files.inflate_ways());
                if (!arts) {
                    result.error_message =
                        "Failed to build gzip index artifacts";
                } else {
                    job.artifacts = std::move(*arts);
                    result.total_lines =
                        static_cast<std::size_t>(job.artifacts.total_lines);
                    result.chunks_processed = job.artifacts.members.size();
                    // Publish the dictionary's pending entries now that every
                    // member is parsed; bloom buckets by checkpoint and needs
                    // none.
                    if (driver) driver->seal();
                    if (job.bloom_fold) {
                        result.events_processed = static_cast<std::size_t>(
                            job.bloom_fold->total_events());
                        if (job.bloom_fold->spill_runs() > 0) {
                            admission->spilled.fetch_add(
                                1, std::memory_order_relaxed);
                            const std::size_t runs =
                                job.bloom_fold->spill_runs();
                            std::size_t seen = admission->max_spill_runs.load(
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
                    // Lift the aggregation fold's out-of-band outputs before it
                    // goes to the write phase; its metrics stay for write.
                    if (job.agg_fold) {
                        index::schemas::dft::agg::AggFoldOutput ao;
                        ao.file_path = pf.file_path;
                        ao.tracker = job.agg_fold->take_tracker();
                        ao.min_time_bucket = job.agg_fold->min_time_bucket();
                        ao.max_time_bucket = job.agg_fold->max_time_bucket();
                        ao.events_processed = job.agg_fold->events_processed();
                        job.agg_output = std::move(ao);
                    }
                    result.index_created = true;
                    result.success = true;
                    job.result = result;
                    parse_ok = true;
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

            // A large trace spends minutes per file here. Keep the per-file
            // detail at DEBUG and emit an INFO heartbeat only every ~5% so a
            // long build shows progress without a line per file.
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
            // Throttled to ~50 steps to bound the per-file callback/GIL cost.
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

            // Sliced rank with member_begin > 0: skip file-scoped channel send;
            // the aggregation fold output is kept in parsed_jobs[idx] for
            // downstream collection.
            if (pf.slice.skip_file_scoped_writes) {
                (*results_ptr)[idx] = std::move(result);
                (*parsed_jobs_ptr)[idx] = std::move(job);
                continue;
            }

            // Build the payload (move bloom/dict/aggregation into it), and
            // leave the result behind in parsed_jobs[idx].
            const auto w0 = std::chrono::steady_clock::now();
            {
                index::build::ParsedIndexJob write_job;
                write_job.file_id = pf.file_id;
                write_job.fresh_id = pf.fresh_id;
                write_job.file_path = pf.file_path;
                write_job.logical_path = pf.logical_path;
                write_job.record = {static_cast<std::uint32_t>(pf.file_id),
                                    pf.file_mtime, pf.file_hash, pf.file_size};
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

                // The file's state goes into the sink and is freed here, so a
                // worker holds one file's state and the serialized rest of
                // its block.
                if (!open.sink) open.sink = writer->make_sink();
                if (index::build::write_index_job(*open.sink, write_job,
                                                  open.spilled)) {
                    open.written.push_back(idx);
                } else {
                    (*results_ptr)[idx].success = false;
                    (*results_ptr)[idx].error_message = write_job.error_message;
                }
            }
            open.write_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - w0)
                    .count());
            ticket = {};
            // A sink that has grown past the budget commits and the next file
            // opens another: the ids of a block are consecutive, so the SSTs of
            // the two sinks still do not overlap.
            if (open.sink &&
                open.sink->buffered_bytes() >= admission->sink_limit)
                commit_open_sink(writer, open, results_ptr, prepared_ptr);
        }

        commit_open_sink(writer, open, results_ptr, prepared_ptr);
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
        // Cut before file i when the work so far reaches the next share.
        const bool share = seen * blocks >= total * starts.size();
        if (open_for > 0 && share) starts.push_back(i);
        seen += costs.empty() ? 1 : std::max<std::uint64_t>(costs[i], 1);
    }
    starts.push_back(files);
    return starts;
}

// Streaming parse + write pipeline. Each worker takes a block of consecutive
// files, parses them one after another, writes each into a sink as it finishes
// and commits the sink at the end of the block, or sooner when it has grown
// past the sink limit. A file's state is freed as soon as it is written, so
// memory grows with the workers, not the files, and the file ids of a block
// are consecutive, so its SSTs overlap no other block's.
static coro::CoroTask<void> run_streaming_pipeline(CoroScope* scope,
                                                   BatchWriteState* state) {
    std::size_t parse_workers = std::max<std::size_t>(1, state->parallelism);
    // One block of consecutive files per worker, so a worker's SSTs cover one
    // run of file ids and the ingest has about one SST per worker and key
    // prefix. Blocks hold about equal work, so the workers finish together.
    const auto block_starts = dstd::make_shared<dstd::Vector<std::size_t>>(
        cut_blocks(state->num_files, state->file_costs, parse_workers));
    DFTRACER_UTILS_LOG_INFO(
        "IndexBatch: streaming pipeline begin (%zu files, workers=%zu "
        "blocks=%zu)",
        state->num_files, parse_workers, block_starts->size() - 1);

    auto writer_metrics = std::make_shared<index::build::BatchWriterMetrics>();

    // Only open the RocksDB-backed DB when no external sink factory is
    // provided. The distributed SST path routes writes through caller-owned
    // SstWriterContext instances and must not hold a process-exclusive
    // RocksDB handle on the target index dir.
    std::shared_ptr<index::store::IndexDatabase> writer_db;
    if (!state->sink_factory) {
        writer_db =
            std::make_shared<index::store::IndexDatabase>(state->index_path);
    }

    // The workers' sinks hold up to their limit each, so the budget sets the
    // workers and the limit, and the files get what is left.
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
    if (budget != NO_BUDGET && (!state->sink_factory || state->spill_commit))
        admission->spill_root = state->spill_dir.empty()
                                    ? state->index_path + ".spill"
                                    : state->spill_dir;
    admission->schema = state->schema;
    auto* admission_ptr = admission.get();
    auto next_block = std::make_shared<std::atomic<std::size_t>>(0);
    auto files_done = std::make_shared<std::atomic<std::size_t>>(0);
    auto parse_ns = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto bloom_config_holder =
        std::make_shared<index::build::ChunkIndexerConfig>(state->bloom_config);

    BlockWriter block_writer;
    block_writer.metrics = writer_metrics.get();
    if (state->sink_factory) {
        block_writer.make_sink = state->sink_factory;
        block_writer.commit_sink = state->sink_commit;
        block_writer.commit_spill = state->spill_commit;
    } else {
        auto* db = writer_db.get();
        block_writer.make_sink = [db] { return db->begin_write(); };
        block_writer.commit_sink = [](index::store::IndexWrite& sink) {
            static_cast<index::store::IndexDatabaseWriterContext&>(sink)
                .commit();
        };
        block_writer.commit_spill =
            [db](std::vector<
                 index::store::IndexDatabaseSstWriterContext::Artifacts>
                     runs) { ingest_spill_runs(*db, std::move(runs)); };
    }
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

    if (!admission->spill_root.empty()) {
        std::error_code ec;
        fs::remove(admission->spill_root, ec);
    }
    state->metrics.parse_ns = parse_ns->load(std::memory_order_relaxed);

    state->metrics.files_parsed = state->num_files;
    state->metrics.write_ns =
        writer_metrics->write_ns.load(std::memory_order_relaxed);
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
    state->spill_dir = config.spill_dir;
    state->schema = config.schema;
    state->spill_commit = config.spill_commit;
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
    if (!config.file_slices.empty() &&
        config.file_slices.size() != config.file_paths.size()) {
        throw index::store::IndexerError(
            index::store::IndexerError::Type::InvalidArgument,
            "file_slices.size() must match file_paths.size() (or be empty)");
    }
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
            if (!config.file_slices.empty()) pf.slice = config.file_slices[i];
            if (!config.input_fds.empty())
                pf.input_fd = std::move(config.input_fds[i]);
            prepared.push_back(std::move(pf));
        }
        state->prepared =
            std::make_shared<std::vector<PreparedFile>>(std::move(prepared));
    } else {
        state->prepared = std::make_shared<std::vector<PreparedFile>>(
            prepare_file_identities(state->index_path, config.file_paths));
        if (!config.file_slices.empty()) {
            auto& prepared = *state->prepared;
            for (std::size_t i = 0; i < prepared.size(); ++i) {
                prepared[i].slice = config.file_slices[i];
            }
        }
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
    state->sink_factory = std::move(config.sink_factory);
    state->sink_commit = std::move(config.sink_commit);
    if (static_cast<bool>(state->sink_factory) !=
        static_cast<bool>(state->sink_commit)) {
        throw index::store::IndexerError(
            index::store::IndexerError::Type::InvalidArgument,
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
    out.metrics.files_spilled += partial.metrics.files_spilled;
    out.metrics.max_spill_runs =
        std::max(out.metrics.max_spill_runs, partial.metrics.max_spill_runs);
    out.metrics.max_files_in_flight = std::max(
        out.metrics.max_files_in_flight, partial.metrics.max_files_in_flight);
    for (auto& ao : partial.agg_outputs) {
        out.agg_outputs.push_back(std::move(ao));
    }
}

static coro::CoroTask<IndexBuildBatchResult> run_batch_write_pipeline(
    CoroScope* scope, std::shared_ptr<IndexBuildBatchConfig> config_ptr) {
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
        if (!config_ptr->input_fds.empty()) {
            chunk_config.input_fds.assign(
                std::make_move_iterator(config_ptr->input_fds.begin() +
                                        static_cast<std::ptrdiff_t>(start)),
                std::make_move_iterator(config_ptr->input_fds.begin() +
                                        static_cast<std::ptrdiff_t>(end)));
        }
        if (!config_ptr->file_costs.empty()) {
            chunk_config.file_costs.assign(
                config_ptr->file_costs.begin() +
                    static_cast<std::ptrdiff_t>(start),
                config_ptr->file_costs.begin() +
                    static_cast<std::ptrdiff_t>(end));
        }
        if (!config_ptr->file_slices.empty()) {
            chunk_config.file_slices.assign(
                config_ptr->file_slices.begin() +
                    static_cast<std::ptrdiff_t>(start),
                config_ptr->file_slices.begin() +
                    static_cast<std::ptrdiff_t>(end));
        }
        chunk_config.first_fresh_file_id = config_ptr->first_fresh_file_id;
        chunk_config.sink_factory = config_ptr->sink_factory;
        chunk_config.sink_commit = config_ptr->sink_commit;
        chunk_config.index_dir = config_ptr->index_dir;
        chunk_config.checkpoint_size = config_ptr->checkpoint_size;
        chunk_config.parallelism = config_ptr->parallelism;
        chunk_config.force_rebuild = config_ptr->force_rebuild;
        chunk_config.build_bloom = config_ptr->build_bloom;
        chunk_config.tier_only = config_ptr->tier_only;
        chunk_config.memory_budget = config_ptr->memory_budget;
        chunk_config.spill_dir = config_ptr->spill_dir;
        chunk_config.schema = config_ptr->schema;
        chunk_config.spill_commit = config_ptr->spill_commit;
        chunk_config.bloom_config = config_ptr->bloom_config;
        chunk_config.bloom_dimensions = config_ptr->bloom_dimensions;
        chunk_config.agg_fold_factory = config_ptr->agg_fold_factory;
        if (config_ptr->progress) {
            // Sub-batch `done` restarts at 0; offset it so the callback always
            // reports global (files_done, total) across the whole batch.
            auto user_progress = config_ptr->progress;
            const std::size_t offset = start;
            chunk_config.progress = [user_progress, offset, total](
                                        std::size_t done, std::size_t) {
                user_progress(offset + done, total);
            };
        }

        auto partial =
            co_await run_single_batch(scope, std::move(chunk_config));
        DFTRACER_UTILS_LOG_INFO(
            "IndexBatch: sub-batch %zu/%zu complete (indexed=%zu skipped=%zu "
            "failed=%zu)",
            sub_batch_idx + 1, num_sub_batches, partial.indexed,
            partial.skipped, partial.failed);
        merge_partial_into(result, std::move(partial));
        ++sub_batch_idx;
    }

    config_ptr.reset();

    co_return result;
}

coro::CoroTask<IndexBuildBatchResult> BatchBuilder::process(
    CoroScope* scope, std::shared_ptr<IndexBuildBatchConfig> config_ptr) {
    DFTRACER_UTILS_TRACE_SCOPE("build index batch");
    if (!config_ptr || config_ptr->file_paths.empty()) {
        co_return IndexBuildBatchResult{};
    }
    // The pipeline writes one index; files whose indexes live in different
    // roots build one root at a time, each into its own. A sink routes the
    // writes itself.
    const IndexBuildBatchConfig& config = *config_ptr;
    std::vector<std::string> roots;
    std::vector<std::size_t> root_of(config.file_paths.size());
    if (!config.sink_factory)
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
        part->file_slices.clear();
        part->preassigned_file_ids.clear();
        part->input_fds.clear();
        part->file_costs.clear();
        std::vector<std::size_t> at;
        for (std::size_t i = 0; i < config.file_paths.size(); ++i) {
            if (root_of[i] != r) continue;
            at.push_back(i);
            part->file_paths.push_back(config.file_paths[i]);
            if (!config.file_slices.empty())
                part->file_slices.push_back(config.file_slices[i]);
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
