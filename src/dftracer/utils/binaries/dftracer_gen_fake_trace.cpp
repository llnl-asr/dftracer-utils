// SPDX-License-Identifier: MIT
#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/coro/combinators.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/metadata_collector_utility.h>
#include <dftracer/utils/utilities/fileio/streaming_file_writer_utility.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>
#include <dftracer/utils/utilities/trace_gen/fake_trace_writer.h>

#include <cstdint>
#include <cstdio>
#include <random>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::trace;
using namespace dftracer::utils::index::build;
using namespace dftracer::utils::index::extensions;
using namespace dftracer::utils::index::plan;
using dftracer::utils::index::build::BatchBuilder;
using dftracer::utils::index::build::IndexBuildBatchConfig;
using dftracer::utils::index::store::IndexExtension;
using dftracer::utils::index::store::internal::get_logical_path;

using trace_gen::emit_event;
using trace_gen::emit_metadata;
using trace_gen::EventArgs;
using trace_gen::jitter;
using trace_gen::make_hash;
using trace_gen::TraceWriter;

// ---------------------------------------------------------------------------
// Verify: build bloom indices and run queries to prove chunk-skipping works
// ---------------------------------------------------------------------------
struct QuerySpec {
    dstd::String label;
    dstd::Map<dstd::String, dstd::Vector<dstd::String>> predicates;
};

