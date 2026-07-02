// MPI driver for the distributed-SST aggregator.
//
// Pipeline DAG:
//   scan -> phase_a -> phase_b -> phase_c -> merge
// Each stage is its own task wired via depends_on(); MPI collectives
// between stages sit inside the task bodies.

#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/rocksdb/column_families.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_key.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_visitor.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregators.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/association_resolver_utility.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/association_tracker.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/event_aggregator.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/perfetto_trace_writer_utility.h>
#include <dftracer/utils/utilities/fileio/parallel/layout.h>
#include <dftracer/utils/utilities/fileio/parallel/merge.h>
#include <dftracer/utils/utilities/indexer/index_batch_sink.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_sst_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/common/gzip_member_scanner.h>
#include <fcntl.h>
#include <mpi.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "common_cli.h"
#include "common_cli_mpi.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using dftracer::utils::utilities::composites::dft::aggregators::
    AGG_KEY_NUM_SHARDS;
using dftracer::utils::utilities::composites::dft::aggregators::
    AggregationConfig;
using dftracer::utils::utilities::composites::dft::aggregators::
    AggregationVisitor;
using dftracer::utils::utilities::composites::dft::aggregators::
    AssociationTracker;
using dftracer::utils::utilities::composites::dft::aggregators::EventAggregator;
using dftracer::utils::utilities::composites::dft::aggregators::
    PerfettoEventFormat;
using dftracer::utils::utilities::composites::dft::aggregators::
    PerfettoTraceWriterInput;
using dftracer::utils::utilities::composites::dft::aggregators::
    PerfettoTraceWriterUtility;
using dftracer::utils::utilities::indexer::IndexBatchBuilderUtility;
using dftracer::utils::utilities::indexer::IndexBatchSink;
using dftracer::utils::utilities::indexer::IndexBuildBatchConfig;
using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::IndexDatabaseSstWriterContext;
using dftracer::utils::utilities::indexer::SstArtifactRegistry;
using dftracer::utils::utilities::indexer::internal::
    enumerate_gzip_member_candidates;
using dftracer::utils::utilities::indexer::internal::GzipMember;

namespace {

class AggregatorMpiArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory{
        cli::DirMode::DEFAULT_DOT,
        "Input directory containing .pfw or .pfw.gz files"};
    cli::PipelineArgs pipeline;
    cli::IndexingArgs indexing;

    std::string output;
    std::string staging_dir;
    std::string shared_staging_dir;
    double time_interval = 5000.0;
    bool keep_staging = false;

    explicit AggregatorMpiArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        indexing.index_dir_help =
            "Directory to store the final index (shared across ranks)";
        indexing.force_help = "Force index recreation";
        schema(directory, pipeline, indexing);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("-o", "--output")
            .help("Output file path for aggregated counters (gzip JSON)")
            .default_value<std::string>("aggregated_output.json.gz");

        parser()
            .add_argument("--staging-dir")
            .help(
                "Per-rank SST staging root. Defaults to <index_dir>/_staging. "
                "Each rank writes to <staging_dir>/rank_<R>.")
            .default_value<std::string>("");

        parser()
            .add_argument("--shared-staging")
            .help(
                "Shared-FS staging root. When set and different from "
                "--staging-dir, each rank moves its SSTs + tracker.bin from "
                "the (node-local) staging dir to <shared-staging>/rank_<R> "
                "before the coordinator ingest. Required for multi-node runs "
                "where --staging-dir points at node-local NVMe.")
            .default_value<std::string>("");

        parser()
            .add_argument("-t", "--time-interval")
            .help("Time interval in milliseconds for bucketing (default: 5000)")
            .scan<'g', double>()
            .default_value(5000.0);

        parser()
            .add_argument("--keep-staging")
            .help("Keep per-rank SST staging dirs after successful ingest")
            .default_value(false)
            .implicit_value(true);
    }

    void post_parse() override {
        output = parser().get<std::string>("--output");
        staging_dir = parser().get<std::string>("--staging-dir");
        shared_staging_dir = parser().get<std::string>("--shared-staging");
        time_interval = parser().get<double>("--time-interval");
        keep_staging = parser().get<bool>("--keep-staging");
    }
};

