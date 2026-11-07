// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/memory_budget.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/file_admission.h>
#include <dftracer/utils/index/build/file_group.h>
#include <dftracer/utils/index/build/index_batch_writer.h>
#include <dftracer/utils/index/build/index_fold_driver.h>
#include <dftracer/utils/index/build/new_members.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/rowset_fold.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/source.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/io/ops.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>

namespace dftracer::utils::index::build {

using trace::internal::determine_index_path;

namespace {

// False when the inflate pass left a gap between the records, so a member after
// the gap is not proven to follow the ones before it.
bool covers_every_byte(
    const dstd::Vector<index::gzip::GzipMemberRecord>& members,
    std::uint64_t from, std::uint64_t to) {
    std::uint64_t at = from;
    for (const auto& member : members) {
        if (member.c_offset != at) return false;
        at += member.c_size;
    }
    return at == to;
}

/// Leaves the parse of a file with no new whole member.
struct NothingNew {};

struct PreparedFile {
    std::size_t index;
    dstd::String file_path;
    dstd::String logical_path;
    dstd::String index_path;
    std::uint64_t file_hash = 0;
    std::uint64_t file_mtime = 0;
    std::int64_t file_mtime_ns = 0;
    std::uint64_t file_size = 0;
    /// The parse worker takes the caller's open descriptor, if it passed one.
    dstd::SharedPtr<dstd::File> input_fd;
};

struct ParsedBloomJob {
    PreparedFile identity;
    IndexBuildResult result;
    index::gzip::GzipBuildArtifacts artifacts;
    // The intern feeds the folds during the parse only; write reads the
    // already-resolved chunk/dictionary state, so the folds outlive it.
    dstd::UniquePtr<dftracer::utils::dstd::StringIntern> intern;
    dstd::UniquePtr<index::extensions::BloomFold> bloom_fold;
    dstd::UniquePtr<index::extensions::RowSetFold> rowset_fold;
    dstd::UniquePtr<index::schemas::dft::agg::AggregationFold> agg_fold;
    dstd::UniquePtr<index::extensions::PluginBuilders> plugin_builders;
    dstd::Optional<index::schemas::dft::agg::AggFoldOutput> agg_output;
    const index::RecordSchema* schema = nullptr;
};

PreparedFile prepare_file(std::size_t i, const dstd::String& file_path,
                          const dstd::String& index_path) {
    PreparedFile pf;
    pf.index = i;
    pf.file_path = file_path;
    pf.logical_path = index::store::internal::get_logical_path(file_path);
    pf.index_path = index_path;
    return pf;
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
    dstd::String schema;
    bool force_rebuild = false;
    bool whole_members = false;
    bool walk_members = false;
    std::atomic<std::size_t> spilled{0};
    std::atomic<std::size_t> max_spill_runs{0};
    FoldBufferPool buffers;
};

struct BatchWriteState {
    dstd::SharedPtr<dstd::Vector<IndexBuildResult>> results;
    dstd::SharedPtr<dstd::Vector<dstd::Optional<ParsedBloomJob>>> parsed_jobs;
    dstd::SharedPtr<dstd::Vector<PreparedFile>> prepared;
    dstd::SharedPtr<dstd::Vector<dstd::String>> bloom_dims;
    dstd::String index_path;
    IndexBuildBatchMetrics metrics;
    index::build::ChunkIndexerConfig bloom_config;
    std::size_t file_count = 0;
    dstd::Vector<std::uint64_t> file_costs;
    std::size_t parallelism = 0;
    std::size_t checkpoint_size = 0;
    bool force_rebuild = false;
    bool whole_members = false;
    bool walk_members = false;
    bool build_bloom = true;
    bool tier_only = false;
    std::uint64_t memory_budget = 0;
    dstd::String schema;
    IndexBuildBatchConfig::AggFoldFactory agg_fold_factory;
    IndexBuildBatchConfig::ProgressFn progress;
};

static dstd::String basename_of(const dstd::String& path) {
    auto pos = path.find_last_of('/');
    return pos == dstd::String::npos ? path : path.substr(pos + 1);
}

struct BlockWriter {
    const index::Index* index = nullptr;
    /// The index as it was when the build started, so a file it does not hold
    /// is new and its commit has no old tiers to clear.
    const index::Snapshot* snapshot = nullptr;
    index::build::BatchWriterMetrics* metrics = nullptr;
};

static coro::CoroTask<void> commit_group(
    BlockWriter* writer, dstd::Vector<IndexBuildResult>* results,
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
    coro::Scope* scope, std::atomic<std::size_t>* next_block_ptr,
    const dstd::Vector<std::size_t>* block_starts,
    std::atomic<std::size_t>* done_ptr,
    dstd::Vector<IndexBuildResult>* results_ptr,
    dstd::Vector<dstd::Optional<ParsedBloomJob>>* parsed_jobs_ptr,
    dstd::Vector<PreparedFile>* prepared_ptr, std::size_t checkpoint_size,
    index::build::ChunkIndexerConfig bloom_config,
    std::atomic<std::uint64_t>* parse_ns_ptr,
    const IndexBuildBatchConfig::AggFoldFactory* agg_fold_factory_ptr,
    bool build_bloom, bool tier_only,
    const dstd::Vector<std::uint64_t>* costs_ptr,
    const IndexBuildBatchConfig::ProgressFn* progress_ptr, Admission* admission,
    BlockWriter* writer) {
    index::build::FileGroup group;
    while (true) {
        const auto block =
            next_block_ptr->fetch_add(1, std::memory_order_relaxed);
        if (block + 1 >= block_starts->size()) break;
        const std::size_t block_begin = (*block_starts)[block];
        const std::size_t block_end = (*block_starts)[block + 1];

        for (std::size_t idx = block_begin; idx < block_end; ++idx) {
            auto& pf = (*prepared_ptr)[idx];
            dstd::SharedPtr<dstd::File> input_fd = std::move(pf.input_fd);
            struct stat input_stat{};
            FileAdmission::Ticket ticket;
            bool admitted = false;
            if (!costs_ptr->empty()) {
                ticket = co_await admission->files.admit(
                    FileAdmission::plaintext_estimate(0, (*costs_ptr)[idx]));
                admitted = true;
            }
            if (!input_fd) {
                const dstd::OpenOptions open_options;
                auto opening = runtime::io::open_file(
                    dstd::PathView(pf.file_path), open_options);
                auto opened = co_await std::move(opening);
                if (opened)
                    input_fd =
                        dstd::make_shared<dstd::File>(std::move(*opened));
            }
            const int fd = input_fd ? input_fd->native_handle() : -1;
            ssize_t stat_result = -1;
            if (fd >= 0)
                stat_result = co_await runtime::io::fstat(fd, &input_stat);
            if (stat_result == 0) {
                pf.file_size = static_cast<std::uint64_t>(input_stat.st_size);
                pf.file_mtime = static_cast<std::uint64_t>(input_stat.st_mtime);
                pf.file_mtime_ns = common::stat_mtime_ns(input_stat);
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
            bool is_appending = false;
            NewMembers new_members;
            index::gzip::GzipMemberSlice member_slice;
            dstd::Optional<index::FileRef> appended_file;
            const bool fresh =
                !writer->snapshot->file(pf.logical_path).has_value();
            try {
                if (fd < 0)
                    throw common::DFTUtilsException::cat(
                        common::ErrorCode::Io, "cannot open ", pf.file_path);
                // A file that grew by whole members adds only those members.
                const auto known = writer->snapshot->file(pf.logical_path);
                dstd::Optional<index::store::FileMetadataResult> stored;
                if (known && !tier_only && !admission->force_rebuild &&
                    pf.file_size > known->stat().size &&
                    index::extensions::plugin_extensions().empty()) {
                    stored = common::unwrap(
                        index::file_metadata(*writer->snapshot, *known));
                    if (stored && !stored->truncated &&
                        !stored->has_open_line &&
                        (checkpoint_size == 0 ||
                         stored->checkpoint_size == checkpoint_size) &&
                        index::store::internal::calculate_file_hash(
                            fd, known->stat().size) == known->stat().hash) {
                        is_appending = stored->next_member_idx > 0;
                    }
                }
                const bool whole_only =
                    is_appending || admission->whole_members;
                if (whole_only) {
                    const std::uint64_t from =
                        is_appending ? known->stat().size : 0;
                    auto finding = find_new_members(fd, from, pf.file_size,
                                                    admission->walk_members);
                    new_members = co_await std::move(finding);
                    if (new_members.members.empty()) {
                        result.success = true;
                        result.was_skipped = true;
                        throw NothingNew{};
                    }
                    member_slice.members = &new_members.members;
                    member_slice.member_end = new_members.members.size();
                    member_slice.c_end = new_members.c_end;
                    member_slice.idx_base =
                        is_appending ? stored->next_member_idx : 0;
                    pf.file_size = new_members.c_end;
                }
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
                        dstd::String(started.error().message());
                } else {
                    build.emplace(std::move(*started));
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

                    dstd::Vector<trace::views::detail::Fold*> fold_ptrs;
                    dstd::Optional<index::build::IndexFoldDriver> driver;
                    {
                        job.intern = dstd::make_unique<
                            dftracer::utils::dstd::StringIntern>();
                        if (build_bloom) {
                            job.bloom_fold =
                                dstd::make_unique<index::extensions::BloomFold>(
                                    *job.intern, file_config);
                            job.bloom_fold->set_file_bytes(pf.file_size);
                            if (is_appending) {
                                appended_file = known;
                                job.bloom_fold->continue_file(
                                    &*writer->snapshot, &*appended_file,
                                    member_slice.idx_base);
                            }
                            if (admission->spill)
                                job.bloom_fold->enable_spill(
                                    ticket.elastic_bytes(), *build);
                            fold_ptrs.push_back(job.bloom_fold.get());
                        }
                        if (!tier_only && !is_appending) {
                            job.rowset_fold = dstd::make_unique<
                                index::extensions::RowSetFold>(
                                *job.intern, index::indexed_rowsets(schema));
                            fold_ptrs.push_back(job.rowset_fold.get());
                        }
                        if (agg_fold_factory_ptr && *agg_fold_factory_ptr) {
                            job.agg_fold = (*agg_fold_factory_ptr)(*job.intern);
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
                                job.plugin_builders = dstd::make_unique<
                                    index::extensions::PluginBuilders>(
                                    std::move(exts));
                                driver->set_plugin_builders(
                                    job.plugin_builders.get());
                            }
                        }
                        visitors.emplace_back(*driver);
                    }

                    auto arts =
                        co_await index::gzip::build_gzip_index_artifacts(
                            fd, pf.file_path,
                            is_appending ? stored->checkpoint_size
                                         : checkpoint_size,
                            visitors, scope,
                            whole_only ? &member_slice : nullptr,
                            admission->files.inflate_ways());
                    if (whole_only && (!arts || arts->truncated ||
                                       !covers_every_byte(
                                           arts->members,
                                           new_members.members.front().c_offset,
                                           new_members.c_end))) {
                        // A false header or a hole in the new bytes. The walk
                        // finds the part that is whole.
                        result.needs_member_walk = !admission->walk_members;
                        result.error_message =
                            "a new gzip member of the file is not whole";
                    } else if (!arts) {
                        result.error_message =
                            "Failed to build gzip index artifacts";
                    } else {
                        if (is_appending) {
                            for (auto& m : arts->members) {
                                m.uc_offset += stored->max_bytes;
                                m.first_line_num += stored->line_count;
                                m.last_line_num += stored->line_count;
                            }
                            arts->total_lines += stored->line_count;
                            arts->total_uc_size += stored->max_bytes;
                            arts->has_mid_line_chunk =
                                stored->has_mid_line_chunk;
                        }
                        job.artifacts = std::move(*arts);
                        result.total_lines =
                            static_cast<std::size_t>(job.artifacts.total_lines);
                        result.chunks_processed = job.artifacts.members.size();
                        // Publish the dictionary's pending entries now that
                        // every member is parsed. Bloom needs none.
                        if (driver) {
                            driver->seal();
                            result.dropped_lines = driver->dropped_lines();
                            if (result.dropped_lines > 0)
                                DFTRACER_UTILS_LOG_WARN(
                                    "%s: %zu line(s) could not be read in full",
                                    pf.file_path.c_str(), result.dropped_lines);
                        }
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
                        result.was_appended = is_appending;
                        result.success = true;
                        job.result = result;
                        parse_ok = true;
                    }
                }
            } catch (const NothingNew&) {
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
                write_job.fresh = fresh;
                write_job.appended = is_appending;
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
                                     dstd::Span<const std::uint64_t> costs,
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
static coro::CoroTask<void> run_streaming_pipeline(BatchWriteState* state) {
    std::size_t parse_workers = std::max<std::size_t>(1, state->parallelism);
    const auto block_starts = dstd::make_shared<dstd::Vector<std::size_t>>(
        cut_blocks(state->file_count, state->file_costs, parse_workers));
    DFTRACER_UTILS_LOG_INFO(
        "IndexBatch: streaming pipeline begin (%zu files, workers=%zu "
        "blocks=%zu)",
        state->file_count, parse_workers, block_starts->size() - 1);

    auto writer_metrics = dstd::make_shared<index::build::BatchWriterMetrics>();

    // A lookup per file stalled a build on Lustre for minutes.
    index::load_index_schemas(state->index_path);
    auto writer_index =
        index::Index::open(index::normalize_index_root(state->index_path));
    if (!writer_index) {
        for (auto& result : *state->results) {
            result.success = false;
            result.error_message = dstd::String(writer_index.error().message());
        }
        co_return;
    }
    auto taken = writer_index->snapshot();
    if (!taken) {
        for (auto& result : *state->results) {
            result.success = false;
            result.error_message = dstd::String(taken.error().message());
        }
        co_return;
    }
    const index::Snapshot writer_snapshot = std::move(*taken);

    constexpr std::uint64_t NO_BUDGET = memory::NO_SPILL_BUDGET;
    const std::uint64_t budget =
        state->memory_budget == NO_BUDGET
            ? NO_BUDGET
            : memory::resolve_spill_budget(state->memory_budget);
    const auto plan = FileAdmission::plan_sinks(
        budget, parse_workers,
        g_max_sink_bytes.load(std::memory_order_relaxed));
    parse_workers = plan.workers;
    auto admission = dstd::make_shared<Admission>(
        budget, parse_workers, plan.sink_bytes,
        std::min(state->file_count, parse_workers), state->file_count);
    admission->spill = budget != NO_BUDGET;
    admission->schema = state->schema;
    admission->force_rebuild = state->force_rebuild;
    admission->whole_members = state->whole_members;
    admission->walk_members = state->walk_members;
    auto* admission_ptr = admission.get();
    auto next_block = dstd::make_shared<std::atomic<std::size_t>>(0);
    auto files_done = dstd::make_shared<std::atomic<std::size_t>>(0);
    auto parse_ns = dstd::make_shared<std::atomic<std::uint64_t>>(0);
    auto bloom_config_holder =
        dstd::make_shared<index::build::ChunkIndexerConfig>(
            state->bloom_config);

    BlockWriter block_writer;
    block_writer.metrics = writer_metrics.get();
    block_writer.index = &*writer_index;
    block_writer.snapshot = &writer_snapshot;
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
         progress_ptr](coro::Scope& child) -> coro::CoroTask<void> {
        for (std::size_t w = 0; w < parse_workers; ++w) {
            child.spawn([&child, block_starts_ptr, next_block_ptr, done_ptr,
                         parse_ns_ptr, results_ptr, parsed_jobs_ptr,
                         prepared_ptr, checkpoint_size, bloom_config_ptr,
                         agg_fold_factory_ptr, build_bloom, tier_only,
                         costs_ptr, progress_ptr, admission_ptr,
                         writer_ptr]() -> coro::CoroTask<void> {
                co_await parse_and_write_worker(
                    &child, next_block_ptr, block_starts_ptr, done_ptr,
                    results_ptr, parsed_jobs_ptr, prepared_ptr, checkpoint_size,
                    *bloom_config_ptr, parse_ns_ptr, agg_fold_factory_ptr,
                    build_bloom, tier_only, costs_ptr, progress_ptr,
                    admission_ptr, writer_ptr);
            });
        }
        co_return;
    };
    co_await coro::with_scope(std::move(pipeline_scope));

    state->metrics.parse_ns = parse_ns->load(std::memory_order_relaxed);

    state->metrics.files_parsed = state->file_count;
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
        state->file_count, state->metrics.files_written);
    co_return;
}

static dstd::UniquePtr<BatchWriteState> init_batch_write_state(
    IndexBuildBatchConfig& config) {
    auto state = dstd::make_unique<BatchWriteState>();
    state->file_count = config.file_paths.size();
    state->file_costs = config.file_costs;
    state->parallelism = config.parallelism;
    state->checkpoint_size = config.checkpoint_size;
    state->bloom_config = config.bloom_config;
    state->build_bloom = config.build_bloom;
    state->tier_only = config.tier_only;
    state->memory_budget = config.memory_budget;
    state->schema = config.schema;
    state->force_rebuild = config.force_rebuild;
    state->whole_members = config.whole_members;
    state->walk_members = config.walk_members;
    state->progress = std::move(config.progress);
    state->bloom_dims = dstd::make_shared<dstd::Vector<dstd::String>>(
        config.bloom_dimensions.empty()
            ? dstd::Vector<dstd::String>(DEFAULT_BLOOM_DIMENSIONS.begin(),
                                         DEFAULT_BLOOM_DIMENSIONS.end())
            : std::move(config.bloom_dimensions));
    state->results =
        dstd::make_shared<dstd::Vector<IndexBuildResult>>(state->file_count);
    state->index_path =
        determine_index_path(config.file_paths.front(), config.index_dir);
    dstd::Vector<PreparedFile> prepared;
    prepared.reserve(config.file_paths.size());
    for (std::size_t i = 0; i < config.file_paths.size(); ++i) {
        auto pf = prepare_file(i, config.file_paths[i], state->index_path);
        if (!config.input_fds.empty())
            pf.input_fd = std::move(config.input_fds[i]);
        prepared.push_back(std::move(pf));
    }
    state->prepared =
        dstd::make_shared<dstd::Vector<PreparedFile>>(std::move(prepared));
    state->parsed_jobs =
        dstd::make_shared<dstd::Vector<dstd::Optional<ParsedBloomJob>>>(
            state->file_count);
    if (config.agg_fold_factory) {
        state->agg_fold_factory = std::move(config.agg_fold_factory);
    }
    return state;
}

static void finalize_batch_result(BatchWriteState* state,
                                  IndexBuildBatchResult* out) {
    out->results = std::move(*state->results);
    out->metrics = state->metrics;
    out->metrics.files_enqueued = state->file_count;

    for (std::size_t i = 0; i < state->file_count; ++i) {
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
    IndexBuildBatchConfig chunk_config) {
    auto state = init_batch_write_state(chunk_config);
    co_await run_streaming_pipeline(state.get());
    IndexBuildBatchResult partial;
    finalize_batch_result(state.get(), &partial);
    co_return partial;
}

static coro::CoroTask<IndexBuildBatchResult> run_batch_write_pipeline(
    dstd::SharedPtr<IndexBuildBatchConfig> config_ptr) {
    IndexBuildBatchConfig config = std::move(*config_ptr);
    config_ptr.reset();
    auto result = co_await run_single_batch(std::move(config));
    co_return result;
}

coro::CoroTask<IndexBuildBatchResult> BatchBuilder::process(
    dstd::SharedPtr<IndexBuildBatchConfig> config_ptr) {
    DFTRACER_UTILS_TRACE_SCOPE("build index batch");
    if (!config_ptr || config_ptr->file_paths.empty()) {
        co_return IndexBuildBatchResult{};
    }
    // The pipeline writes one index; files whose indexes live in different
    // roots build one root at a time, each into its own.
    const IndexBuildBatchConfig& config = *config_ptr;
    dstd::Vector<dstd::String> roots;
    dstd::Vector<std::size_t> root_of(config.file_paths.size());
    for (std::size_t i = 0; i < config.file_paths.size(); ++i) {
        const dstd::String root =
            determine_index_path(config.file_paths[i], config.index_dir);
        auto it = std::find(roots.begin(), roots.end(), root);
        root_of[i] = static_cast<std::size_t>(it - roots.begin());
        if (it == roots.end()) roots.push_back(root);
    }
    if (roots.size() <= 1)
        co_return co_await run_batch_write_pipeline(std::move(config_ptr));

    IndexBuildBatchResult out;
    out.results.resize(config.file_paths.size());
    for (std::size_t r = 0; r < roots.size(); ++r) {
        auto part = dstd::make_shared<IndexBuildBatchConfig>();
        part->index_dir = config.index_dir;
        part->checkpoint_size = config.checkpoint_size;
        part->parallelism = config.parallelism;
        part->force_rebuild = config.force_rebuild;
        part->build_bloom = config.build_bloom;
        part->bloom_config = config.bloom_config;
        part->bloom_dimensions = config.bloom_dimensions;
        part->tier_only = config.tier_only;
        part->memory_budget = config.memory_budget;
        part->schema = config.schema;
        part->whole_members = config.whole_members;
        part->walk_members = config.walk_members;
        if (config.agg_fold_factory)
            part->agg_fold_factory =
                [factory = &config.agg_fold_factory](
                    dftracer::utils::dstd::StringIntern& build_intern) {
                    return (*factory)(build_intern);
                };
        if (config.progress)
            part->progress = [progress = &config.progress](std::size_t done,
                                                           std::size_t total) {
                (*progress)(done, total);
            };
        dstd::Vector<std::size_t> at;
        for (std::size_t i = 0; i < config.file_paths.size(); ++i) {
            if (root_of[i] != r) continue;
            at.push_back(i);
            part->file_paths.push_back(config.file_paths[i]);
            if (!config.input_fds.empty())
                part->input_fds.push_back(std::move(config_ptr->input_fds[i]));
            if (!config.file_costs.empty())
                part->file_costs.push_back(config.file_costs[i]);
        }
        auto got = co_await run_batch_write_pipeline(std::move(part));
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