static coro::CoroTask<int> run_verify(
    const dstd::Vector<dstd::String>& file_paths,
    const dstd::Vector<QuerySpec>& queries, std::size_t ckpt_size) {
    // Extra dimensions: arbitrary dot-paths into args
    dstd::Vector<dstd::String> extra_dims = {"ret", "count", "offset", "epoch",
                                             "step"};

    dstd::Vector<dstd::String> all_dimensions = {"name",  "cat",   "pid", "tid",
                                                 "hhash", "fhash", "sref"};
    for (const auto& dim : extra_dims) {
        all_dimensions.push_back(dim);
    }

    ChunkIndexerConfig indexer_config;
    indexer_config.expected_entries_per_chunk = 1024;
    indexer_config.false_positive_rate = 0.01;
    indexer_config.extra_dimensions = extra_dims;

    std::printf("\n==========================================\n");
    std::printf("Verify: building bloom indices\n");
    std::printf("==========================================\n");

    // Batch-build gzip indexes for all files
    {
        dstd::Vector<dstd::String> abs_paths;
        abs_paths.reserve(file_paths.size());
        for (const auto& fp : file_paths) {
            abs_paths.emplace_back(binaries::absolute_path(fp).str());
        }

        auto index_path = internal::determine_index_path(abs_paths.front(), "");
        dftracer::utils::index::release_root(index_path);

        auto batch_config = dstd::make_shared<IndexBuildBatchConfig>();
        batch_config->file_paths = std::move(abs_paths);
        batch_config->checkpoint_size = ckpt_size;
        batch_config->parallelism = file_paths.size();

        co_await BatchBuilder::process(std::move(batch_config));
    }

    for (const auto& file_path : file_paths) {
        dstd::String abs_path(binaries::absolute_path(file_path).str());
        dstd::String index_path = internal::determine_index_path(abs_path, "");

        // Collect metadata
        auto meta_input = MetadataCollectorUtilityInput::from_file(abs_path)
                              .with_checkpoint_size(ckpt_size)
                              .with_force_rebuild(false)
                              .with_index(index_path);
        MetadataCollectorUtility collector;
        auto metadata = co_await collector(meta_input);

        if (!metadata.success) {
            std::fprintf(stderr, "  WARN: metadata failed for %s\n",
                         abs_path.c_str());
            continue;
        }

        // 3. Index chunks and write to .idx
        try {
            dstd::String idx_path_bidx =
                internal::determine_index_path(abs_path, "");
            auto opened = index::Index::open(idx_path_bidx);
            if (!opened) {
                std::fprintf(stderr, "  WARN: index open failed for %s\n",
                             abs_path.c_str());
                continue;
            }
            index::SourceStat source;
            source.mtime = static_cast<std::uint64_t>(
                index::store::internal::get_file_modification_time(abs_path));
            source.size = index::store::internal::file_size_bytes(abs_path);
            source.hash = index::store::internal::calculate_file_hash(abs_path);
            auto build = opened->build(get_logical_path(abs_path), source);
            if (!build) {
                std::fprintf(stderr, "  WARN: index build failed for %s\n",
                             abs_path.c_str());
                continue;
            }

            std::size_t file_size = metadata.uncompressed_size;
            std::size_t ckpt_count = metadata.checkpoint_count;

            struct ChunkWork {
                std::uint64_t idx;
                std::size_t start;
                std::size_t end;
            };
            dstd::Vector<ChunkWork> chunks;

            if (ckpt_count == 0) {
                chunks.push_back({0, 0, file_size});
            } else {
                std::size_t bytes_per = file_size / ckpt_count;
                for (std::size_t i = 0; i < ckpt_count; ++i) {
                    std::size_t start = i * bytes_per;
                    std::size_t end =
                        (i + 1 == ckpt_count) ? file_size : (i + 1) * bytes_per;
                    chunks.push_back(
                        {static_cast<std::uint64_t>(i), start, end});
                }
            }

            dstd::HashMap<dstd::String, ScalableBloomFilter> file_blooms;
            std::size_t total_events = 0;

            for (const auto& chunk : chunks) {
                ChunkIndexerInput ci;
                ci.with_file_path(abs_path)
                    .with_index_path(index_path)
                    .with_checkpoint_size(ckpt_size)
                    .with_checkpoint_idx(chunk.idx)
                    .with_byte_range(chunk.start, chunk.end)
                    .with_config(indexer_config)
                    .with_batch_size(4 * 1024 * 1024);

                ChunkIndexer idx_util;
                auto output = co_await idx_util(ci);
                total_events += output.events_processed;

                for (auto& [dim, bloom] : output.bloom_filters) {
                    auto blob = bloom.serialize();
                    index::put_path_granule(
                        *build, IndexExtension::Bloom, dim,
                        output.checkpoint_idx,
                        index::extensions::kinds::encode_bloom(
                            blob, bloom.entry_count()));

                    auto it = file_blooms.find(dim);
                    if (it == file_blooms.end()) {
                        file_blooms.emplace(dim, std::move(bloom));
                    } else {
                        it->second.merge_from(bloom);
                    }
                }

                index::put_chunk_statistics(*build, output.checkpoint_idx,
                                            output.statistics);
            }

            for (auto& [dim, bloom] : file_blooms) {
                auto blob = bloom.serialize();
                index::put_path_file(*build, IndexExtension::Bloom, dim,
                                     index::extensions::kinds::encode_bloom(
                                         blob, bloom.entry_count()));
            }
            for (const auto& dim : all_dimensions)
                index::put_path(*build, IndexExtension::Bloom, dim);
            build->finish(IndexExtension::Bloom,
                          indexer_config.params_hash(IndexExtension::Bloom));
            build->finish(IndexExtension::Stats,
                          indexer_config.params_hash(IndexExtension::Stats));
            auto committed = co_await build->commit();
            if (!committed) {
                std::fprintf(stderr, "  WARN: index commit failed for %s\n",
                             abs_path.c_str());
                continue;
            }

            const dstd::StringView basename =
                dstd::PathView(abs_path).filename();
            std::printf("  %.*s: indexed (%zu events, %zu chunks)\n",
                        static_cast<int>(basename.size()), basename.data(),
                        total_events, chunks.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                         "  WARN: bloom index generation failed for %s: %s\n",
                         abs_path.c_str(), e.what());
        }
    }

    // Run queries
    std::printf("\n==========================================\n");
    std::printf("Verify: bloom filter query results\n");
    std::printf("==========================================\n");
    std::printf("  %-40s  %s  %s\n", "Query", "Files matched",
                "Chunks skipped");
    std::printf("  %-40s  %s  %s\n", "----------------------------------------",
                "-------------", "--------------");

    for (const auto& q : queries) {
        std::size_t files_matched = 0;
        std::size_t total_chunks = 0;
        std::size_t chunks_matched = 0;

        for (const auto& file_path : file_paths) {
            dstd::String abs_path(binaries::absolute_path(file_path).str());
            dstd::String idx_path_q =
                internal::determine_index_path(abs_path, "");

            try {
                // Convert predicate map to query DSL string
                dstd::String query_dsl;
                for (const auto& [dim, vals] : q.predicates) {
                    if (!query_dsl.empty()) query_dsl += " and ";
                    if (vals.size() == 1) {
                        query_dsl += dim + " == \"" + vals[0] + "\"";
                    } else {
                        query_dsl += dim + " in [";
                        for (std::size_t vi = 0; vi < vals.size(); ++vi) {
                            if (vi > 0) query_dsl += ", ";
                            query_dsl += "\"" + vals[vi] + "\"";
                        }
                        query_dsl += "]";
                    }
                }

                auto parsed = duql::Query::from_string(query_dsl);
                if (!parsed) continue;

                ChunkPrunerInput pruner_input{idx_path_q, abs_path,
                                              std::move(*parsed), nullptr};
                ChunkPruner pruner;
                auto pruning = pruner(pruner_input);
                auto pruned = co_await std::move(pruning);
                if (!pruned) {
                    std::fprintf(stderr,
                                 "  WARN: bloom query failed for %s: %s\n",
                                 abs_path.c_str(), pruned.error().c_str());
                    continue;
                }
                auto& result = *pruned;

                total_chunks += result.total_checkpoints;
                if (result.file_may_match) {
                    files_matched++;
                    chunks_matched += result.candidate_checkpoints.size();
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "  WARN: bloom query failed for %s: %s\n",
                             abs_path.c_str(), e.what());
            }
        }

        std::size_t chunks_skipped =
            total_chunks > 0 ? total_chunks - chunks_matched : 0;
        std::printf("  %-40s  %zu/%zu          %zu/%zu\n", q.label.c_str(),
                    files_matched, file_paths.size(), chunks_skipped,
                    total_chunks);
    }

    std::printf("==========================================\n");
    co_return 0;
}