std::vector<std::string> enumerate_inputs(const std::string& dir) {
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto& p = entry.path();
        const auto ext = p.extension().string();
        if (ext == ".pfw" || ext == ".gz") files.push_back(p.string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<char> pack_paths(const std::vector<std::string>& paths) {
    std::uint64_t total = sizeof(std::uint64_t);
    for (const auto& p : paths) total += sizeof(std::uint64_t) + p.size();
    std::vector<char> buf;
    buf.reserve(total);
    auto u64 = [&](std::uint64_t v) {
        buf.insert(buf.end(), reinterpret_cast<const char*>(&v),
                   reinterpret_cast<const char*>(&v) + sizeof(v));
    };
    u64(paths.size());
    for (const auto& p : paths) {
        u64(p.size());
        buf.insert(buf.end(), p.begin(), p.end());
    }
    return buf;
}

std::vector<std::string> unpack_paths(const std::vector<char>& buf) {
    std::vector<std::string> paths;
    if (buf.size() < sizeof(std::uint64_t)) return paths;
    const char* p = buf.data();
    const char* end = buf.data() + buf.size();
    auto read_u64 = [&](std::uint64_t& out) -> bool {
        if (end - p < static_cast<std::ptrdiff_t>(sizeof(out))) return false;
        std::memcpy(&out, p, sizeof(out));
        p += sizeof(out);
        return true;
    };
    std::uint64_t n = 0;
    if (!read_u64(n)) return paths;
    paths.reserve(n);
    for (std::uint64_t i = 0; i < n; ++i) {
        std::uint64_t len = 0;
        if (!read_u64(len)) break;
        if (end - p < static_cast<std::ptrdiff_t>(len)) break;
        paths.emplace_back(p, p + len);
        p += len;
    }
    return paths;
}

void pack_artifacts(const IndexDatabaseSstWriterContext::Artifacts& a,
                    std::vector<char>& buf) {
    auto append_u64 = [&](std::uint64_t v) {
        buf.insert(buf.end(), reinterpret_cast<const char*>(&v),
                   reinterpret_cast<const char*>(&v) + sizeof(v));
    };
    auto append_opt = [&](const std::optional<std::string>& s) {
        if (s) {
            buf.push_back(1);
            append_u64(s->size());
            buf.insert(buf.end(), s->begin(), s->end());
        } else {
            buf.push_back(0);
        }
    };
    append_opt(a.metadata_sst);
    append_opt(a.checkpoints_sst);
    append_opt(a.manifest_sst);
    append_opt(a.chunk_bloom_sst);
    append_opt(a.file_bloom_sst);
    append_opt(a.chunk_stats_sst);
    append_opt(a.chunk_dim_stats_sst);
    append_opt(a.dimensions_sst);
    append_opt(a.file_scalar_stats_sst);
    append_opt(a.file_cat_counts_sst);
    append_opt(a.file_pid_tid_counts_sst);
    append_opt(a.file_name_counts_sst);
    append_opt(a.name_dictionary_sst);
    append_opt(a.name_file_postings_sst);
    append_opt(a.name_chunk_postings_sst);
    append_opt(a.hash_tables_sst);
    append_opt(a.aggregation_sst);
    append_opt(a.system_metrics_sst);
}

bool unpack_artifacts(const char*& p, const char* end,
                      IndexDatabaseSstWriterContext::Artifacts& a) {
    auto read_u64 = [&](std::uint64_t& out) -> bool {
        if (end - p < static_cast<std::ptrdiff_t>(sizeof(out))) return false;
        std::memcpy(&out, p, sizeof(out));
        p += sizeof(out);
        return true;
    };
    auto read_opt = [&](std::optional<std::string>& s) -> bool {
        if (p == end) return false;
        const char flag = *p++;
        if (!flag) return true;
        std::uint64_t len = 0;
        if (!read_u64(len)) return false;
        if (end - p < static_cast<std::ptrdiff_t>(len)) return false;
        s = std::string(p, p + len);
        p += len;
        return true;
    };
    return read_opt(a.metadata_sst) && read_opt(a.checkpoints_sst) &&
           read_opt(a.manifest_sst) && read_opt(a.chunk_bloom_sst) &&
           read_opt(a.file_bloom_sst) && read_opt(a.chunk_stats_sst) &&
           read_opt(a.chunk_dim_stats_sst) && read_opt(a.dimensions_sst) &&
           read_opt(a.file_scalar_stats_sst) &&
           read_opt(a.file_cat_counts_sst) &&
           read_opt(a.file_pid_tid_counts_sst) &&
           read_opt(a.file_name_counts_sst) &&
           read_opt(a.name_dictionary_sst) &&
           read_opt(a.name_file_postings_sst) &&
           read_opt(a.name_chunk_postings_sst) && read_opt(a.hash_tables_sst) &&
           read_opt(a.aggregation_sst) && read_opt(a.system_metrics_sst);
}

std::vector<char> pack_artifact_list(
    const std::vector<IndexDatabaseSstWriterContext::Artifacts>& main_artifacts,
    const std::vector<IndexDatabaseSstWriterContext::Artifacts>&
        agg_artifacts) {
    std::vector<char> buf;
    std::uint64_t count = 0;
    for (const auto& a : main_artifacts)
        if (!a.empty()) ++count;
    for (const auto& a : agg_artifacts)
        if (!a.empty()) ++count;
    buf.insert(buf.end(), reinterpret_cast<const char*>(&count),
               reinterpret_cast<const char*>(&count) + sizeof(count));
    for (const auto& a : main_artifacts)
        if (!a.empty()) pack_artifacts(a, buf);
    for (const auto& a : agg_artifacts)
        if (!a.empty()) pack_artifacts(a, buf);
    return buf;
}

bool append_artifact_list(const char* p, const char* end,
                          SstArtifactRegistry& registry) {
    if (end - p < static_cast<std::ptrdiff_t>(sizeof(std::uint64_t)))
        return false;
    std::uint64_t count = 0;
    std::memcpy(&count, p, sizeof(count));
    p += sizeof(count);
    for (std::uint64_t i = 0; i < count; ++i) {
        IndexDatabaseSstWriterContext::Artifacts a;
        if (!unpack_artifacts(p, end, a)) return false;
        registry.append(std::move(a));
    }
    return true;
}

coro::CoroTask<void> scan_one_file(const std::string& path,
                                   std::vector<GzipMember>& out) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) co_return;
    struct stat st;
    if (::fstat(fd, &st) == 0 && st.st_size >= 18) {
        co_await enumerate_gzip_member_candidates(
            fd, static_cast<std::uint64_t>(st.st_size), out);
    }
    ::close(fd);
}

struct WorkUnit {
    std::size_t file_idx;
    std::size_t member_begin;
    std::size_t member_end;
    std::uint64_t c_size;
};

