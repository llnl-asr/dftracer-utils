#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_visitor.h>
#include <dftracer/utils/utilities/composites/dft/dft_event_dispatcher.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/group_writer_task.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/provenance_tracker.h>
#include <dftracer/utils/utilities/composites/dft/visitors/bloom_visitor.h>
#include <dftracer/utils/utilities/composites/dft/visitors/hash_table_visitor.h>
#include <dftracer/utils/utilities/composites/dft/visitors/manifest_visitor.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>
#include <dftracer/utils/utilities/fileio/chunk_writer.h>
#include <dftracer/utils/utilities/fileio/parallel/layout.h>
#include <dftracer/utils/utilities/fileio/parallel/merge.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_sst_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint.h>

#include <algorithm>
// #include <cstdlib>  // re-enable with the DFT_MOCK_PADDED_STRIPE_BYTES block
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::reorganize {

namespace {

constexpr std::size_t DEFAULT_FLUSH_BYTES = 32 * 1024 * 1024;
constexpr std::size_t BUFFER_HEADROOM_BYTES = 1 * 1024 * 1024;

coro::CoroTask<void> compress_to_gzip_member(int level, ByteView data,
                                             std::vector<unsigned char>& out) {
    out.clear();
    compression::zlib::ManualStreamingCompressorUtility comp(
        level, compression::zlib::CompressionFormat::GZIP);
    if (data.size() > 0) {
        auto gen = comp.compress(data);
        while (auto view = co_await gen.next()) {
            const auto* p =
                reinterpret_cast<const unsigned char*>(view->data());
            out.insert(out.end(), p, p + view->size());
        }
    }
    auto fin = comp.finalize_stream();
    while (auto view = co_await fin.next()) {
        const auto* p = reinterpret_cast<const unsigned char*>(view->data());
        out.insert(out.end(), p, p + view->size());
    }
    co_return;
}

std::string make_chunk_path(const std::string& dir, int index, bool compress) {
    return dir + "/chunk_chunk" + std::to_string(index) + ".pfw" +
           (compress ? ".gz" : "");
}

struct WorkerBuf {
    std::vector<char> payload;
    std::size_t lines_in_flush = 0;
    std::vector<std::size_t> flush_line_counts;
};

struct PendingSegment {
    int source_file_idx;
    int checkpoint_idx;
    std::size_t worker_idx;
    std::size_t flush_idx;
    std::size_t offset_in_flush;
    std::size_t count;
};

struct FlushTask {
    std::vector<char> payload;
    std::uint64_t uc_offset = 0;
    std::uint64_t line_count = 0;
    std::uint64_t first_line_num = 0;
    std::size_t dispatch_idx = 0;
};

struct IndexBatch {
    std::shared_ptr<std::string> payload;
    std::size_t dispatch_idx = 0;
    std::size_t worker_idx = 0;
    std::uint64_t c_offset = 0;
    std::uint64_t c_size = 0;
    std::uint64_t uc_offset = 0;
    std::uint64_t uc_size = 0;
    std::uint64_t line_count = 0;
    std::uint64_t first_line_num = 0;
};

constexpr std::size_t FLUSH_CHANNEL_CAPACITY = 3;
constexpr std::size_t INDEX_CHANNEL_CAPACITY = 8;

struct InlineIndexState {
    std::unique_ptr<visitors::BloomVisitor> bloom;
    std::unique_ptr<visitors::HashTableVisitor> hash_table;
    std::unique_ptr<visitors::ManifestVisitor> manifest;
    std::unique_ptr<aggregators::AggregationVisitor> aggregation;
    std::unique_ptr<DftEventDispatcher> dispatcher;
    std::shared_ptr<coro::Channel<IndexBatch>> index_channel;
    std::vector<IndexBatch> finalized_batches;
    int file_id = -1;
    std::unique_ptr<indexer::IndexDatabaseSstWriterContext> sink;
    std::uint64_t slice_uc_bytes = 0;
    std::uint64_t sink_uc_bytes = 0;
};

constexpr std::uint64_t SLICE_UC_THRESHOLD = 64ULL * 1024 * 1024;
constexpr std::uint64_t SINK_UC_THRESHOLD = 1ULL * 1024 * 1024 * 1024;

struct ChunkState {
    std::unique_ptr<fileio::parallel::ParallelWriter> writer;
    std::string output_path;
    fileio::parallel::LayoutInfo layout_info;
    std::size_t num_workers = 1;
    bool compress = false;
    int compression_level = Z_DEFAULT_COMPRESSION;
    std::size_t flush_threshold = 0;
    std::size_t buffer_capacity = 0;
    std::size_t bytes_uncompressed = 0;
    std::size_t events_written = 0;
    bool has_any_events = false;
    std::vector<WorkerBuf> workers;
    std::vector<PendingSegment> segments;
    std::vector<unsigned char> compressed_scratch;
    std::vector<std::shared_ptr<coro::Channel<FlushTask>>> flush_channels;

    bool inline_index_enabled = false;
    std::uint64_t inline_uc_dispatched = 0;
    std::uint64_t inline_lines_dispatched = 0;
    std::size_t inline_dispatch_counter = 0;
    InlineIndexState inline_index;
};

coro::CoroTask<bool> run_flusher(
    std::size_t worker_idx, std::shared_ptr<coro::Channel<FlushTask>> channel,
    ChunkState* st) {
    std::vector<unsigned char> scratch;
    while (auto task_opt = co_await channel->receive()) {
        FlushTask& task = *task_opt;
        if (task.payload.empty()) continue;
        ByteView view(task.payload.data(), task.payload.size());
        int rc;
        if (st->compress) {
            scratch.clear();
            co_await compress_to_gzip_member(st->compression_level, view,
                                             scratch);
            rc = co_await st->writer->write_chunk(
                worker_idx,
                ByteView(reinterpret_cast<const char*>(scratch.data()),
                         scratch.size()));
        } else {
            rc = co_await st->writer->write_chunk(worker_idx, view);
        }
        if (rc != 0) co_return false;

        if (st->inline_index_enabled) {
            auto member = st->writer->last_member(worker_idx);
            if (!member) co_return false;
            IndexBatch batch;
            batch.payload = std::make_shared<std::string>(task.payload.data(),
                                                          task.payload.size());
            batch.dispatch_idx = task.dispatch_idx;
            batch.worker_idx = worker_idx;
            batch.c_offset = member->offset;
            batch.c_size = member->length;
            batch.uc_offset = task.uc_offset;
            batch.uc_size = task.payload.size();
            batch.line_count = task.line_count;
            batch.first_line_num = task.first_line_num;
            if (!co_await st->inline_index.index_channel->send(
                    std::move(batch))) {
                co_return false;
            }
        }
    }
    co_return true;
}

void rotate_inline_sink(InlineIndexState& idx,
                        const GroupWriterConfig& config) {
    auto a = idx.sink->commit();
    if (!a.empty()) config.artifacts_queue->enqueue(std::move(a));
    const auto next_idx =
        config.batch_counter->fetch_add(1, std::memory_order_relaxed);
    idx.sink = std::make_unique<indexer::IndexDatabaseSstWriterContext>(
        config.staging_root, "inline_" + std::to_string(next_idx));
    idx.sink_uc_bytes = 0;
}

void flush_slice_visitors(InlineIndexState& idx,
                          const GroupWriterConfig& config) {
    if (!idx.sink || idx.file_id < 0) return;
    if (idx.bloom) {
        idx.bloom->flush_per_checkpoint_to_sink(*idx.sink, idx.file_id);
    }
    if (idx.manifest) {
        idx.manifest->flush_per_checkpoint_to_sink(*idx.sink, idx.file_id);
    }
    idx.sink_uc_bytes += idx.slice_uc_bytes;
    idx.slice_uc_bytes = 0;
    if (idx.sink_uc_bytes >= SINK_UC_THRESHOLD) {
        rotate_inline_sink(idx, config);
    }
}

coro::CoroTask<bool> run_index_feeder(ChunkState* st,
                                      const GroupWriterConfig* config) {
    auto& idx = st->inline_index;
    while (auto batch_opt = co_await idx.index_channel->receive()) {
        IndexBatch batch = std::move(*batch_opt);
        if (batch.payload && !batch.payload->empty()) {
            co_await idx.dispatcher->on_chunk(batch.payload->data(),
                                              batch.payload->size(),
                                              batch.dispatch_idx);
            co_await idx.dispatcher->on_checkpoint(batch.dispatch_idx);
        }
        idx.slice_uc_bytes += batch.uc_size;
        batch.payload.reset();
        idx.finalized_batches.push_back(std::move(batch));
        if (idx.slice_uc_bytes >= SLICE_UC_THRESHOLD) {
            co_await idx.dispatcher->flush();
            flush_slice_visitors(idx, *config);
        }
    }
    co_return true;
}

coro::CoroTask<bool> dispatch_flush(ChunkState& st, std::size_t w) {
    auto& ww = st.workers[w];
    if (ww.payload.empty()) co_return true;
    ww.flush_line_counts.push_back(ww.lines_in_flush);
    FlushTask task;
    task.payload = std::move(ww.payload);
    task.line_count = ww.lines_in_flush;
    task.uc_offset = st.inline_uc_dispatched;
    task.first_line_num = st.inline_lines_dispatched;
    task.dispatch_idx = st.inline_dispatch_counter++;
    st.inline_uc_dispatched += task.payload.size();
    st.inline_lines_dispatched += task.line_count;
    ww.payload = std::vector<char>();
    ww.payload.reserve(st.buffer_capacity);
    ww.lines_in_flush = 0;
    bool ok = co_await st.flush_channels[w]->send(std::move(task));
    co_return ok;
}

coro::CoroTask<bool> write_section(ChunkState& st, ByteView data,
                                   bool is_footer) {
    ByteView payload = data;
    if (st.compress) {
        co_await compress_to_gzip_member(st.compression_level, data,
                                         st.compressed_scratch);
        payload = ByteView(
            reinterpret_cast<const char*>(st.compressed_scratch.data()),
            st.compressed_scratch.size());
    }
    int rc = is_footer ? co_await st.writer->write_footer(payload)
                       : co_await st.writer->write_header(payload);
    co_return rc == 0;
}

coro::CoroTask<bool> open_chunk(ChunkState& st, const std::string& path,
                                bool compress, int compression_level,
                                std::size_t chunk_size_bytes,
                                std::size_t baseline_workers, CoroScope* scope,
                                const GroupWriterConfig& config) {
    st.output_path = path;
    st.compress = compress;
    st.compression_level = compression_level;
    st.bytes_uncompressed = 0;
    st.events_written = 0;
    st.has_any_events = false;
    st.segments.clear();

    st.layout_info = fileio::parallel::detect_layout(path);

    // Local validation aid: force padded-striped layout when no Lustre is
    // available. Uncomment to exercise the padded-striped writer path.
    // if (const char* mock = std::getenv("DFT_MOCK_PADDED_STRIPE_BYTES")) {
    //     const auto sz =
    //         static_cast<std::size_t>(std::strtoull(mock, nullptr, 10));
    //     if (sz >= fileio::parallel::MIN_PADDED_STRIPE_BYTES) {
    //         st.layout_info.layout = fileio::parallel::FileLayout::STRIPED;
    //         st.layout_info.stripe_size = sz;
    //     }
    // }

    if (st.layout_info.layout == fileio::parallel::FileLayout::STRIPED &&
        st.layout_info.stripe_size == 0) {
        st.layout_info.layout = fileio::parallel::FileLayout::SHARDED;
    }

    const bool uses_padded =
        st.layout_info.layout == fileio::parallel::FileLayout::STRIPED &&
        compress &&
        st.layout_info.stripe_size >= fileio::parallel::MIN_PADDED_STRIPE_BYTES;
    const bool uses_sharded =
        st.layout_info.layout == fileio::parallel::FileLayout::SHARDED;
    // Plain striped writes at an atomic offset so cross-worker order is
    // non-deterministic; keep one worker so we can resolve absolute line
    // numbers. Padded-striped and sharded layouts expose deterministic
    // worker slotting so we can fan out.
    const std::size_t effective_baseline =
        (uses_padded || uses_sharded)
            ? std::max<std::size_t>(baseline_workers, 1)
            : 1;
    const auto sizing = fileio::parallel::compute_writer_sizing(
        st.layout_info, effective_baseline, DEFAULT_FLUSH_BYTES,
        BUFFER_HEADROOM_BYTES, uses_padded);
    st.num_workers = sizing.num_workers;
    st.flush_threshold = sizing.flush_threshold;
    st.buffer_capacity = sizing.buffer_capacity;
    if (chunk_size_bytes > 0 && st.flush_threshold > chunk_size_bytes) {
        st.flush_threshold = chunk_size_bytes;
    }

    st.workers.clear();
    st.workers.resize(st.num_workers);
    for (auto& w : st.workers) {
        w.payload.reserve(st.buffer_capacity);
    }

    fileio::parallel::WriterConfig wcfg;
    wcfg.layout = st.layout_info.layout;
    wcfg.stripe_size = st.layout_info.stripe_size;
    wcfg.gzip = compress;
    st.writer = fileio::parallel::make_writer(wcfg);

    if (co_await st.writer->open(path, st.num_workers, compress, scope) != 0) {
        co_return false;
    }

    st.inline_index_enabled =
        !config.index_dir.empty() && config.artifacts_queue &&
        config.batch_counter &&
        (st.layout_info.layout == fileio::parallel::FileLayout::STRIPED ||
         st.layout_info.layout == fileio::parallel::FileLayout::SHARDED);
    st.inline_uc_dispatched = 0;
    st.inline_lines_dispatched = 0;
    st.inline_dispatch_counter = 0;
    if (st.inline_index_enabled) {
        st.inline_index = InlineIndexState{};
        st.inline_index.bloom = std::make_unique<visitors::BloomVisitor>(
            config.bloom_config, config.bloom_dimensions);
        st.inline_index.hash_table =
            std::make_unique<visitors::HashTableVisitor>();
        st.inline_index.manifest =
            std::make_unique<visitors::ManifestVisitor>();
        if (config.with_aggregation) {
            aggregators::AggregationConfig agg_cfg;
            agg_cfg.time_interval_us =
                static_cast<std::uint64_t>(config.agg_time_interval_us);
            agg_cfg.compute_statistics = true;
            agg_cfg.track_process_parents = true;
            agg_cfg.track_default_args = true;
            const std::size_t batch_idx =
                config.batch_counter->fetch_add(1, std::memory_order_relaxed);
            st.inline_index.aggregation =
                std::make_unique<aggregators::AggregationVisitor>(
                    config.staging_root, "agg_" + std::to_string(batch_idx),
                    /*config_hash=*/0u, agg_cfg, path);
        }
        DftEventDispatcher::VisitorList visitors;
        visitors.emplace_back(*st.inline_index.bloom);
        visitors.emplace_back(*st.inline_index.hash_table);
        visitors.emplace_back(*st.inline_index.manifest);
        if (st.inline_index.aggregation) {
            visitors.emplace_back(*st.inline_index.aggregation);
        }
        st.inline_index.dispatcher = std::make_unique<DftEventDispatcher>(
            std::move(visitors), /*force_serial=*/true);
        st.inline_index.dispatcher->begin(0);
        st.inline_index.index_channel =
            coro::make_channel<IndexBatch>(INDEX_CHANNEL_CAPACITY);
    }

    const char header[] = "[\n";
    if (!co_await write_section(
            st, ByteView(reinterpret_cast<const char*>(header), 2), false)) {
        co_return false;
    }
    co_return true;
}

void emit_segments(const ChunkState& st, int chunk_idx,
                   ProvenanceTracker& prov) {
    std::vector<std::vector<std::size_t>> abs_base(st.num_workers);
    for (std::size_t w = 0; w < st.num_workers; ++w) {
        abs_base[w].assign(st.workers[w].flush_line_counts.size(), 0);
    }
    std::size_t cum = 0;
    const bool striped_parallel =
        st.layout_info.layout == fileio::parallel::FileLayout::STRIPED &&
        st.num_workers > 1;
    if (striped_parallel) {
        std::size_t max_flushes = 0;
        for (std::size_t w = 0; w < st.num_workers; ++w) {
            max_flushes =
                std::max(max_flushes, st.workers[w].flush_line_counts.size());
        }
        for (std::size_t k = 0; k < max_flushes; ++k) {
            for (std::size_t w = 0; w < st.num_workers; ++w) {
                if (k >= st.workers[w].flush_line_counts.size()) continue;
                abs_base[w][k] = cum;
                cum += st.workers[w].flush_line_counts[k];
            }
        }
    } else {
        for (std::size_t w = 0; w < st.num_workers; ++w) {
            for (std::size_t k = 0; k < st.workers[w].flush_line_counts.size();
                 ++k) {
                abs_base[w][k] = cum;
                cum += st.workers[w].flush_line_counts[k];
            }
        }
    }
    for (const auto& seg : st.segments) {
        std::size_t abs_start =
            abs_base[seg.worker_idx][seg.flush_idx] + seg.offset_in_flush;
        std::size_t abs_end = abs_start + seg.count - 1;
        prov.record(seg.source_file_idx, seg.checkpoint_idx, chunk_idx,
                    static_cast<int>(abs_start), static_cast<int>(abs_end),
                    static_cast<int>(seg.count));
    }
}

coro::CoroTask<bool> dispatch_flush_all(ChunkState& st) {
    for (std::size_t w = 0; w < st.num_workers; ++w) {
        if (!co_await dispatch_flush(st, w)) co_return false;
    }
    co_return true;
}

// Append a single line (plus trailing '\n') to worker w's payload. Does NOT
// flush; caller decides when to flush.
void append_line(ChunkState& st, std::size_t w, ByteView line) {
    auto& ww = st.workers[w];
    const char* p = line.as<char>();
    ww.payload.insert(ww.payload.end(), p, p + line.size());
    ww.payload.push_back('\n');
    ww.lines_in_flush += 1;
    st.bytes_uncompressed += line.size() + 1;
    st.events_written += 1;
    st.has_any_events = true;
}

}  // namespace

coro::CoroTask<Result<GroupWriterResult>> run_group_writer(
    CoroScope* scope, GroupWriterConfig config) {
    auto result = std::make_unique<GroupWriterResult>();
    result->group_name = config.group_name;

    try {
        std::string group_output_dir =
            config.output_dir + "/" + config.group_name;
        if (!fs::exists(group_output_dir)) {
            fs::create_directories(group_output_dir);
        }

        auto provenance = std::make_unique<ProvenanceTracker>();

        int current_chunk_idx = 0;
        std::vector<fileio::ChunkInfo> chunks_info;
        std::unique_ptr<indexer::IndexDatabase> coord_db;
        bool any_chunk_inline_indexed = false;
        const bool inline_index_active = !config.index_dir.empty() &&
                                         config.artifacts_queue &&
                                         config.batch_counter;
        if (inline_index_active) {
            coord_db =
                std::make_unique<indexer::IndexDatabase>(config.index_dir);
            coord_db->init_schema();
        }

        const std::size_t baseline_workers =
            (scope && scope->get_executor())
                ? scope->get_executor()->get_num_threads()
                : 1;

        auto open_inline_sink = [&](ChunkState& cs, const std::string& path) {
            if (!cs.inline_index_enabled) return;
            // Sharded writers don't materialize the merged path until
            // finalize_chunk runs `merge_shards`. Touch it so register_files
            // can stat/hash it now.
            if (!fs::exists(path)) {
                std::ofstream(path).close();
            }
            std::vector<int> ids =
                coord_db->register_files({path}, /*build_manifest=*/true);
            cs.inline_index.file_id = ids.empty() ? -1 : ids.front();
            const auto idx =
                config.batch_counter->fetch_add(1, std::memory_order_relaxed);
            cs.inline_index.sink =
                std::make_unique<indexer::IndexDatabaseSstWriterContext>(
                    config.staging_root, "inline_" + std::to_string(idx));
            cs.inline_index.slice_uc_bytes = 0;
            cs.inline_index.sink_uc_bytes = 0;
        };

        constexpr std::size_t MAX_IN_FLIGHT_CHUNKS = 4;
        auto sync_mutex = std::make_shared<std::mutex>();
        auto inline_indexed_flag = std::make_shared<std::atomic<bool>>(false);
        auto sem = coro::make_channel<int>(MAX_IN_FLIGHT_CHUNKS);

        co_await scope->scope([&](CoroScope& group_scope)
                                  -> coro::CoroTask<void> {
            for (std::size_t i = 0; i < MAX_IN_FLIGHT_CHUNKS; ++i) {
                co_await sem->send(0);
            }

            auto cs = std::make_shared<ChunkState>();
            {
                const auto path = make_chunk_path(
                    group_output_dir, current_chunk_idx, config.compress);
                if (!co_await open_chunk(
                        *cs, path, config.compress, config.compression_level,
                        config.chunk_size_bytes, baseline_workers, &group_scope,
                        config)) {
                    throw DFTUtilsException(ErrorCode::IO,
                                            "Failed to open initial chunk");
                }
                open_inline_sink(*cs, path);
            }

            bool input_eof = false;
            while (!input_eof) {
                cs->flush_channels.clear();
                cs->flush_channels.reserve(cs->num_workers);
                for (std::size_t i = 0; i < cs->num_workers; ++i) {
                    cs->flush_channels.push_back(
                        coro::make_channel<FlushTask>(FLUSH_CHANNEL_CAPACITY));
                }

                co_await sem->receive();

                const int captured_chunk_idx = current_chunk_idx;
                const GroupWriterConfig* cfg_ptr = &config;
                ProvenanceTracker* prov_ptr = provenance.get();
                std::vector<fileio::ChunkInfo>* chunks_info_ptr = &chunks_info;
                GroupWriterResult* result_raw = result.get();
                auto sync_mtx = sync_mutex;
                auto indexed_flag = inline_indexed_flag;
                auto sem_release = sem;

                group_scope.spawn([cs, captured_chunk_idx, cfg_ptr, prov_ptr,
                                   chunks_info_ptr, result_raw, sync_mtx,
                                   indexed_flag,
                                   sem_release](CoroScope& orch_scope)
                                      -> coro::CoroTask<void> {
                    ChunkState* cs_p = cs.get();
                    co_await orch_scope.scope([cs_p,
                                               cfg_ptr](CoroScope& work_scope)
                                                  -> coro::CoroTask<void> {
                        if (cs_p->inline_index_enabled) {
                            work_scope.spawn(
                                [cs_p,
                                 cfg_ptr](CoroScope&) -> coro::CoroTask<void> {
                                    co_await run_index_feeder(cs_p, cfg_ptr);
                                });
                        }
                        co_await work_scope.scope([cs_p](CoroScope& flush_scope)
                                                      -> coro::CoroTask<void> {
                            for (std::size_t i = 0; i < cs_p->num_workers;
                                 ++i) {
                                auto ch = cs_p->flush_channels[i];
                                flush_scope.spawn(
                                    [i, ch,
                                     cs_p](CoroScope&) -> coro::CoroTask<void> {
                                        co_await run_flusher(i, ch, cs_p);
                                    });
                            }
                            co_return;
                        });
                        if (cs_p->inline_index_enabled &&
                            cs_p->inline_index.index_channel) {
                            cs_p->inline_index.index_channel->close();
                        }
                        co_return;
                    });

                    if (cs_p->inline_index_enabled) {
                        co_await cs_p->inline_index.dispatcher->flush();
                    }

                    {
                        std::lock_guard<std::mutex> lk(*sync_mtx);
                        emit_segments(*cs_p, captured_chunk_idx, *prov_ptr);
                    }
                    cs_p->segments.clear();

                    const char footer[] = "]\n";
                    if (!co_await write_section(
                            *cs_p,
                            ByteView(reinterpret_cast<const char*>(footer), 2),
                            true)) {
                        throw DFTUtilsException(ErrorCode::IO,
                                                "Failed to write footer");
                    }
                    if (co_await cs_p->writer->close() != 0) {
                        throw DFTUtilsException(ErrorCode::IO,
                                                "Failed to close writer");
                    }
                    if (cs_p->inline_index_enabled) {
                        auto bases = cs_p->writer->shard_base_offsets();
                        if (!bases.empty()) {
                            for (auto& b :
                                 cs_p->inline_index.finalized_batches) {
                                if (b.worker_idx < bases.size()) {
                                    b.c_offset += bases[b.worker_idx];
                                }
                            }
                        }
                    }
                    if (cs_p->layout_info.layout ==
                        fileio::parallel::FileLayout::SHARDED) {
                        auto shards = cs_p->writer->output_paths();
                        if (co_await fileio::parallel::merge_shards(
                                cs_p->output_path, shards) != 0) {
                            throw DFTUtilsException(ErrorCode::INTERNAL,
                                                    "merge_shards failed");
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lk(*sync_mtx);
                        chunks_info_ptr->push_back(fileio::ChunkInfo{
                            .path = cs_p->output_path,
                            .bytes_written = cs_p->bytes_uncompressed,
                            .events_written = cs_p->events_written,
                            .chunk_index = captured_chunk_idx,
                        });
                        result_raw->output_files.push_back(cs_p->output_path);
                        auto span = cs_p->writer->member_layout();
                        if (!span.empty()) {
                            ChunkMemberLayout layout;
                            layout.path = cs_p->output_path;
                            layout.members.assign(span.begin(), span.end());
                            result_raw->chunk_layouts.push_back(
                                std::move(layout));
                        }
                    }

                    if (cs_p->inline_index_enabled && cs_p->inline_index.sink &&
                        cs_p->inline_index.file_id >= 0) {
                        auto& idx = cs_p->inline_index;

                        std::vector<IndexBatch*> ordered;
                        ordered.reserve(idx.finalized_batches.size());
                        for (auto& b : idx.finalized_batches)
                            ordered.push_back(&b);
                        std::sort(ordered.begin(), ordered.end(),
                                  [](const IndexBatch* a, const IndexBatch* b) {
                                      return a->c_offset < b->c_offset;
                                  });
                        std::uint64_t running = 0;
                        for (auto* b : ordered) {
                            b->first_line_num = running;
                            running += b->line_count;
                        }
                        for (const auto& b : idx.finalized_batches) {
                            indexer::internal::IndexerCheckpoint cp;
                            cp.checkpoint_idx = b.dispatch_idx;
                            cp.uc_offset = b.uc_offset;
                            cp.uc_size = b.uc_size;
                            cp.c_offset = b.c_offset;
                            cp.c_size = b.c_size;
                            cp.bits = 0;
                            cp.num_lines = b.line_count;
                            cp.first_line_num = b.first_line_num;
                            cp.last_line_num =
                                b.line_count > 0
                                    ? b.first_line_num + b.line_count - 1
                                    : b.first_line_num;
                            idx.sink->insert_checkpoint(idx.file_id, cp);
                        }
                        idx.sink->insert_file_metadata(
                            idx.file_id, /*checkpoint_size=*/0,
                            cs_p->inline_lines_dispatched,
                            cs_p->inline_uc_dispatched);

                        if (idx.bloom) {
                            idx.bloom->finalize_file_to_sink(*idx.sink,
                                                             idx.file_id);
                        }
                        if (idx.manifest) {
                            idx.manifest->finalize_file_to_sink(*idx.sink,
                                                                idx.file_id);
                        }
                        if (idx.hash_table) {
                            idx.hash_table->finalize(*idx.sink, idx.file_id);
                        }
                        if (idx.aggregation) {
                            co_await idx.aggregation->on_file_complete();

                            aggregators::AggGlobalConfig agg_global;
                            agg_global.time_interval_us =
                                static_cast<std::uint64_t>(
                                    cfg_ptr->agg_time_interval_us);
                            agg_global.config_hash = 0;
                            idx.sink->insert_aggregation_put(
                                std::string_view(
                                    aggregators::AGG_GLOBAL_CONFIG_KEY, 2),
                                aggregators::serialize_agg_global_config(
                                    agg_global));
                            idx.sink->insert_aggregation_put(
                                aggregators::make_agg_file_key(idx.file_id),
                                "");

                            for (auto& a :
                                 idx.aggregation->aggregation_artifacts()) {
                                if (!a.empty()) {
                                    cfg_ptr->artifacts_queue->enqueue(
                                        std::move(a));
                                }
                            }
                        }
                        auto a = idx.sink->commit();
                        if (!a.empty()) {
                            cfg_ptr->artifacts_queue->enqueue(std::move(a));
                        }
                        indexed_flag->store(true, std::memory_order_release);
                    }

                    co_await sem_release->send(0);
                });

                bool rotated = false;
                ChunkState* cs_ptr = cs.get();
                std::size_t batch_counter = 0;

                while (auto batch_opt =
                           co_await config.input_channel->receive()) {
                    LineBatch* batch_ptr = batch_opt->get();
                    const std::size_t line_count = batch_ptr->lines.size();
                    if (line_count == 0) continue;

                    const std::size_t worker =
                        batch_counter++ %
                        std::max<std::size_t>(cs_ptr->num_workers, 1);
                    const int src_file =
                        static_cast<int>(batch_ptr->lines[0].source_file_idx);
                    const int ckpt =
                        static_cast<int>(batch_ptr->lines[0].checkpoint_idx);

                    std::size_t line_idx = 0;
                    while (line_idx < line_count) {
                        auto& ww = cs_ptr->workers[worker];
                        PendingSegment seg{
                            src_file,
                            ckpt,
                            worker,
                            ww.flush_line_counts.size(),
                            ww.lines_in_flush,
                            0,
                        };
                        bool inner_rotated = false;

                        while (line_idx < line_count) {
                            auto view = batch_ptr->line_view(line_idx);
                            ByteView line(view.data(), view.size());
                            append_line(*cs_ptr, worker, line);
                            seg.count++;
                            line_idx++;
                            if (ww.payload.size() >= cs_ptr->flush_threshold) {
                                if (!co_await dispatch_flush(*cs_ptr, worker)) {
                                    throw DFTUtilsException(
                                        ErrorCode::INTERNAL,
                                        "Failed to dispatch flush");
                                }
                                break;
                            }
                            if (config.chunk_size_bytes > 0 &&
                                cs_ptr->bytes_uncompressed >=
                                    config.chunk_size_bytes) {
                                inner_rotated = true;
                                break;
                            }
                        }

                        if (seg.count > 0) cs_ptr->segments.push_back(seg);
                        if (inner_rotated) {
                            rotated = true;
                            break;
                        }
                    }

                    result->events_written += line_count;
                    if (rotated) break;
                }

                if (!co_await dispatch_flush_all(*cs_ptr)) {
                    throw DFTUtilsException(
                        ErrorCode::INTERNAL,
                        "Failed to dispatch trailing flush");
                }
                for (auto& ch : cs_ptr->flush_channels) ch->close();

                if (!rotated) {
                    input_eof = true;
                    break;
                }

                current_chunk_idx++;
                cs = std::make_shared<ChunkState>();
                const auto next_path = make_chunk_path(
                    group_output_dir, current_chunk_idx, config.compress);
                if (!co_await open_chunk(
                        *cs, next_path, config.compress,
                        config.compression_level, config.chunk_size_bytes,
                        baseline_workers, &group_scope, config)) {
                    throw DFTUtilsException(ErrorCode::IO,
                                            "Failed to open next chunk");
                }
                open_inline_sink(*cs, next_path);
            }
            co_return;
        });

        if (inline_indexed_flag->load(std::memory_order_acquire)) {
            any_chunk_inline_indexed = true;
        }

        result->bytes_written = 0;
        for (const auto& c : chunks_info) {
            result->bytes_written += c.bytes_written;
        }
        result->chunks_created = chunks_info.size();

        if (config.source_files) {
            ExtractionPlan plan;
            plan.source_files = *config.source_files;
            co_await provenance->flush_to_db(plan, config.group_name,
                                             config.group_query, chunks_info,
                                             config.output_dir);
        }

        if (any_chunk_inline_indexed) result->indexed_inline = true;

    } catch (const DFTUtilsException& e) {
        DFTRACER_UTILS_LOG_ERROR("GroupWriter failed for %s: %s",
                                 config.group_name.c_str(), e.what());
        co_return make_error(e.code(), e.what());
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("GroupWriter failed for %s: %s",
                                 config.group_name.c_str(), e.what());
        co_return make_error(ErrorCode::INTERNAL, e.what());
    }

    co_return std::move(*result);
}

}  // namespace dftracer::utils::utilities::composites::dft::reorganize