class GenFakeTraceArgParse : public binaries::ArgParse {
   public:
    binaries::RuntimeArgs runtime_args;

    dstd::StringView output;
    int rank_count = 8;
    int host_count = 4;
    int epoch_count = 500;
    int steps_per_epoch = 1000;
    int checkpoint_every = 5;
    int validation_every = 2;
    int train_file_count = 8;
    int val_file_count = 2;
    int step_dur_ms = 100;
    double step_duration_ms = 100;
    std::uint64_t base_seed = 42;
    bool verify = false;
    std::size_t checkpoint_size = 2 * 1024 * 1024;

    explicit GenFakeTraceArgParse(binaries::ArgParser& p) : ArgParse(p) {
        schema(runtime_args);
    }

   protected:
    void register_args() override {
        parser()
            .option(output, {"-o", "--output"},
                    "Output folder for the trace files")
            .required();

        parser().option(rank_count, {"--num-processes"}, "Number of ranks");

        parser().option(host_count, {"--num-hosts"}, "Number of hosts");

        parser().option(epoch_count, {"--num-epochs"}, "Training epochs");

        parser().option(steps_per_epoch, {"--steps-per-epoch"},
                        "Steps per epoch");

        parser().option(checkpoint_every, {"--checkpoint-every"},
                        "Checkpoint every N epochs");

        parser().option(validation_every, {"--validation-every"},
                        "Validate every N epochs");

        parser().option(train_file_count, {"--num-train-files"},
                        "Training data shards");

        parser().option(val_file_count, {"--num-val-files"},
                        "Validation data shards");

        parser().duration(
            step_duration_ms, 1e3, {"--step-duration-ms"},
            "Base step duration (default: 100ms). A bare number is "
            "milliseconds; suffixed values (e.g. 2s) are converted",
            "100");

        parser().option(base_seed, {"--seed"},
                        "Random seed for duration jitter");

        parser().flag(
            verify, {"--verify"},
            "After generation, build bloom indices and run queries to "
            "verify chunk-skipping works");

        parser().bytes(
            checkpoint_size, {"--checkpoint-size"},
            "Gzip checkpoint size for indexing (default: 2MB). Accepts "
            "units, e.g. 512KB, 2MB. Smaller values produce more chunks "
            "and better demonstrate chunk-level bloom filter skipping.",
            dstd::to_string(2 * 1024 * 1024));
    }

    void post_parse() override {
        step_dur_ms = static_cast<int>(std::lround(step_duration_ms));
    }
};

namespace {

struct GenOptions {
    dstd::StringView output_dir;
    int rank_count = 0;
    int host_count = 0;
    int epoch_count = 0;
    int steps_per_epoch = 0;
    int checkpoint_every = 0;
    int validation_every = 0;
    int train_file_count = 0;
    int val_file_count = 0;
    int step_dur_ms = 0;
    std::uint64_t step_dur_us = 0;
    std::uint64_t base_seed = 0;
    bool verify = false;
    std::size_t checkpoint_size = 0;