// Partition members into slices of ~target bytes. Deterministic across
// ranks so every rank computes identical assignments from the same map.
std::vector<WorkUnit> build_work_units(
    const std::vector<std::vector<GzipMember>>& per_file_members,
    std::uint64_t target_c_size) {
    std::vector<WorkUnit> units;
    for (std::size_t fi = 0; fi < per_file_members.size(); ++fi) {
        const auto& members = per_file_members[fi];
        if (members.empty()) continue;
        std::size_t begin = 0;
        std::uint64_t accum = 0;
        for (std::size_t i = 0; i < members.size(); ++i) {
            accum += members[i].c_size;
            const bool is_last = (i + 1 == members.size());
            if ((target_c_size > 0 && accum >= target_c_size) || is_last) {
                units.push_back({fi, begin, i + 1, accum});
                begin = i + 1;
                accum = 0;
            }
        }
    }
    return units;
}

std::vector<int> lpt_assign_units(const std::vector<WorkUnit>& units,
                                  int num_ranks) {
    std::vector<std::size_t> order(units.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (units[a].c_size != units[b].c_size)
            return units[a].c_size > units[b].c_size;
        if (units[a].file_idx != units[b].file_idx)
            return units[a].file_idx < units[b].file_idx;
        return units[a].member_begin < units[b].member_begin;
    });
    std::vector<std::uint64_t> loads(num_ranks, 0);
    std::vector<int> owner(units.size(), 0);
    for (std::size_t ord : order) {
        int best = 0;
        for (int r = 1; r < num_ranks; ++r)
            if (loads[r] < loads[best]) best = r;
        owner[ord] = best;
        loads[best] += std::max<std::uint64_t>(units[ord].c_size, 1);
    }
    return owner;
}

// Shared state threaded through the DAG via reference capture.
struct RunCtx {
    int rank = 0;
    int size = 1;
    const AggregatorMpiArgParse* cli = nullptr;

    std::string index_dir;
    std::string staging_root;
    std::string shared_staging_root;
    std::string final_output;
    std::string perfetto_shards_dir;
    std::string my_shard_output;

    std::vector<std::string> all_files;
    std::uint64_t nfiles = 0;
    std::vector<std::vector<GzipMember>> member_map;

    std::vector<std::string> my_files;
    std::vector<int> my_file_ids;
    std::vector<IndexBuildBatchConfig::FileSlice> my_slices;

    std::vector<IndexDatabaseSstWriterContext::Artifacts> main_artifacts;
    std::vector<IndexDatabaseSstWriterContext::Artifacts> agg_artifacts;

    bool failed = false;
    double scan_ms = 0.0;
    double phase_a_ms = 0.0;
    double phase_b_ms = 0.0;
    double phase_c_ms = 0.0;
};