    explicit GenOptions(const GenFakeTraceArgParse& cli)
        : output_dir(cli.output),
          rank_count(cli.rank_count),
          host_count(cli.host_count),
          epoch_count(cli.epoch_count),
          steps_per_epoch(cli.steps_per_epoch),
          checkpoint_every(cli.checkpoint_every),
          validation_every(cli.validation_every),
          train_file_count(cli.train_file_count),
          val_file_count(cli.val_file_count),
          step_dur_ms(cli.step_dur_ms),
          step_dur_us(static_cast<std::uint64_t>(cli.step_dur_ms) * 1000ULL),
          base_seed(cli.base_seed),
          verify(cli.verify),
          checkpoint_size(cli.checkpoint_size) {}
};

// Names of the hosts and files the events refer to, with their hashes.
struct TraceNames {
    dstd::Vector<dstd::String> host_names;
    dstd::Vector<dstd::String> host_hashes;
    dstd::Vector<dstd::String> train_file_names;
    dstd::Vector<dstd::String> train_file_hashes;
    dstd::Vector<dstd::String> val_file_names;
    dstd::Vector<dstd::String> val_file_hashes;
    dstd::String ckpt_file_name;
    dstd::String ckpt_file_hash;
    dstd::String script_name;
    dstd::String script_hash;
};

TraceNames make_trace_names(const GenOptions& opts) {
    TraceNames names;
    names.host_names.resize(opts.host_count);
    names.host_hashes.resize(opts.host_count);
    for (int h = 0; h < opts.host_count; ++h) {
        names.host_names[h] = "node-" + dstd::to_string(h);
        names.host_hashes[h] = make_hash(names.host_names[h]);
    }

    names.train_file_names.resize(opts.train_file_count);
    names.train_file_hashes.resize(opts.train_file_count);
    for (int f = 0; f < opts.train_file_count; ++f) {
        names.train_file_names[f] =
            "/data/train/shard_" + dstd::to_string(f) + ".h5";
        names.train_file_hashes[f] = make_hash(names.train_file_names[f]);
    }

    names.val_file_names.resize(opts.val_file_count);
    names.val_file_hashes.resize(opts.val_file_count);
    for (int f = 0; f < opts.val_file_count; ++f) {
        names.val_file_names[f] = "/data/val/val_" + dstd::to_string(f) + ".h5";
        names.val_file_hashes[f] = make_hash(names.val_file_names[f]);
    }

    names.ckpt_file_name = "/checkpoints/model_ckpt.pt";
    names.ckpt_file_hash = make_hash(names.ckpt_file_name);

    names.script_name = "python train_unet3d.py";
    names.script_hash = make_hash(names.script_name);
    return names;
}

void print_banner(const GenOptions& opts) {
    std::printf("==========================================\n");
    std::printf("DFTracer Fake Trace Generator (UNet3D)\n");
    std::printf("==========================================\n");
    std::printf("  Ranks: %d   Hosts: %d\n", opts.rank_count, opts.host_count);
    std::printf("  Epochs: %d   Steps/epoch: %d\n", opts.epoch_count,
                opts.steps_per_epoch);
    std::printf("  Checkpoint every: %d   Validation every: %d\n",
                opts.checkpoint_every, opts.validation_every);
    std::printf("  Train shards: %d   Val files: %d\n", opts.train_file_count,
                opts.val_file_count);
    std::printf("  Step duration: %d ms   Format: .pfw.gz\n", opts.step_dur_ms);
    std::printf("  Seed: %llu   Verify: %s\n",
                static_cast<unsigned long long>(opts.base_seed),
                opts.verify ? "yes" : "no");
    std::printf("  Output: %s\n", binaries::c_str(opts.output_dir));
    std::printf("==========================================\n\n");
}

dstd::Vector<dstd::String> plan_files(const GenOptions& opts) {
    dstd::Vector<dstd::String> generated_files(opts.rank_count);
    for (int rank = 0; rank < opts.rank_count; ++rank) {
        generated_files[rank] = dstd::String(
            common::str_cat(opts.output_dir, "/rank_", rank, ".pfw.gz"));
    }
    return generated_files;
}

// The events of one rank's file. `opts` and `names` must outlive it.
struct RankTrace {
    const GenOptions& opts;
    const TraceNames& names;
    int rank;
    TraceWriter writer;
    const dstd::String& sref;
    const dstd::String& ckref;
    std::mt19937_64 rng;
    int host_idx;
    const dstd::String& my_hhash;
    std::uint64_t pid;
    std::uint64_t tid_main;
    std::uint64_t tid_io;
    dstd::Vector<int> my_train_shards;
    std::size_t rank_events = 0;
    std::uint64_t next_id = 0;
    std::uint64_t ts = 1000000000ULL;  // 1 second in us

    RankTrace(const GenOptions& options, const TraceNames& trace_names,
              int rank_index, const dstd::String& path)
        : opts(options),
          names(trace_names),
          rank(rank_index),
          writer(path),
          sref(trace_names.script_hash),
          ckref(trace_names.ckpt_file_hash),
          rng(options.base_seed +
              static_cast<std::uint64_t>(rank_index) * 10000ULL),
          host_idx(rank_index % options.host_count),
          my_hhash(trace_names.host_hashes[host_idx]),
          pid(1000 + static_cast<std::uint64_t>(rank_index)),
          tid_main(pid * 10),
          tid_io(pid * 10 + 1) {
        writer.write("[\n");
    }

    // Fills the common fields and assigns the next id.
    EventArgs make_event(const dstd::String& name, const dstd::String& cat,
                         std::uint64_t event_tid, std::uint64_t event_ts,
                         std::uint64_t dur, int level) {
        EventArgs a;
        a.id = next_id++;
        a.pid = pid;
        a.tid = event_tid;
        a.name = name;
        a.cat = cat;
        a.ts = event_ts;
        a.dur = dur;
        a.level = level;
        a.hhash = my_hhash;
        a.cmd_hash = sref;
        return a;
    }

    void emit_header() {
        // Train shards this rank reads (round-robin)
        for (int f = rank; f < opts.train_file_count; f += opts.rank_count) {
            my_train_shards.push_back(f);
        }
        if (my_train_shards.empty()) {
            my_train_shards.push_back(rank % opts.train_file_count);
        }

        emit_metadata(writer, "HH", my_hhash, names.host_names[host_idx],
                      my_hhash);

        for (int si : my_train_shards) {
            emit_metadata(writer, "FH", my_hhash, names.train_file_names[si],
                          names.train_file_hashes[si]);
        }
        for (int vi = 0; vi < opts.val_file_count; ++vi) {
            emit_metadata(writer, "FH", my_hhash, names.val_file_names[vi],
                          names.val_file_hashes[vi]);
        }
        emit_metadata(writer, "FH", my_hhash, names.ckpt_file_name,
                      names.ckpt_file_hash);
        emit_metadata(writer, "SH", my_hhash, names.script_name,
                      names.script_hash);
    }

    void emit_train_step(int epoch, int step) {
        char extra[256];
        std::uint64_t step_start = ts;

        // -- Data loading I/O (on io thread, level=2)
        // --
        std::uint64_t io_start = ts;
        int shard_idx =
            my_train_shards[step % static_cast<int>(my_train_shards.size())];
        const dstd::String& data_fhash = names.train_file_hashes[shard_idx];
        std::uint64_t io_size = jitter(rng, 4096);

        // open
        {
            auto a = make_event("open", "POSIX", tid_io, ts, jitter(rng, 5), 2);
            a.fhash = data_fhash;
            std::snprintf(extra, sizeof(extra), R"("ret":3)");
            a.extra = extra;
            emit_event(writer, a);
            ts += a.dur;
            ++rank_events;
        }

        // pread / read / fread (randomly 3-5 calls)
        int read_count = 3 + static_cast<int>(rng() % 3);
        const char* read_ops[] = {"pread", "read", "fread"};
        for (int r = 0; r < read_count; ++r) {
            auto a = make_event(read_ops[r % 3], "POSIX", tid_io, ts,
                                jitter(rng, 20), 2);
            a.fhash = data_fhash;
            std::uint64_t offset = static_cast<std::uint64_t>(r) * io_size;
            std::snprintf(extra, sizeof(extra),
                          R"("ret":%llu,"count":%llu,"offset":%llu)",
                          static_cast<unsigned long long>(io_size),
                          static_cast<unsigned long long>(io_size),
                          static_cast<unsigned long long>(offset));
            a.extra = extra;
            emit_event(writer, a);
            ts += a.dur;
            ++rank_events;
        }

        // close
        {
            auto a =
                make_event("close", "POSIX", tid_io, ts, jitter(rng, 3), 2);
            a.fhash = data_fhash;
            std::snprintf(extra, sizeof(extra), R"("ret":0)");
            a.extra = extra;
            emit_event(writer, a);
            ts += a.dur;
            ++rank_events;
        }

        // Emit data_loading wrapper (level=1, spans all
        // I/O)
        {
            auto a = make_event("data_loading", "IO", tid_io, io_start,
                                ts - io_start, 1);
            std::snprintf(extra, sizeof(extra), R"("epoch":%d,"step":%d)",
                          epoch, step);
            a.extra = extra;
            emit_event(writer, a);
            ++rank_events;
        }

        // -- Forward pass (5 events on main thread,
        // level=2) --
        std::uint64_t fwd_start = ts;
        const char* fwd_ops[] = {"conv3d", "batch_norm", "relu", "max_pool",
                                 "upsample"};
        for (int f = 0; f < 5; ++f) {
            auto a = make_event(fwd_ops[f], "APP", tid_main, ts,
                                jitter(rng, opts.step_dur_us / 5), 2);
            std::snprintf(extra, sizeof(extra), R"("epoch":%d,"step":%d)",
                          epoch, step);
            a.extra = extra;
            emit_event(writer, a);
            ts += a.dur;
            ++rank_events;
        }

        // Emit forward wrapper (level=1)
        {
            auto a = make_event("forward", "APP", tid_main, fwd_start,
                                ts - fwd_start, 1);
            std::snprintf(extra, sizeof(extra), R"("epoch":%d,"step":%d)",
                          epoch, step);
            a.extra = extra;
            emit_event(writer, a);
            ++rank_events;
        }

        // -- Loss + backward (3 events, level=2) --
        std::uint64_t back_start = ts;
        const char* back_ops[] = {"dice_loss", "backward", "allreduce"};
        for (int b = 0; b < 3; ++b) {
            auto a = make_event(back_ops[b], "APP", tid_main, ts,
                                jitter(rng, opts.step_dur_us / 4), 2);
            std::snprintf(extra, sizeof(extra), R"("epoch":%d,"step":%d)",
                          epoch, step);
            a.extra = extra;
            emit_event(writer, a);
            ts += a.dur;
            ++rank_events;
        }

        // Emit backward wrapper (level=1)
        {
            auto a = make_event("backward_pass", "APP", tid_main, back_start,
                                ts - back_start, 1);
            std::snprintf(extra, sizeof(extra), R"("epoch":%d,"step":%d)",
                          epoch, step);
            a.extra = extra;
            emit_event(writer, a);
            ++rank_events;
        }

        // -- Optimizer step (1 event, level=1) --
        {
            auto a = make_event("optimizer_step", "APP", tid_main, ts,
                                jitter(rng, opts.step_dur_us / 8), 1);
            std::snprintf(extra, sizeof(extra), R"("epoch":%d,"step":%d)",
                          epoch, step);
            a.extra = extra;
            emit_event(writer, a);
            ts += a.dur;
            ++rank_events;
        }

        // Emit train_step wrapper (level=0, spans
        // entire step)
        {
            auto a = make_event("train_step", "APP", tid_main, step_start,
                                ts - step_start, 0);
            std::snprintf(extra, sizeof(extra), R"("epoch":%d,"step":%d)",
                          epoch, step);
            a.extra = extra;
            emit_event(writer, a);
            ++rank_events;
        }
    }