// ---- scan task: co-operative gzip member pre-scan + LPT assignment ----
coro::CoroTask<void> task_scan(RunCtx& ctx, CoroScope& scope) {
    if (ctx.failed) co_return;
    const auto t0 = std::chrono::steady_clock::now();

    std::vector<std::size_t> my_scan_indices;
    for (std::uint64_t i = 0; i < ctx.nfiles; ++i) {
        if (static_cast<int>(i % static_cast<std::uint64_t>(ctx.size)) ==
            ctx.rank) {
            my_scan_indices.push_back(i);
        }
    }
    std::vector<std::vector<GzipMember>> my_scans(my_scan_indices.size());

    co_await scope.scope([&](CoroScope& child) -> coro::CoroTask<void> {
        for (std::size_t si = 0; si < my_scan_indices.size(); ++si) {
            const std::string& path = ctx.all_files[my_scan_indices[si]];
            auto& out = my_scans[si];
            child.spawn([path, &out](CoroScope&) -> coro::CoroTask<void> {
                co_await scan_one_file(path, out);
            });
        }
        co_return;
    });

    std::vector<char> my_packed;
    auto u64 = [&](std::uint64_t v) {
        my_packed.insert(my_packed.end(), reinterpret_cast<const char*>(&v),
                         reinterpret_cast<const char*>(&v) + sizeof(v));
    };
    u64(my_scan_indices.size());
    for (std::size_t si = 0; si < my_scan_indices.size(); ++si) {
        u64(static_cast<std::uint64_t>(my_scan_indices[si]));
        u64(static_cast<std::uint64_t>(my_scans[si].size()));
        for (const auto& m : my_scans[si]) {
            u64(m.c_offset);
            u64(m.c_size);
        }
    }
    const int my_bytes = static_cast<int>(my_packed.size());
    std::vector<int> rank_bytes(ctx.size, 0);
    MPI_Allgather(&my_bytes, 1, MPI_INT, rank_bytes.data(), 1, MPI_INT,
                  MPI_COMM_WORLD);
    std::vector<int> displs(ctx.size, 0);
    int total = 0;
    for (int r = 0; r < ctx.size; ++r) {
        displs[r] = total;
        total += rank_bytes[r];
    }
    std::vector<char> gathered(total);
    MPI_Allgatherv(my_packed.data(), my_bytes, MPI_CHAR, gathered.data(),
                   rank_bytes.data(), displs.data(), MPI_CHAR, MPI_COMM_WORLD);

    ctx.member_map.assign(ctx.nfiles, {});
    for (int r = 0; r < ctx.size; ++r) {
        const char* p = gathered.data() + displs[r];
        const char* end = p + rank_bytes[r];
        auto read_u64 = [&](std::uint64_t& v) -> bool {
            if (end - p < static_cast<std::ptrdiff_t>(sizeof(v))) return false;
            std::memcpy(&v, p, sizeof(v));
            p += sizeof(v);
            return true;
        };
        std::uint64_t count = 0;
        if (!read_u64(count)) continue;
        for (std::uint64_t k = 0; k < count; ++k) {
            std::uint64_t fi = 0, mc = 0;
            if (!read_u64(fi) || !read_u64(mc)) break;
            if (fi >= ctx.nfiles) break;
            ctx.member_map[fi].resize(mc);
            for (std::uint64_t j = 0; j < mc; ++j) {
                if (!read_u64(ctx.member_map[fi][j].c_offset)) break;
                if (!read_u64(ctx.member_map[fi][j].c_size)) break;
            }
        }
    }

    // Fallback for plain .pfw / unreadable / non-dftracer gzip.
    std::uint64_t total_c = 0;
    for (std::uint64_t i = 0; i < ctx.nfiles; ++i) {
        if (ctx.member_map[i].empty()) {
            std::error_code ec;
            std::uint64_t sz = fs::file_size(ctx.all_files[i], ec);
            if (ec) sz = 0;
            ctx.member_map[i].push_back({0, sz});
        }
        for (const auto& m : ctx.member_map[i]) total_c += m.c_size;
    }

    const std::uint64_t target_per_rank =
        (total_c + static_cast<std::uint64_t>(ctx.size) - 1) /
        std::max<std::uint64_t>(static_cast<std::uint64_t>(ctx.size), 1);
    const auto units = build_work_units(ctx.member_map, target_per_rank);
    const auto owner = lpt_assign_units(units, ctx.size);

    for (std::size_t ui = 0; ui < units.size(); ++ui) {
        if (owner[ui] != ctx.rank) continue;
        const auto& u = units[ui];
        ctx.my_files.push_back(ctx.all_files[u.file_idx]);
        ctx.my_file_ids.push_back(static_cast<int>(u.file_idx + 1));
        IndexBuildBatchConfig::FileSlice s;
        s.members = &ctx.member_map[u.file_idx];
        s.member_begin = u.member_begin;
        s.member_end = u.member_end;
        // Disambiguate (file_id, checkpoint_idx) across slices.
        constexpr std::uint64_t CKPT_STRIDE = 1u << 20;
        s.checkpoint_idx_base =
            static_cast<std::uint64_t>(u.member_begin) * CKPT_STRIDE;
        // Only the first slice of a file persists file-scoped CFs.
        s.skip_file_scoped_writes = (u.member_begin != 0);
        ctx.my_slices.push_back(s);
    }

    ctx.scan_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
    if (ctx.rank == 0) {
        std::printf(
            "[rank 0] pre-scan %.2f ms: files=%llu work_units=%zu "
            "target_per_rank=%llu bytes total=%llu bytes\n",
            ctx.scan_ms, static_cast<unsigned long long>(ctx.nfiles),
            units.size(), static_cast<unsigned long long>(target_per_rank),
            static_cast<unsigned long long>(total_c));
    }
    std::printf("[rank %d/%d] files=%zu (work_units)\n", ctx.rank, ctx.size,
                ctx.my_files.size());
    std::fflush(stdout);
    co_return;
}