    void emit_validation(int epoch) {
        std::uint64_t val_phase_start = ts;
        const int val_steps = 10;
        for (int vs = 0; vs < val_steps; ++vs) {
            char extra[256];
            int vf_idx = vs % opts.val_file_count;
            const dstd::String& vf_hash = names.val_file_hashes[vf_idx];
            std::uint64_t vio_size = jitter(rng, 4096);
            std::uint64_t val_step_start = ts;

            // open (level=2)
            {
                auto a =
                    make_event("open", "POSIX", tid_io, ts, jitter(rng, 5), 2);
                a.fhash = vf_hash;
                std::snprintf(extra, sizeof(extra), R"("ret":4)");
                a.extra = extra;
                emit_event(writer, a);
                ts += a.dur;
                ++rank_events;
            }

            // read calls (2-3, level=2)
            int read_count = 2 + static_cast<int>(rng() % 2);
            const char* read_ops[] = {"pread", "read", "fread"};
            for (int r = 0; r < read_count; ++r) {
                auto a = make_event(read_ops[r % 3], "POSIX", tid_io, ts,
                                    jitter(rng, 20), 2);
                a.fhash = vf_hash;
                std::snprintf(extra, sizeof(extra),
                              R"("ret":%llu,"count":%llu,"offset":%llu)",
                              static_cast<unsigned long long>(vio_size),
                              static_cast<unsigned long long>(vio_size),
                              static_cast<unsigned long long>(
                                  static_cast<std::uint64_t>(r) * vio_size));
                a.extra = extra;
                emit_event(writer, a);
                ts += a.dur;
                ++rank_events;
            }

            // close (level=2)
            {
                auto a =
                    make_event("close", "POSIX", tid_io, ts, jitter(rng, 3), 2);
                a.fhash = vf_hash;
                std::snprintf(extra, sizeof(extra), R"("ret":0)");
                a.extra = extra;
                emit_event(writer, a);
                ts += a.dur;
                ++rank_events;
            }

            // val_forward (level=2)
            {
                auto a = make_event("val_forward", "APP", tid_main, ts,
                                    jitter(rng, opts.step_dur_us / 3), 2);
                std::snprintf(extra, sizeof(extra), R"("epoch":%d,"step":%d)",
                              epoch, vs);
                a.extra = extra;
                emit_event(writer, a);
                ts += a.dur;
                ++rank_events;
            }

            // val_loss (level=2)
            {
                auto a = make_event("val_loss", "APP", tid_main, ts,
                                    jitter(rng, opts.step_dur_us / 6), 2);
                std::snprintf(extra, sizeof(extra), R"("epoch":%d,"step":%d)",
                              epoch, vs);
                a.extra = extra;
                emit_event(writer, a);
                ts += a.dur;
                ++rank_events;
            }

            // val_step wrapper (level=1)
            {
                auto a = make_event("val_step", "APP", tid_main, val_step_start,
                                    ts - val_step_start, 1);
                std::snprintf(extra, sizeof(extra), R"("epoch":%d,"step":%d)",
                              epoch, vs);
                a.extra = extra;
                emit_event(writer, a);
                ++rank_events;
            }
        }

        // validation wrapper (level=0)
        {
            char extra[256];
            auto a = make_event("validation", "APP", tid_main, val_phase_start,
                                ts - val_phase_start, 0);
            std::snprintf(extra, sizeof(extra), R"("epoch":%d)", epoch);
            a.extra = extra;
            emit_event(writer, a);
            ++rank_events;
        }
    }