// ---- phase_a task: distributed-SST index + aggregate build ----
coro::CoroTask<void> task_phase_a(RunCtx& ctx, CoroScope& scope) {
    if (ctx.failed) co_return;

    const auto t0 = std::chrono::steady_clock::now();
    bool ok = true;

    if (!ctx.my_files.empty()) {
        const std::string rank_staging =
            (fs::path(ctx.staging_root) / ("rank_" + std::to_string(ctx.rank)))
                .string();
        std::error_code ec;
        fs::create_directories(rank_staging, ec);

        auto agg_config = std::make_shared<AggregationConfig>();
        agg_config->time_interval_us =
            static_cast<std::uint64_t>(ctx.cli->time_interval * 1000.0);
        agg_config->compute_statistics = true;
        agg_config->track_process_parents = true;
        agg_config->track_default_args = true;

        // Atomic: write_phase spawns N concurrent write workers; a plain
        // size_t would let two workers share an idx and clobber each
        // other's SSTs ("Bad table magic number" at ingest).
        auto batch_counter = std::make_shared<std::atomic<std::size_t>>(0);
        struct SharedArtifacts {
            std::mutex mu;
            std::vector<IndexDatabaseSstWriterContext::Artifacts> list;
        };
        auto artifacts_shared = std::make_shared<SharedArtifacts>();

        auto batch_config = std::make_shared<IndexBuildBatchConfig>();
        batch_config->file_paths = ctx.my_files;
        batch_config->preassigned_file_ids = ctx.my_file_ids;
        batch_config->file_slices = ctx.my_slices;
        batch_config->index_dir = ctx.index_dir;
        batch_config->checkpoint_size = ctx.cli->indexing.checkpoint_size;
        batch_config->force_rebuild = ctx.cli->indexing.force;
        batch_config->build_manifest = false;
        batch_config->parallelism = ctx.cli->pipeline.executor_threads;
        batch_config->rebuild_root_summaries = false;

        const std::string batch_id = "r" + std::to_string(ctx.rank);
        batch_config->dft_visitor_factory =
            [rank_staging, batch_id, agg_config](const std::string& file_path)
            -> std::vector<std::unique_ptr<composites::dft::DftEventVisitor>> {
            std::vector<std::unique_ptr<composites::dft::DftEventVisitor>> v;
            v.push_back(std::make_unique<AggregationVisitor>(
                rank_staging, batch_id + "_agg", 0, *agg_config, file_path));
            return v;
        };
        batch_config->sink_factory =
            [rank_staging, batch_id,
             batch_counter]() -> std::unique_ptr<IndexBatchSink> {
            const std::size_t idx =
                batch_counter->fetch_add(1, std::memory_order_relaxed);
            return std::make_unique<IndexDatabaseSstWriterContext>(
                rank_staging, batch_id + "_" + std::to_string(idx));
        };
        batch_config->sink_commit = [artifacts_shared](IndexBatchSink& sink) {
            auto& sst = static_cast<IndexDatabaseSstWriterContext&>(sink);
            auto a = sst.commit();
            std::lock_guard<std::mutex> lock(artifacts_shared->mu);
            if (!a.empty()) artifacts_shared->list.push_back(std::move(a));
        };

        auto batch_result =
            co_await IndexBatchBuilderUtility::process(&scope, batch_config);

        if (batch_result.failed > 0) {
            for (const auto& r : batch_result.results) {
                if (!r.success) {
                    std::fprintf(
                        stderr, "[rank %d] build failed: %s (file=%s)\n",
                        ctx.rank, r.error_message.c_str(), r.file_path.c_str());
                    ok = false;
                    break;
                }
            }
        }

        if (ok) {
            {
                std::lock_guard<std::mutex> lock(artifacts_shared->mu);
                ctx.main_artifacts = std::move(artifacts_shared->list);
            }

            std::vector<AggregationVisitor*> seen;
            for (auto& file_visitors : batch_result.extra_visitors) {
                for (auto& v : file_visitors) {
                    auto* agg = dynamic_cast<AggregationVisitor*>(v.get());
                    if (!agg) continue;
                    if (std::find(seen.begin(), seen.end(), agg) != seen.end())
                        continue;
                    seen.push_back(agg);
                    for (auto& a : agg->aggregation_artifacts()) {
                        if (!a.empty())
                            ctx.agg_artifacts.push_back(std::move(a));
                    }
                }
            }

            AssociationTracker combined;
            for (auto* agg : seen) {
                auto out = agg->take_output();
                if (out.local_tracker) combined.merge(*out.local_tracker);
            }
            combined.finalize();
            const std::string serialized = combined.serialize();
            const std::string tracker_local =
                (fs::path(rank_staging) / "tracker.bin").string();
            FILE* f = std::fopen(tracker_local.c_str(), "wb");
            if (f) {
                std::fwrite(serialized.data(), 1, serialized.size(), f);
                std::fclose(f);
            }

            // Move per-rank artifacts from node-local staging to shared
            // staging so rank 0 can ingest them from a path visible on
            // every node. No-op when the two roots are the same.
            if (ctx.shared_staging_root != ctx.staging_root) {
                const std::string rank_shared =
                    (fs::path(ctx.shared_staging_root) /
                     ("rank_" + std::to_string(ctx.rank)))
                        .string();
                std::error_code mec;
                fs::create_directories(rank_shared, mec);
                try {
                    for (std::size_t i = 0; i < ctx.main_artifacts.size();
                         ++i) {
                        const std::string sub = (fs::path(rank_shared) /
                                                 ("main_" + std::to_string(i)))
                                                    .string();
                        ctx.main_artifacts[i] =
                            std::move(ctx.main_artifacts[i]).move_to(sub);
                    }
                    for (std::size_t i = 0; i < ctx.agg_artifacts.size(); ++i) {
                        const std::string sub = (fs::path(rank_shared) /
                                                 ("agg_" + std::to_string(i)))
                                                    .string();
                        ctx.agg_artifacts[i] =
                            std::move(ctx.agg_artifacts[i]).move_to(sub);
                    }
                } catch (const std::exception& e) {
                    std::fprintf(stderr,
                                 "[rank %d] failed to relocate SSTs to shared "
                                 "staging: %s\n",
                                 ctx.rank, e.what());
                    ok = false;
                }
                if (ok && !serialized.empty()) {
                    const std::string tracker_shared =
                        (fs::path(rank_shared) / "tracker.bin").string();
                    std::error_code tec;
                    fs::rename(tracker_local, tracker_shared, tec);
                    if (tec) {
                        fs::copy_file(tracker_local, tracker_shared,
                                      fs::copy_options::overwrite_existing,
                                      tec);
                        if (!tec) fs::remove(tracker_local, tec);
                    }
                }
            }
        }
    }

    int ok_int = ok ? 1 : 0, global = 0;
    MPI_Allreduce(&ok_int, &global, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
    if (!global) {
        if (ctx.rank == 0)
            std::fprintf(stderr, "Phase A failed on some rank\n");
        ctx.failed = true;
        co_return;
    }

    ctx.phase_a_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0)
                         .count();
    std::printf(
        "[rank %d/%d] Phase A done in %.2f ms: main_artifacts=%zu "
        "agg_flushes=%zu\n",
        ctx.rank, ctx.size, ctx.phase_a_ms, ctx.main_artifacts.size(),
        ctx.agg_artifacts.size());
    std::fflush(stdout);
    co_return;
}