    void emit_checkpoint(int epoch) {
        char extra[256];
        std::uint64_t ckpt_start = ts;

        // open checkpoint file (level=1)
        {
            auto a =
                make_event("open", "POSIX", tid_io, ts, jitter(rng, 10), 1);
            a.fhash = ckref;
            std::snprintf(extra, sizeof(extra), R"("ret":5)");
            a.extra = extra;
            emit_event(writer, a);
            ts += a.dur;
            ++rank_events;
        }

        // pwrite calls (10-20, level=1)
        int write_count = 10 + static_cast<int>(rng() % 11);
        for (int wr = 0; wr < write_count; ++wr) {
            auto a = make_event((wr % 2 == 0) ? "pwrite" : "write", "POSIX",
                                tid_io, ts, jitter(rng, 50), 1);
            a.fhash = ckref;
            std::uint64_t wr_size = jitter(rng, 1048576);  // ~1 MB
            std::snprintf(extra, sizeof(extra),
                          R"("ret":%llu,"count":%llu,"offset":%llu)",
                          static_cast<unsigned long long>(wr_size),
                          static_cast<unsigned long long>(wr_size),
                          static_cast<unsigned long long>(
                              static_cast<std::uint64_t>(wr) * wr_size));
            a.extra = extra;
            emit_event(writer, a);
            ts += a.dur;
            ++rank_events;
        }

        // fsync (level=1)
        {
            auto a =
                make_event("fsync", "POSIX", tid_io, ts, jitter(rng, 200), 1);
            a.fhash = ckref;
            std::snprintf(extra, sizeof(extra), R"("ret":0)");
            a.extra = extra;
            emit_event(writer, a);
            ts += a.dur;
            ++rank_events;
        }

        // close (level=1)
        {
            auto a =
                make_event("close", "POSIX", tid_io, ts, jitter(rng, 3), 1);
            a.fhash = ckref;
            std::snprintf(extra, sizeof(extra), R"("ret":0)");
            a.extra = extra;
            emit_event(writer, a);
            ts += a.dur;
            ++rank_events;
        }

        // checkpoint wrapper (level=0)
        {
            auto a = make_event("checkpoint", "IO", tid_io, ckpt_start,
                                ts - ckpt_start, 0);
            std::snprintf(extra, sizeof(extra), R"("epoch":%d)", epoch);
            a.extra = extra;
            emit_event(writer, a);
            ++rank_events;
        }
    }

    void run() {
        emit_header();
        for (int epoch = 0; epoch < opts.epoch_count; ++epoch) {
            for (int step = 0; step < opts.steps_per_epoch; ++step) {
                emit_train_step(epoch, step);
            }
            if (opts.validation_every > 0 &&
                (epoch + 1) % opts.validation_every == 0) {
                emit_validation(epoch);
            }
            if (opts.checkpoint_every > 0 &&
                (epoch + 1) % opts.checkpoint_every == 0) {
                emit_checkpoint(epoch);
            }
        }
        writer.write("]\n");
        writer.close();
    }
};

// One file per rank. The pointers must outlive the task.
coro::CoroTask<void> generate_rank(const GenOptions* opts,
                                   const TraceNames* names, int rank,
                                   const dstd::String* file,
                                   std::size_t* event_count) {
    RankTrace trace(*opts, *names, rank, *file);
    trace.run();
    *event_count = trace.rank_events;
    co_return;
}

// Prints the results after generation.
void print_summary(const GenOptions& opts, const TraceNames& names,
                   const dstd::Vector<dstd::String>& generated_files,
                   const dstd::Vector<std::size_t>& rank_event_counts) {
    std::size_t total_events = 0;
    for (int rank = 0; rank < opts.rank_count; ++rank) {
        std::printf("  rank %d: %zu events -> %s\n", rank,
                    rank_event_counts[rank], generated_files[rank].c_str());
        total_events += rank_event_counts[rank];
    }

    std::printf("\n==========================================\n");
    std::printf("Generation complete\n");
    std::printf("==========================================\n");
    std::printf("  Total events: %zu\n", total_events);
    std::printf("  Total files:  %d\n", opts.rank_count);
    std::printf("\nInteresting queries for bloom filter testing:\n");
    std::printf(
        "  1. name=pwrite                   (checkpoint I/O, "
        "~%d%% of "
        "epochs)\n",
        opts.checkpoint_every > 0 ? 100 / opts.checkpoint_every : 0);
    std::printf("  2. fhash=%s  (validation data, ~%d%% of epochs)\n",
                names.val_file_hashes[0].c_str(),
                opts.validation_every > 0 ? 100 / opts.validation_every : 0);
    if (!names.train_file_hashes.empty()) {
        std::printf("  3. fhash=%s  (rank-specific train shard)\n",
                    names.train_file_hashes[0].c_str());
    }
    std::printf("  4. hhash=%s  (host-specific, %s)\n",
                names.host_hashes[0].c_str(), names.host_names[0].c_str());
    std::printf(
        "  5. name=allreduce                (every step, "
        "dense)\n");
    std::printf(
        "  6. name=fsync                    (checkpoint only, "
        "sparse)\n");
    std::printf("==========================================\n");
}

dstd::Vector<QuerySpec> make_test_queries(const TraceNames& names) {
    dstd::Vector<QuerySpec> test_queries;

    test_queries.push_back(
        {"name=pwrite (sparse, ckpt only)", {{"name", {"pwrite"}}}});
    test_queries.push_back(
        {"name=allreduce (dense, every step)", {{"name", {"allreduce"}}}});
    test_queries.push_back(
        {"name=fsync (sparse, ckpt only)", {{"name", {"fsync"}}}});
    test_queries.push_back(
        {"name=val_forward (periodic)", {{"name", {"val_forward"}}}});
    test_queries.push_back(
        {"cat=POSIX (all I/O events)", {{"cat", {"POSIX"}}}});
    test_queries.push_back(
        {"cat=APP (all compute events)", {{"cat", {"APP"}}}});

    dstd::String pid0 = dstd::to_string(1000);
    test_queries.push_back(
        {"pid=" + pid0 + " (rank 0 only)", {{"pid", {pid0}}}});

    dstd::String tid_io_0 = dstd::to_string(10001);
    test_queries.push_back(
        {"tid=" + tid_io_0 + " (rank 0 io thread)", {{"tid", {tid_io_0}}}});

    test_queries.push_back({"fhash=" + names.val_file_names[0] + " (resolved)",
                            {{"fhash", {names.val_file_hashes[0]}}}});
    if (!names.train_file_hashes.empty()) {
        test_queries.push_back(
            {"fhash=" + names.train_file_names[0] + " (resolved)",
             {{"fhash", {names.train_file_hashes[0]}}}});
    }
    test_queries.push_back(
        {"fhash=ckpt (resolved)", {{"fhash", {names.ckpt_file_hash}}}});
    test_queries.push_back({"hhash=" + names.host_names[0] + " (resolved)",
                            {{"hhash", {names.host_hashes[0]}}}});
    test_queries.push_back(
        {"shash=train_unet3d (resolved)", {{"shash", {names.script_hash}}}});

    test_queries.push_back({"name=pwrite AND cat=POSIX",
                            {{"name", {"pwrite"}}, {"cat", {"POSIX"}}}});
    test_queries.push_back(
        {"name=fsync AND fhash=ckpt",
         {{"name", {"fsync"}}, {"fhash", {names.ckpt_file_hash}}}});
    test_queries.push_back(
        {"cat=POSIX AND hhash=" + names.host_names[0],
         {{"cat", {"POSIX"}}, {"hhash", {names.host_hashes[0]}}}});
    test_queries.push_back(
        {"cat=APP AND pid=" + pid0, {{"cat", {"APP"}}, {"pid", {pid0}}}});

    if (!names.train_file_hashes.empty()) {
        test_queries.push_back({"name=read AND hhash=" + names.host_names[0] +
                                    " AND fhash=shard_0",
                                {{"name", {"read"}},
                                 {"hhash", {names.host_hashes[0]}},
                                 {"fhash", {names.train_file_hashes[0]}}}});
    }

    test_queries.push_back(
        {"name=pwrite|write (ckpt writes)", {{"name", {"pwrite", "write"}}}});
    test_queries.push_back(
        {"name=open|close (all open/close)", {{"name", {"open", "close"}}}});
    test_queries.push_back(
        {"fhash=any val file (OR)",
         {{"fhash", dstd::Vector<dstd::String>(names.val_file_hashes.begin(),
                                               names.val_file_hashes.end())}}});

    test_queries.push_back(
        {"name=NONEXISTENT (expect 0)", {{"name", {"NONEXISTENT"}}}});
    test_queries.push_back(
        {"cat=NONEXISTENT (expect 0)", {{"cat", {"NONEXISTENT"}}}});
    test_queries.push_back({"name=pwrite AND cat=APP (impossible)",
                            {{"name", {"pwrite"}}, {"cat", {"APP"}}}});

    return test_queries;
}

}  // namespace

int main(int argc, char** argv) {
    return binaries::cli_main<GenFakeTraceArgParse>(
        argc, argv, "dftracer_gen_fake_trace",
        "Generate realistic DFTracer traces modeled after UNet3D training. "
        "Produces per-rank .pfw.gz files with known patterns "
        "suitable for testing bloom-filter indexing.",
        [](GenFakeTraceArgParse& cli) -> int {
            const GenOptions opts(cli);

            common::unwrap(
                dstd::fs::create_directories(dstd::PathView(opts.output_dir)));

            const TraceNames names = make_trace_names(opts);
            print_banner(opts);

            dstd::Vector<dstd::String> generated_files = plan_files(opts);
            dstd::Vector<std::size_t> rank_event_counts(opts.rank_count, 0);

            const auto executor_threads = cli.runtime_args.executor_threads;
            return binaries::run_on_pool(
                "DFTracer Fake Trace Generator", cli.runtime_args,
                [&](coro::Scope&) -> coro::CoroTask<int> {
                    dstd::Vector<int> ranks;
                    for (int rank = 0; rank < opts.rank_count; ++rank) {
                        ranks.push_back(rank);
                    }
                    co_await coro::parallel_map(
                        std::move(ranks),
                        [&](int rank) {
                            return generate_rank(&opts, &names, rank,
                                                 &generated_files[rank],
                                                 &rank_event_counts[rank]);
                        },
                        {.max_in_flight = executor_threads});
                    print_summary(opts, names, generated_files,
                                  rank_event_counts);
                    if (!opts.verify) co_return 0;
                    const auto test_queries = make_test_queries(names);
                    co_return co_await run_verify(generated_files, test_queries,
                                                  opts.checkpoint_size);
                });
        });
}