// ---- phase_b task: Gatherv + rank 0 bulk_ingest + tracker merge ----
coro::CoroTask<void> task_phase_b(RunCtx& ctx) {
    if (ctx.failed) co_return;

    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<char> packed =
        pack_artifact_list(ctx.main_artifacts, ctx.agg_artifacts);
    const int my_bytes = static_cast<int>(packed.size());

    std::vector<int> rank_bytes(ctx.size, 0);
    MPI_Gather(&my_bytes, 1, MPI_INT, rank_bytes.data(), 1, MPI_INT, 0,
               MPI_COMM_WORLD);

    std::vector<int> displs(ctx.size, 0);
    std::vector<char> gathered;
    if (ctx.rank == 0) {
        int total = 0;
        for (int r = 0; r < ctx.size; ++r) {
            displs[r] = total;
            total += rank_bytes[r];
        }
        gathered.resize(total);
    }
    MPI_Gatherv(packed.data(), my_bytes, MPI_CHAR,
                ctx.rank == 0 ? gathered.data() : nullptr, rank_bytes.data(),
                displs.data(), MPI_CHAR, 0, MPI_COMM_WORLD);

    int ok = 1;
    if (ctx.rank == 0) {
        try {
            SstArtifactRegistry registry;
            for (int r = 0; r < ctx.size; ++r) {
                if (rank_bytes[r] == 0) continue;
                const char* p = gathered.data() + displs[r];
                if (!append_artifact_list(p, p + rank_bytes[r], registry)) {
                    std::fprintf(
                        stderr,
                        "[rank 0] failed to parse artifacts from rank %d\n", r);
                    ok = 0;
                    break;
                }
            }
            if (ok) {
                IndexDatabase db(ctx.index_dir);
                db.bulk_ingest(registry, {});
                db.rebuild_root_summaries();

                db.write_agg_global_config(static_cast<std::uint64_t>(
                    ctx.cli->time_interval * 1000.0));
                std::vector<int> all_file_ids;
                all_file_ids.reserve(ctx.nfiles);
                for (std::uint64_t i = 1; i <= ctx.nfiles; ++i)
                    all_file_ids.push_back(static_cast<int>(i));
                db.write_agg_file_markers(all_file_ids);

                AssociationTracker unified;
                for (int r = 0; r < ctx.size; ++r) {
                    char suffix[32];
                    std::snprintf(suffix, sizeof(suffix),
                                  "/rank_%d/tracker.bin", r);
                    std::ifstream f(ctx.shared_staging_root + suffix,
                                    std::ios::binary);
                    if (!f) continue;
                    std::string bytes((std::istreambuf_iterator<char>(f)), {});
                    if (!bytes.empty())
                        unified.merge(AssociationTracker::deserialize(bytes));
                }
                unified.finalize();
                constexpr std::string_view TRACKER_KEY = "__tracker__";
                db.db()->put(TRACKER_KEY, unified.serialize(),
                             dftracer::utils::rocksdb::cf::AGGREGATION);

                // Diagnostic: count aggregation CF keys right after ingest,
                // split by shard-prefixed data vs special 0xFF-prefixed
                // keys (global config / file markers / tracker). If the
                // first bucket is 0 after ingest, the aggregation SSTs
                // never actually landed in the CF.
                {
                    std::size_t shard_keys = 0, special_keys = 0;
                    auto it = db.db()->new_iterator(
                        dftracer::utils::rocksdb::cf::AGGREGATION);
                    for (it->SeekToFirst(); it->Valid(); it->Next()) {
                        auto k = it->key();
                        if (k.size() >= 2 &&
                            static_cast<std::uint8_t>(k[0]) < 0xFF) {
                            shard_keys++;
                        } else {
                            special_keys++;
                        }
                    }
                    std::printf(
                        "[rank 0] AGGREGATION CF after ingest: shard_keys=%zu "
                        "special_keys=%zu\n",
                        shard_keys, special_keys);
                    std::fflush(stdout);
                }
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[rank 0] bulk_ingest failed: %s\n", e.what());
            ok = 0;
        }
    }
    MPI_Bcast(&ok, 1, MPI_INT, 0, MPI_COMM_WORLD);

    if (ctx.rank == 0 && !ctx.cli->keep_staging) {
        std::error_code ec;
        fs::remove_all(ctx.shared_staging_root, ec);
    }
    // Each rank drops its own node-local staging dir; rank 0's shared
    // cleanup above only covers the shared-FS side.
    if (!ctx.cli->keep_staging && ctx.shared_staging_root != ctx.staging_root) {
        const std::string rank_local =
            (fs::path(ctx.staging_root) / ("rank_" + std::to_string(ctx.rank)))
                .string();
        std::error_code ec;
        fs::remove_all(rank_local, ec);
    }

    if (!ok) {
        ctx.failed = true;
        co_return;
    }
    ctx.phase_b_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0)
                         .count();
    if (ctx.rank == 0) {
        std::printf("[rank 0] Phase B done in %.2f ms (ingest %d ranks)\n",
                    ctx.phase_b_ms, ctx.size);
        std::fflush(stdout);
    }
    co_return;
}

// ---- phase_c task: per-rank shard-prefix perfetto write ----
coro::CoroTask<void> task_phase_c(RunCtx& ctx, CoroScope& scope) {
    if (ctx.failed) co_return;

    const auto t0 = std::chrono::steady_clock::now();
    const std::string actual_index_path =
        (fs::path(ctx.index_dir) / ".dftindex").string();
    const std::uint16_t shards_total = AGG_KEY_NUM_SHARDS;
    const std::uint16_t my_shard_begin =
        static_cast<std::uint16_t>(static_cast<std::uint32_t>(shards_total) *
                                   static_cast<std::uint32_t>(ctx.rank) /
                                   static_cast<std::uint32_t>(ctx.size));
    const std::uint16_t my_shard_end =
        (ctx.rank + 1 == ctx.size)
            ? shards_total
            : static_cast<std::uint16_t>(
                  static_cast<std::uint32_t>(shards_total) *
                  static_cast<std::uint32_t>(ctx.rank + 1) /
                  static_cast<std::uint32_t>(ctx.size));

    if (ctx.rank == 0) {
        std::error_code ec;
        fs::create_directories(ctx.perfetto_shards_dir, ec);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "/rank_%05d.json.gz", ctx.rank);
    ctx.my_shard_output = ctx.perfetto_shards_dir + suffix;

    AggregationConfig phase_c_config;
    phase_c_config.time_interval_us =
        static_cast<std::uint64_t>(ctx.cli->time_interval * 1000.0);
    phase_c_config.compute_statistics = true;
    phase_c_config.track_process_parents = true;
    phase_c_config.track_default_args = true;

    auto agg_db =
        EventAggregator::open_read_only_with_merge_operator(actual_index_path);
    composites::dft::aggregators::load_intern_dictionary(*agg_db);

    EventAggregator aggregator(agg_db, 0);

    PerfettoTraceWriterInput input;
    input.output_path = ctx.my_shard_output;
    input.aggregator = &aggregator;
    input.agg_config = &phase_c_config;
    auto tracker = aggregator.build_global_tracker();
    input.tracker = tracker.get();
    input.root_pids = tracker->get_root_pids();
    input.owned_tracker = std::move(tracker);
    input.compute_statistics = true;
    input.compute_percentiles = false;
    input.compress = true;
    input.compression_level = 6;
    input.format = PerfettoEventFormat::COUNTER;
    input.merge_on_sharded = true;
    input.shard_begin = my_shard_begin;
    input.shard_end = my_shard_end;
    input.emit_header = (ctx.rank == 0);
    input.emit_footer = (ctx.rank == ctx.size - 1);

    PerfettoTraceWriterUtility writer;
    const bool ok = co_await scope.spawn(writer, std::move(input));

    int ok_int = ok ? 1 : 0, global = 0;
    MPI_Allreduce(&ok_int, &global, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
    if (!global) {
        if (ctx.rank == 0)
            std::fprintf(stderr, "Phase C failed on some rank\n");
        ctx.failed = true;
        co_return;
    }

    ctx.phase_c_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0)
                         .count();
    std::printf("[rank %d/%d] Phase C scan+write done in %.2f ms\n", ctx.rank,
                ctx.size, ctx.phase_c_ms);
    std::fflush(stdout);
    co_return;
}

// ---- merge task: striped parallel pwrite (Lustre/SSD) or sharded-serial ----
coro::CoroTask<void> task_merge(RunCtx& ctx) {
    if (ctx.failed) co_return;
    const auto t0 = std::chrono::steady_clock::now();

    auto layout = fileio::parallel::detect_layout(ctx.final_output);
    const bool striped = layout.layout == fileio::parallel::FileLayout::STRIPED;

    std::uint64_t my_sz = 0;
    {
        std::error_code ec;
        my_sz = fs::file_size(ctx.my_shard_output, ec);
        if (ec) my_sz = 0;
    }
    std::vector<std::uint64_t> all_sizes(ctx.size, 0);
    MPI_Allgather(&my_sz, 1, MPI_UINT64_T, all_sizes.data(), 1, MPI_UINT64_T,
                  MPI_COMM_WORLD);
    std::uint64_t my_offset = 0, total_bytes = 0;
    for (int r = 0; r < ctx.size; ++r) {
        if (r < ctx.rank) my_offset += all_sizes[r];
        total_bytes += all_sizes[r];
    }

    int ok = 1;
    if (striped) {
        if (ctx.rank == 0) {
            int fd = ::open(ctx.final_output.c_str(),
                            O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd < 0 ||
                ::ftruncate(fd, static_cast<off_t>(total_bytes)) != 0) {
                std::fprintf(stderr, "[rank 0] failed to create %s\n",
                             ctx.final_output.c_str());
                ok = 0;
            }
            if (fd >= 0) ::close(fd);
        }
        MPI_Bcast(&ok, 1, MPI_INT, 0, MPI_COMM_WORLD);
        if (ok) {
            int out_fd = ::open(ctx.final_output.c_str(), O_WRONLY);
            int in_fd = ::open(ctx.my_shard_output.c_str(), O_RDONLY);
            if (out_fd < 0 || in_fd < 0) {
                ok = 0;
            } else {
                std::vector<char> buf(1 << 20);
                off_t out_pos = static_cast<off_t>(my_offset);
                while (true) {
                    ssize_t n = ::read(in_fd, buf.data(), buf.size());
                    if (n == 0) break;
                    if (n < 0) {
                        ok = 0;
                        break;
                    }
                    ssize_t w = ::pwrite(out_fd, buf.data(),
                                         static_cast<std::size_t>(n), out_pos);
                    if (w != n) {
                        ok = 0;
                        break;
                    }
                    out_pos += n;
                }
            }
            if (in_fd >= 0) ::close(in_fd);
            if (out_fd >= 0) ::close(out_fd);
        }
        int global = 1;
        MPI_Allreduce(&ok, &global, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
        ok = global;
    } else if (ctx.rank == 0) {
        std::vector<std::string> shards;
        shards.reserve(ctx.size);
        for (int r = 0; r < ctx.size; ++r) {
            char rs[32];
            std::snprintf(rs, sizeof(rs), "/rank_%05d.json.gz", r);
            shards.emplace_back(ctx.perfetto_shards_dir + rs);
        }
        const int rc =
            co_await fileio::parallel::merge_shards(ctx.final_output, shards);
        if (rc != 0) ok = 0;
    }

    if (!ok) {
        if (ctx.rank == 0) std::fprintf(stderr, "merge step failed\n");
        ctx.failed = true;
        co_return;
    }

    if (ctx.rank == 0) {
        const double merge_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
        std::printf(
            "[rank 0] merge (%s, %llu bytes from %d ranks) -> %s (%.2f ms)\n",
            striped ? "parallel-pwrite" : "sharded-serial",
            static_cast<unsigned long long>(total_bytes), ctx.size,
            ctx.final_output.c_str(), merge_ms);
        std::fflush(stdout);
        if (!ctx.cli->keep_staging) {
            std::error_code ec;
            fs::remove_all(ctx.perfetto_shards_dir, ec);
        }
    }
    co_return;
}

int run(int argc, char** argv) {
    dftracer::utils::logger::init();

    argparse::ArgumentParser program("dftracer_aggregator_mpi",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "MPI driver for the distributed-SST aggregator. Each rank produces "
        "per-rank aggregation SSTs; rank 0 bulk-ingests and the ranks jointly "
        "write the final gzip JSON output.");

    AggregatorMpiArgParse cli(program);
    if (!cli::setup_and_parse(cli, argc, argv)) return 1;

    RunCtx ctx;
    ctx.cli = &cli;
    MPI_Comm_rank(MPI_COMM_WORLD, &ctx.rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ctx.size);

    // Divide executor/io threads by per-node rank count to avoid
    // oversubscription when multiple ranks share a node.
    cli::scale_threads_for_ppn(cli.pipeline, ctx.rank, /*verbose=*/true);

    // Deterministic hash-based intern ids so the same string maps to the
    // same id on every rank, keeping cross-rank aggregation keys identical.
    composites::dft::aggregators::aggregation_intern()
        .enable_deterministic_ids();

    ctx.index_dir = cli.indexing.index_dir;
    if (ctx.index_dir.empty())
        ctx.index_dir = fs::absolute(cli.directory.value).string();
    ctx.staging_root = cli.staging_dir.empty()
                           ? (fs::path(ctx.index_dir) / "_staging").string()
                           : cli.staging_dir;
    ctx.shared_staging_root = (cli.shared_staging_dir.empty() ||
                               cli.shared_staging_dir == ctx.staging_root)
                                  ? ctx.staging_root
                                  : cli.shared_staging_dir;
    ctx.final_output =
        cli::ensure_suffix(fs::absolute(cli.output).string(), ".gz");
    ctx.perfetto_shards_dir =
        (fs::path(ctx.index_dir) / "_perfetto_shards").string();

    std::vector<char> packed_files;
    if (ctx.rank == 0) {
        const std::string dir = fs::absolute(cli.directory.value).string();
        ctx.all_files = enumerate_inputs(dir);
        if (ctx.all_files.empty()) {
            std::fprintf(stderr,
                         "[rank 0] no .pfw/.pfw.gz files in %s, aborting\n",
                         dir.c_str());
        }
        packed_files = pack_paths(ctx.all_files);
        std::error_code ec;
        fs::create_directories(ctx.staging_root, ec);
        if (ctx.shared_staging_root != ctx.staging_root)
            fs::create_directories(ctx.shared_staging_root, ec);
    }
    std::uint64_t packed_size = packed_files.size();
    MPI_Bcast(&packed_size, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
    if (packed_size == 0) return 1;
    if (ctx.rank != 0) packed_files.resize(packed_size);
    MPI_Bcast(packed_files.data(), static_cast<int>(packed_size), MPI_CHAR, 0,
              MPI_COMM_WORLD);
    if (ctx.rank != 0) ctx.all_files = unpack_paths(packed_files);
    ctx.nfiles = ctx.all_files.size();

    // Build the DAG: scan -> phase_a -> phase_b -> phase_c -> merge.
    // Each task is scheduled independently; a downstream task only
    // starts once its parent finishes.
    auto pipeline_config =
        cli::build_pipeline_config("DFTracer MPI", cli.pipeline);
    Pipeline pipeline(pipeline_config);

    auto scan = make_task(
        [&ctx](CoroScope& scope) -> coro::CoroTask<void> {
            co_await task_scan(ctx, scope);
        },
        "scan");
    auto phase_a = make_task(
        [&ctx](CoroScope& scope) -> coro::CoroTask<void> {
            co_await task_phase_a(ctx, scope);
        },
        "phase_a");
    auto phase_b = make_task(
        [&ctx](CoroScope&) -> coro::CoroTask<void> {
            co_await task_phase_b(ctx);
        },
        "phase_b");
    auto phase_c = make_task(
        [&ctx](CoroScope& scope) -> coro::CoroTask<void> {
            co_await task_phase_c(ctx, scope);
        },
        "phase_c");
    auto merge = make_task(
        [&ctx](CoroScope&) -> coro::CoroTask<void> {
            co_await task_merge(ctx);
        },
        "merge");

    phase_a->depends_on(scan);
    phase_b->depends_on(phase_a);
    phase_c->depends_on(phase_b);
    merge->depends_on(phase_c);

    pipeline.set_source(scan);
    pipeline.set_destination(merge);
    pipeline.execute();

    MPI_Barrier(MPI_COMM_WORLD);
    return ctx.failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) { return cli::mpi_main(argc, argv, run); }
