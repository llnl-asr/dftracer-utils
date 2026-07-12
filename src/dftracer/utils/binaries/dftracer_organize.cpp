#include <concurrentqueue.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utils/timer.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_visitor.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/event_aggregator.h>
#include <dftracer/utils/utilities/composites/dft/indexing/index_resolver_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/resolve_and_build.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/group_writer_task.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/manifest_extractor.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/organize_visitor.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>
#include <dftracer/utils/utilities/fileio/parallel/layout.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_sst_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/common/gzip_member_scanner.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common_cli.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites;
using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using namespace dftracer::utils::utilities::composites::dft::reorganize;
using namespace dftracer::utils::utilities::composites::dft::aggregators;
using namespace dftracer::utils::utilities::indexer;

class OrganizeArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory{cli::DirMode::DEFAULT_EMPTY};
    cli::FilesArgs files_args;
    cli::PipelineArgs pipeline;
    cli::IndexingArgs indexing;

    std::string output_dir;
    std::vector<std::string> group_specs;
    std::size_t chunk_size_mb = 0;  // 0 = auto (one file/group on Lustre)
    bool no_compress = false;
    int compression_level = 1;
    bool with_aggregation = false;
    double time_interval_ms = 5000.0;
    std::size_t memory_budget_mb = 0;         // 0 = auto-detect
    std::size_t estimated_file_bytes_mb = 0;  // 0 = auto from input sizes

    explicit OrganizeArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        indexing.force_help = "Force rebuild of indices";
        schema(directory, files_args, pipeline, indexing);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("-o", "--output")
            .help("Output directory")
            .required();

        parser()
            .add_argument("--groups")
            .help(
                "Predicate groups: \"io:cat==\\\"POSIX\\\"\" "
                "\"compute:cat==\\\"APP\\\"\"")
            .nargs(argparse::nargs_pattern::at_least_one)
            .required();

        parser()
            .add_argument("--chunk-size")
            .help(
                "Target chunk size in MB. 0 = auto: one file per group on "
                "Lustre, 256 MB rotation elsewhere (default: 0)")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(0));

        parser()
            .add_argument("--no-compress")
            .help("Write plain .pfw instead of .pfw.gz")
            .flag();

        parser()
            .add_argument("--compression-level")
            .help("Gzip compression level (0-9, default: 1)")
            .scan<'d', int>()
            .default_value(1);

        parser()
            .add_argument("--with-aggregation")
            .help(
                "Build aggregation index on organized chunks so downstream "
                "analyzers skip the first-read aggregation cost")
            .flag();

        parser()
            .add_argument("--time-interval-ms")
            .help(
                "Aggregation bucket size in ms (used with --with-aggregation, "
                "default: 5000)")
            .scan<'g', double>()
            .default_value(5000.0);

        parser()
            .add_argument("--memory-budget-mb")
            .help(
                "Peak memory budget in MB for input file indexing. 0 = auto "
                "(50%% of detected available memory). Bounds peak RSS on "
                "large workloads by processing input files in batches.")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(0));

        parser()
            .add_argument("--estimated-file-bytes-mb")
            .help(
                "Per-file peak memory estimate in MB for index build. "
                "0 = auto (sample input file sizes and apply "
                "gzip/JSON expansion factor). Combined with "
                "--memory-budget-mb to derive flush_every_files.")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(0));
    }

    void post_parse() override {
        output_dir = parser().get<std::string>("--output");
        group_specs = parser().get<std::vector<std::string>>("--groups");
        chunk_size_mb = parser().get<std::size_t>("--chunk-size");
        no_compress = parser().get<bool>("--no-compress");
        compression_level = parser().get<int>("--compression-level");
        with_aggregation = parser().get<bool>("--with-aggregation");
        time_interval_ms = parser().get<double>("--time-interval-ms");
        memory_budget_mb = parser().get<std::size_t>("--memory-budget-mb");
        estimated_file_bytes_mb =
            parser().get<std::size_t>("--estimated-file-bytes-mb");
    }
};

namespace {

// Forward decl: defined below at first use site.
using ChunkLayoutMap = std::unordered_map<
    std::string, std::vector<fileio::parallel::ParallelWriter::MemberSpan>>;

struct OrganizeResult {
    std::size_t total_events_written = 0;
    std::size_t total_events_unmatched = 0;
    std::size_t chunks_created = 0;
    std::size_t source_files_processed = 0;
    std::vector<std::string> output_files;
    /// Per-chunk-file gzip-member layout, captured during Phase 3 by the
    /// striped writer so Phase 4 indexing can slice without re-scanning.
    ChunkLayoutMap chunk_layouts;
    std::unordered_set<std::string> inline_indexed_groups;
};

struct GroupRuntime {
    std::string name;
    std::string group_index_dir;
    std::string staging_root;
    std::shared_ptr<
        moodycamel::ConcurrentQueue<IndexDatabaseSstWriterContext::Artifacts>>
        artifacts_queue;
    std::shared_ptr<std::atomic<std::size_t>> batch_counter;
    std::atomic<bool> indexed_inline{false};
};

static coro::CoroTask<void> run_group_writer_task(
    CoroScope* inner_scope, GroupWriterConfig writer_config,
    std::atomic<std::size_t>* total_events_ptr,
    std::atomic<std::size_t>* chunks_ptr,
    std::vector<std::string>* output_files_ptr, std::mutex* output_mutex_ptr,
    ChunkLayoutMap* chunk_layouts_ptr, GroupRuntime* runtime_ptr) {
    auto writer_result = co_await run_group_writer(inner_scope, writer_config);

    if (writer_result) {
        total_events_ptr->fetch_add(writer_result->events_written);
        chunks_ptr->fetch_add(writer_result->chunks_created);
        if (runtime_ptr) {
            runtime_ptr->indexed_inline.store(writer_result->indexed_inline,
                                              std::memory_order_release);
        }

        std::lock_guard<std::mutex> lock(*output_mutex_ptr);
        for (const auto& f : writer_result->output_files) {
            output_files_ptr->push_back(f);
        }
        if (chunk_layouts_ptr) {
            for (auto& cl : writer_result->chunk_layouts) {
                (*chunk_layouts_ptr)[cl.path] = std::move(cl.members);
            }
        }
    }
}

static coro::CoroTask<void> run_group_indexing(
    CoroScope* scope, const std::string& group_output_dir,
    const std::vector<std::string>& chunk_files,
    const AggregationConfig* agg_config, std::size_t checkpoint_size,
    std::size_t parallelism, std::size_t flush_every_files,
    const ChunkLayoutMap* writer_layouts) {
    if (chunk_files.empty()) co_return;

    const std::string index_path =
        dft::internal::determine_index_path(group_output_dir);
    fs::create_directories(index_path);

    std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> agg_db;
    std::unique_ptr<EventAggregator> merger;
    if (agg_config) {
        agg_db = EventAggregator::open_with_merge_operator(index_path);
        merger = std::make_unique<EventAggregator>(agg_db, /*config_hash=*/0u);
    }

    std::vector<int> chunk_file_ids;
    {
        IndexDatabase coord_db(index_path);
        coord_db.init_schema();
        chunk_file_ids =
            coord_db.register_files(chunk_files, /*build_manifest=*/true);
    }

    // Per-chunk-file gzip member layout. Prefer writer-captured layout (no
    // I/O, exact); fall back to a post-write scan for chunks the writer
    // didn't track (sharded/padded layouts return empty spans).
    auto member_map = std::make_shared<std::vector<std::vector<
        dftracer::utils::utilities::indexer::internal::GzipMember>>>(
        chunk_files.size());
    std::size_t scanned = 0;
    std::size_t from_writer = 0;
    for (std::size_t fi = 0; fi < chunk_files.size(); ++fi) {
        if (writer_layouts) {
            auto it = writer_layouts->find(chunk_files[fi]);
            if (it != writer_layouts->end() && !it->second.empty()) {
                auto& dst = (*member_map)[fi];
                dst.reserve(it->second.size());
                for (const auto& span : it->second) {
                    dst.push_back({span.offset, span.length});
                }
                ++from_writer;
                continue;
            }
        }
        int fd = ::open(chunk_files[fi].c_str(), O_RDONLY);
        if (fd < 0) continue;
        struct stat st;
        if (::fstat(fd, &st) == 0 && st.st_size >= 18) {
            co_await dftracer::utils::utilities::indexer::internal::
                enumerate_gzip_member_candidates(
                    fd, static_cast<std::uint64_t>(st.st_size),
                    (*member_map)[fi]);
        }
        ::close(fd);
        ++scanned;
    }
    DFTRACER_UTILS_LOG_INFO(
        "Phase 4 group '%s': layouts from writer=%zu, rescanned=%zu",
        group_output_dir.c_str(), from_writer, scanned);

    // Build per-file slices targeting
    std::vector<std::string> sliced_file_paths;
    std::vector<int> sliced_file_ids;
    std::vector<IndexBuildBatchConfig::FileSlice> sliced_slices;
    for (std::size_t fi = 0; fi < chunk_files.size(); ++fi) {
        const auto& members = (*member_map)[fi];
        if (members.size() <= 1) {
            sliced_file_paths.push_back(chunk_files[fi]);
            sliced_file_ids.push_back(chunk_file_ids[fi]);
            sliced_slices.push_back({});
            continue;
        }
        std::uint64_t total_c = 0;
        for (const auto& m : members) total_c += m.c_size;
        const std::size_t target_units =
            std::max<std::size_t>(parallelism, std::size_t(1));
        const std::uint64_t target_c =
            (total_c + target_units - 1) / target_units;
        std::size_t begin = 0;
        std::uint64_t accum = 0;
        bool first_slice_for_file = true;
        for (std::size_t i = 0; i < members.size(); ++i) {
            accum += members[i].c_size;
            const bool is_last = (i + 1 == members.size());
            if ((target_c > 0 && accum >= target_c) || is_last) {
                IndexBuildBatchConfig::FileSlice s;
                s.members = &(*member_map)[fi];
                s.member_begin = begin;
                s.member_end = i + 1;
                constexpr std::uint64_t CKPT_STRIDE = 1u << 20;
                s.checkpoint_idx_base =
                    static_cast<std::uint64_t>(begin) * CKPT_STRIDE;
                // Only the first slice persists file-scoped data
                // (chunk_bloom/file_bloom/manifest/file_metadata). Subsequent
                // slices contribute aggregation/system_metrics SSTs only.
                s.skip_file_scoped_writes = !first_slice_for_file;
                first_slice_for_file = false;

                sliced_file_paths.push_back(chunk_files[fi]);
                sliced_file_ids.push_back(chunk_file_ids[fi]);
                sliced_slices.push_back(s);
                begin = i + 1;
                accum = 0;
            }
        }
    }
    DFTRACER_UTILS_LOG_INFO(
        "Phase 4 group indexing: %zu chunk files -> %zu slices "
        "(parallelism=%zu)",
        chunk_files.size(), sliced_file_paths.size(), parallelism);

    const std::string staging_root =
        (fs::path(index_path) / ".dftindex_staging").string();
    fs::create_directories(staging_root);
    auto artifacts_queue = std::make_shared<moodycamel::ConcurrentQueue<
        IndexDatabaseSstWriterContext::Artifacts>>();
    auto batch_counter = std::make_shared<std::atomic<std::size_t>>(0);

    auto batch_config = std::make_shared<IndexBuildBatchConfig>();
    batch_config->file_paths = std::move(sliced_file_paths);
    batch_config->preassigned_file_ids = std::move(sliced_file_ids);
    batch_config->file_slices = std::move(sliced_slices);
    batch_config->index_dir = group_output_dir;
    batch_config->checkpoint_size = checkpoint_size;
    batch_config->parallelism = parallelism;
    batch_config->force_rebuild = false;
    batch_config->build_manifest = true;
    batch_config->use_batch_write = true;
    batch_config->flush_every_files = flush_every_files;
    batch_config->sink_factory =
        [staging_root, batch_counter]() -> std::unique_ptr<IndexBatchSink> {
        const std::size_t idx =
            batch_counter->fetch_add(1, std::memory_order_relaxed);
        return std::make_unique<IndexDatabaseSstWriterContext>(
            staging_root, "batch_" + std::to_string(idx));
    };
    batch_config->sink_commit = [artifacts_queue](IndexBatchSink& sink) {
        auto& sst = static_cast<IndexDatabaseSstWriterContext&>(sink);
        auto a = sst.commit();
        if (!a.empty()) artifacts_queue->enqueue(std::move(a));
    };

    if (agg_config) {
        auto agg_config_ptr = std::make_shared<AggregationConfig>(*agg_config);
        batch_config->dft_visitor_factory =
            [agg_db, agg_config_ptr](const std::string& file_path)
            -> std::vector<std::unique_ptr<composites::dft::DftEventVisitor>> {
            std::vector<std::unique_ptr<composites::dft::DftEventVisitor>>
                visitors;
            visitors.push_back(std::make_unique<AggregationVisitor>(
                agg_db, /*config_hash=*/0u, *agg_config_ptr, file_path));
            return visitors;
        };
        auto* merger_ptr = merger.get();
        batch_config->extra_visitors_drain =
            [merger_ptr](std::vector<std::vector<
                             std::unique_ptr<composites::dft::DftEventVisitor>>>
                             per_file) {
                for (auto& file_visitors : per_file) {
                    for (auto& visitor : file_visitors) {
                        auto* agg_visitor =
                            dynamic_cast<AggregationVisitor*>(visitor.get());
                        if (!agg_visitor) continue;
                        for (const auto& k : agg_visitor->observed_extra_keys())
                            merger_ptr->add_observed_extra_key(k);
                        for (const auto& m :
                             agg_visitor->observed_custom_metrics())
                            merger_ptr->add_observed_custom_metric(m);
                        merger_ptr->merge_chunk(agg_visitor->take_output());
                    }
                }
            };
    }

    auto batch_result = co_await IndexBatchBuilderUtility::process(
        scope, std::move(batch_config));

    {
        SstArtifactRegistry registry;
        IndexDatabaseSstWriterContext::Artifacts a;
        while (artifacts_queue->try_dequeue(a)) {
            registry.append(std::move(a));
        }
        IndexDatabase ingest_db(index_path);
        ingest_db.bulk_ingest(registry, {});
        std::error_code ec;
        fs::remove_all(staging_root, ec);
    }

    if (!agg_config) co_return;

    namespace rcf = dftracer::utils::rocksdb::cf;
    IndexDatabase idx_db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);

    auto batch = agg_db->begin_batch();
    AggGlobalConfig global_cfg;
    global_cfg.time_interval_us = agg_config->time_interval_us;
    global_cfg.config_hash = 0;
    agg_db->put(batch, rcf::AGGREGATION,
                std::string_view(AGG_GLOBAL_CONFIG_KEY, 2),
                serialize_agg_global_config(global_cfg));
    for (const auto& chunk_path : chunk_files) {
        int file_id = idx_db.find_file(chunk_path);
        if (file_id >= 0) {
            auto key = make_agg_file_key(file_id);
            agg_db->put(batch, rcf::AGGREGATION, key, "");
        }
    }
    agg_db->commit_batch(batch);
}

static coro::CoroTask<void> run_manifest_extractor_task(
    ManifestExtractorConfig extractor_config) {
    auto extract_result = co_await extract_from_manifest(extractor_config);
    if (!extract_result) {
        DFTRACER_UTILS_LOG_WARN("ManifestExtractor failed for %s: %s",
                                extractor_config.file_path.c_str(),
                                extract_result.error().message.c_str());
    }
}

struct ProducerScopeInput {
    ResolverResult resolver_result;
    std::vector<FileWorkItem> files_needing_index;
    std::vector<ResolvedFile>
        manifest_entries;  // Files with manifest for extraction
    std::uint64_t checkpoint_size;
    std::size_t executor_threads;
    bool force_rebuild;
    std::size_t flush_every_files;  // per-flush sub-batch inside indexer
    std::vector<PredicateGroup> groups;
    std::vector<std::shared_ptr<coro::Channel<std::shared_ptr<LineBatch>>>>
        group_channels;
    std::unordered_map<std::string, std::size_t> file_index_map;
};

static coro::CoroTask<void> run_producer_scope(CoroScope* producer_scope,
                                               ProducerScopeInput* input) {
    if (!input->files_needing_index.empty()) {
        const std::size_t total_files = input->files_needing_index.size();
        const std::size_t flush_every =
            std::max(input->flush_every_files, std::size_t(1));
        std::printf(
            "  Processing %zu files needing index (flush_every=%zu)...\n",
            total_files, flush_every);

        auto factory = [input](const std::string& file_path)
            -> std::vector<std::unique_ptr<DftEventVisitor>> {
            std::size_t file_idx = 0;
            if (auto it = input->file_index_map.find(file_path);
                it != input->file_index_map.end()) {
                file_idx = it->second;
            }

            OrganizeVisitorConfig visitor_config;
            visitor_config.groups = input->groups;
            visitor_config.group_channels = input->group_channels;
            visitor_config.source_file_idx = file_idx;

            std::vector<std::unique_ptr<DftEventVisitor>> visitors;
            visitors.push_back(
                std::make_unique<OrganizeVisitor>(std::move(visitor_config)));
            return visitors;
        };

        auto batch_config = std::make_shared<IndexBuildBatchConfig>();
        batch_config->file_paths.reserve(total_files);
        for (const auto& item : input->files_needing_index) {
            batch_config->file_paths.push_back(item.file_path);
        }
        const std::string& input_index_path = input->resolver_result.index_path;

        std::vector<int> preassigned_file_ids;
        {
            IndexDatabase coord_db(input_index_path);
            coord_db.init_schema();
            preassigned_file_ids = coord_db.register_files(
                batch_config->file_paths, /*build_manifest=*/true);
        }
        const std::string staging_root =
            (fs::path(input_index_path) / ".dftindex_staging").string();
        fs::create_directories(staging_root);
        auto artifacts_queue = std::make_shared<moodycamel::ConcurrentQueue<
            IndexDatabaseSstWriterContext::Artifacts>>();
        auto batch_counter = std::make_shared<std::atomic<std::size_t>>(0);

        batch_config->preassigned_file_ids = std::move(preassigned_file_ids);
        batch_config->index_dir = input_index_path;
        batch_config->checkpoint_size = input->checkpoint_size;
        batch_config->parallelism = input->executor_threads;
        batch_config->force_rebuild = input->force_rebuild;
        batch_config->build_manifest = true;
        batch_config->use_batch_write = true;
        batch_config->rebuild_root_summaries = false;
        batch_config->flush_every_files = flush_every;
        batch_config->dft_visitor_factory = factory;
        batch_config->sink_factory =
            [staging_root, batch_counter]() -> std::unique_ptr<IndexBatchSink> {
            const std::size_t idx =
                batch_counter->fetch_add(1, std::memory_order_relaxed);
            return std::make_unique<IndexDatabaseSstWriterContext>(
                staging_root, "batch_" + std::to_string(idx));
        };
        batch_config->sink_commit = [artifacts_queue](IndexBatchSink& sink) {
            auto& sst = static_cast<IndexDatabaseSstWriterContext&>(sink);
            auto a = sst.commit();
            if (!a.empty()) artifacts_queue->enqueue(std::move(a));
        };

        co_await IndexBatchBuilderUtility::process(producer_scope,
                                                   batch_config);

        SstArtifactRegistry registry;
        {
            IndexDatabaseSstWriterContext::Artifacts a;
            while (artifacts_queue->try_dequeue(a)) {
                registry.append(std::move(a));
            }
        }
        {
            IndexDatabase ingest_db(input_index_path);
            ingest_db.bulk_ingest(registry, {});
            ingest_db.rebuild_root_summaries();
        }
        std::error_code ec;
        fs::remove_all(staging_root, ec);
    }

    if (!input->manifest_entries.empty()) {
        std::printf("  Processing %zu files via manifest extraction...\n",
                    input->manifest_entries.size());

        const auto& index_path = input->resolver_result.index_path;
        for (const auto& entry : input->manifest_entries) {
            ManifestExtractorConfig extractor_config;
            extractor_config.file_path = entry.file_path;
            extractor_config.index_path = index_path;
            extractor_config.source_file_idx = entry.file_index;
            extractor_config.groups = input->groups;
            extractor_config.group_channels = input->group_channels;

            producer_scope->spawn(
                [extractor_config](CoroScope&) -> coro::CoroTask<void> {
                    co_await run_manifest_extractor_task(extractor_config);
                });
        }
    }
}

coro::CoroTask<int> run_organize(const OrganizeArgParse* cli) {
    const auto& output_dir = cli->output_dir;
    const auto& index_dir = cli->indexing.index_dir;
    const auto checkpoint_size = cli->indexing.checkpoint_size;
    const auto force_rebuild = cli->indexing.force;
    const auto no_compress = cli->no_compress;
    const auto compression_level = cli->compression_level;
    const auto executor_threads = cli->pipeline.executor_threads;
    const bool time_profiling = cli->pipeline.time_profiling;

    auto groups = parse_group_specs(cli->group_specs);
    if (groups.empty()) {
        DFTRACER_UTILS_LOG_ERROR("%s", "No groups specified.");
        co_return 1;
    }

    fs::create_directories(output_dir);

    std::size_t chunk_size_mb = cli->chunk_size_mb;
    if (chunk_size_mb == 0) {
        auto layout = fileio::parallel::detect_layout(output_dir);
        if (layout.fs == fileio::parallel::FilesystemKind::LUSTRE) {
            chunk_size_mb = 0;  // single file per group, no rotation
        } else {
            chunk_size_mb = 256;
        }
    }

    std::printf("==========================================\n");
    std::printf("DFTracer Trace Reorganizer (Streaming)\n");
    std::printf("==========================================\n");
    std::printf("  Output directory: %s\n", output_dir.c_str());
    if (chunk_size_mb == 0) {
        std::printf("  Chunk size: auto (one file per group)\n");
    } else {
        std::printf("  Chunk size: %zu MB\n", chunk_size_mb);
    }
    std::printf("  Compress: %s\n", no_compress ? "false" : "true");
    std::printf("  Executor threads: %zu\n", executor_threads);
    std::printf("  Groups: %zu\n", groups.size());
    for (const auto& g : groups) {
        std::printf("    %s: %s\n", g.name.c_str(),
                    g.query.empty() ? "(remainder)" : g.query.c_str());
    }
    std::printf("==========================================\n\n");

    Timer stages_storage("dftracer_organize");
    Timer* stages = time_profiling ? &stages_storage : nullptr;
    Timer overall(true);

    OrganizeResult result;
    // Success/failure signal shared across the two pipeline tasks. Starts as an
    // error so any early exit or throw before Phase 3 completes leaves it
    // failed; set to a value once Phase 3 succeeds.
    Result<void> organize_status =
        make_error(ErrorCode::INTERNAL, "organize pipeline did not complete");

    auto pipeline_config =
        cli::build_pipeline_config("Organize: Streaming", cli->pipeline);

    Pipeline pipeline(pipeline_config);

    auto* cli_ptr = cli;
    auto* groups_ptr = &groups;
    auto* result_ptr = &result;
    auto* status_ptr = &organize_status;

    auto organize_task = make_task(
        [cli_ptr, groups_ptr, result_ptr, status_ptr, output_dir, index_dir,
         checkpoint_size, force_rebuild, no_compress, compression_level,
         executor_threads, chunk_size_mb,
         stages](CoroScope& ctx) -> coro::CoroTask<void> {
            // Phase 1: Scan & Partition
            DFTRACER_UTILS_LOG_INFO("%s", "Phase 1 begin: scan & partition");
            std::printf("Phase 1: Scanning and partitioning files...\n");

            IndexResolverUtility resolver;
            ResolverInput resolver_input;
            ResolverResult resolve_result;
            {
                ScopedTimer _t(stages, "phase1_scan_partition");
                resolver_input.directory = cli_ptr->directory.value;
                resolver_input.files = cli_ptr->files_args.value;
                resolver_input.index_dir = index_dir;
                resolver_input.require_manifest = true;

                co_await ensure_indexes_fresh(&ctx, cli_ptr->directory.value,
                                              cli_ptr->files_args.value,
                                              index_dir, force_rebuild);
                resolve_result = co_await resolver.process(resolver_input);
            }

            if (resolve_result.all_files.empty()) {
                DFTRACER_UTILS_LOG_ERROR(
                    "%s", "No input files. Use --files or --directory.");
                *status_ptr =
                    make_error(ErrorCode::NOT_FOUND,
                               "No input files. Use --files or --directory.");
                co_return;
            }

            std::printf("  Total files: %zu\n",
                        resolve_result.all_files.size());
            std::printf("  Files needing index: %zu\n",
                        resolve_result.needs_checkpoint.size() +
                            resolve_result.needs_manifest.size());
            std::printf("  Already indexed: %zu\n",
                        resolve_result.cached.size());
            DFTRACER_UTILS_LOG_INFO(
                "Phase 1 complete: %zu files (%zu need index, %zu cached)",
                resolve_result.all_files.size(),
                resolve_result.needs_checkpoint.size() +
                    resolve_result.needs_manifest.size(),
                resolve_result.cached.size());

            // Build file index map for source_file_idx lookup
            std::unordered_map<std::string, std::size_t> file_index_map;
            for (std::size_t i = 0; i < resolve_result.all_files.size(); ++i) {
                file_index_map[resolve_result.all_files[i]] = i;
            }

            // Build source file info for provenance tracking
            std::vector<SourceFileInfo> source_files;
            source_files.reserve(resolve_result.all_files.size());
            for (std::size_t i = 0; i < resolve_result.all_files.size(); ++i) {
                source_files.push_back(SourceFileInfo{
                    .file_path = resolve_result.all_files[i],
                    .index_path = resolve_result.index_path,
                    .num_checkpoints = 0,
                });
            }

            // Phase 2: Setup channels and writers
            DFTRACER_UTILS_LOG_INFO("%s",
                                    "Phase 2 begin: setup streaming pipeline");
            std::printf("Phase 2: Starting streaming pipeline...\n");

            std::vector<
                std::shared_ptr<coro::Channel<std::shared_ptr<LineBatch>>>>
                group_channels;
            std::atomic<std::size_t> total_events_written{0};
            std::atomic<std::size_t> chunks_created{0};
            std::vector<std::string> all_output_files;
            std::mutex output_files_mutex;
            // Each group writer writes distinct chunk paths, so concurrent
            // inserts into chunk_layouts are key-disjoint and safe under
            // output_files_mutex (already taken when appending output_files).
            // Stored on result_ptr so Phase 4 (separate task) can read it.

            {
                ScopedTimer _t(stages, "phase2_setup_channels");
                group_channels.reserve(groups_ptr->size());

                for (std::size_t i = 0; i < groups_ptr->size(); ++i) {
                    group_channels.push_back(
                        std::make_shared<
                            coro::Channel<std::shared_ptr<LineBatch>>>(
                            executor_threads * 4));
                }
            }

            auto* total_events_ptr = &total_events_written;
            auto* chunks_ptr = &chunks_created;
            auto* output_files_ptr = &all_output_files;
            auto* output_mutex_ptr = &output_files_mutex;
            auto* chunk_layouts_ptr = &result_ptr->chunk_layouts;
            const auto* source_files_ptr = &source_files;

            DFTRACER_UTILS_LOG_INFO("%s", "Phase 2 complete");
            DFTRACER_UTILS_LOG_INFO("%s",
                                    "Phase 3 begin: producers + group writers");
            std::vector<std::unique_ptr<GroupRuntime>> group_runtimes;
            group_runtimes.reserve(groups_ptr->size());
            for (const auto& group : *groups_ptr) {
                auto rt = std::make_unique<GroupRuntime>();
                rt->name = group.name;
                const std::string group_output_dir =
                    output_dir + "/" + group.name;
                fs::create_directories(group_output_dir);
                rt->group_index_dir =
                    dft::internal::determine_index_path(group_output_dir);
                fs::create_directories(rt->group_index_dir);
                rt->staging_root =
                    (fs::path(rt->group_index_dir) / ".dftindex_staging")
                        .string();
                fs::create_directories(rt->staging_root);
                rt->artifacts_queue =
                    std::make_shared<moodycamel::ConcurrentQueue<
                        IndexDatabaseSstWriterContext::Artifacts>>();
                rt->batch_counter =
                    std::make_shared<std::atomic<std::size_t>>(0);
                group_runtimes.push_back(std::move(rt));
            }

            for (std::size_t g = 0; g < groups_ptr->size(); ++g) {
                const auto& group = (*groups_ptr)[g];
                auto channel = group_channels[g];
                GroupRuntime* runtime = group_runtimes[g].get();

                GroupWriterConfig writer_config;
                writer_config.group_name = group.name;
                writer_config.group_query = group.query;
                writer_config.output_dir = output_dir;
                writer_config.chunk_size_bytes = chunk_size_mb * 1024 * 1024;
                writer_config.compress = !no_compress;
                writer_config.compression_level = compression_level;
                writer_config.input_channel = channel;
                writer_config.source_files = source_files_ptr;
                writer_config.build_output_index = true;
                writer_config.index_dir = runtime->group_index_dir;
                writer_config.staging_root = runtime->staging_root;
                writer_config.artifacts_queue = runtime->artifacts_queue;
                writer_config.batch_counter = runtime->batch_counter;
                writer_config.with_aggregation = cli_ptr->with_aggregation;
                writer_config.agg_time_interval_us =
                    cli_ptr->time_interval_ms * 1000.0;
                writer_config.bloom_dimensions = std::vector<std::string>(
                    indexer::DEFAULT_BLOOM_DIMENSIONS.begin(),
                    indexer::DEFAULT_BLOOM_DIMENSIONS.end());
                writer_config.bloom_config.build_manifest = true;

                ctx.spawn(
                    [writer_config, total_events_ptr, chunks_ptr,
                     output_files_ptr, output_mutex_ptr, chunk_layouts_ptr,
                     runtime](CoroScope& inner_scope) -> coro::CoroTask<void> {
                        co_await run_group_writer_task(
                            &inner_scope, writer_config, total_events_ptr,
                            chunks_ptr, output_files_ptr, output_mutex_ptr,
                            chunk_layouts_ptr, runtime);
                    });
            }

            // Phase 3b & 3c: Run producers in a nested scope
            // When this scope completes, all producers have finished
            const auto total_source_files = resolve_result.all_files.size();
            const std::size_t memory_budget = compute_memory_budget(
                cli_ptr->memory_budget_mb * 1024ULL * 1024ULL);
            const std::size_t per_file_bytes = estimate_per_file_bytes(
                resolve_result.all_file_sizes,
                cli_ptr->estimated_file_bytes_mb * 1024ULL * 1024ULL);
            const std::size_t phase3_flush_every =
                compute_file_batch_size(memory_budget, per_file_bytes, 4);
            {
                ScopedTimer _t(stages, "phase3_producers");
                auto producer_input = std::make_shared<ProducerScopeInput>();
                producer_input->resolver_result = std::move(resolve_result);
                producer_input->files_needing_index =
                    std::move(producer_input->resolver_result.needs_checkpoint);
                for (auto& item :
                     producer_input->resolver_result.needs_manifest) {
                    producer_input->files_needing_index.push_back(
                        std::move(item));
                }
                producer_input->manifest_entries =
                    std::move(producer_input->resolver_result.cached);
                producer_input->checkpoint_size = checkpoint_size;
                producer_input->executor_threads = executor_threads;
                producer_input->force_rebuild = force_rebuild;
                producer_input->flush_every_files = phase3_flush_every;
                std::printf(
                    "  Memory budget: %.2f GB; per-file peak estimate: %.2f "
                    "GB; flush_every: %zu files\n",
                    static_cast<double>(memory_budget) /
                        (1024.0 * 1024.0 * 1024.0),
                    static_cast<double>(per_file_bytes) /
                        (1024.0 * 1024.0 * 1024.0),
                    phase3_flush_every);
                producer_input->groups = *groups_ptr;
                producer_input->group_channels = group_channels;
                producer_input->file_index_map = std::move(file_index_map);

                // GCC 12 coroutine bug: capturing shared_ptr by value in
                // coroutine lambdas corrupts refcount. Capture raw pointer
                // instead - lifetime is guaranteed by outer shared_ptr.
                auto* producer_input_raw = producer_input.get();
                co_await ctx.scope(
                    [producer_input_raw](
                        CoroScope& producer_scope) -> coro::CoroTask<void> {
                        co_await run_producer_scope(&producer_scope,
                                                    producer_input_raw);
                    });
            }

            // Producers done - close channels to signal EOF to writers
            for (auto& channel : group_channels) {
                channel->close();
            }
            DFTRACER_UTILS_LOG_INFO("%s",
                                    "Phase 3 producers complete; waiting for "
                                    "group writers to drain");

            // Wait for all writers to complete
            co_await ctx.join_all();

            for (auto& rt : group_runtimes) {
                if (!rt->indexed_inline.load(std::memory_order_acquire)) {
                    continue;
                }
                SstArtifactRegistry registry;
                IndexDatabaseSstWriterContext::Artifacts a;
                while (rt->artifacts_queue->try_dequeue(a)) {
                    registry.append(std::move(a));
                }
                IndexDatabase ingest_db(rt->group_index_dir);
                ingest_db.bulk_ingest(registry, {});
                std::error_code ec;
                fs::remove_all(rt->staging_root, ec);
                result_ptr->inline_indexed_groups.insert(rt->name);
            }

            result_ptr->total_events_written = total_events_written.load();
            result_ptr->chunks_created = chunks_created.load();
            result_ptr->source_files_processed = total_source_files;
            result_ptr->output_files = std::move(all_output_files);

            *status_ptr = Result<void>{};
            DFTRACER_UTILS_LOG_INFO(
                "Phase 3 complete: %zu chunks created, %zu events written",
                result_ptr->chunks_created, result_ptr->total_events_written);
        },
        "OrganizeStreaming");

    auto index_task = make_task(
        [cli_ptr, groups_ptr, result_ptr, status_ptr, output_dir,
         checkpoint_size,
         executor_threads](CoroScope& ctx) -> coro::CoroTask<void> {
            if (!*status_ptr) co_return;

            AggregationConfig agg_config;
            const AggregationConfig* agg_ptr = nullptr;
            if (cli_ptr->with_aggregation) {
                agg_config.time_interval_us = static_cast<std::uint64_t>(
                    cli_ptr->time_interval_ms * 1000.0);
                agg_config.compute_statistics = true;
                agg_config.track_process_parents = true;
                agg_config.track_default_args = true;
                agg_ptr = &agg_config;
                DFTRACER_UTILS_LOG_INFO(
                    "Phase 4 begin: indexes + aggregation "
                    "(time_interval=%.2f ms)",
                    cli_ptr->time_interval_ms);
                std::printf(
                    "Phase 4: Building indexes + aggregation "
                    "(time_interval=%.2f ms) ...\n",
                    cli_ptr->time_interval_ms);
            } else {
                DFTRACER_UTILS_LOG_INFO("%s", "Phase 4 begin: indexes");
                std::printf("Phase 4: Building indexes ...\n");
            }

            const std::size_t phase4_memory_budget = compute_memory_budget(
                cli_ptr->memory_budget_mb * 1024ULL * 1024ULL);
            const std::size_t override_per_file_bytes =
                cli_ptr->estimated_file_bytes_mb * 1024ULL * 1024ULL;

            filesystem::PatternDirectoryScannerUtility chunk_scanner;
            for (const auto& group : *groups_ptr) {
                const std::string group_dir = output_dir + "/" + group.name;
                if (!fs::exists(group_dir)) continue;
                if (result_ptr->inline_indexed_groups.count(group.name)) {
                    DFTRACER_UTILS_LOG_INFO(
                        "Phase 4: skipping group '%s' (indexed inline)",
                        group.name.c_str());
                    continue;
                }

                filesystem::PatternDirectoryScannerUtilityInput scan_input{
                    group_dir, {".pfw", ".pfw.gz"}, false};
                auto entries = co_await chunk_scanner.process(scan_input);
                if (entries.empty()) continue;

                std::sort(entries.begin(), entries.end(),
                          [](const filesystem::FileEntry& a,
                             const filesystem::FileEntry& b) {
                              return a.path.string() < b.path.string();
                          });

                std::vector<std::string> chunk_files;
                std::vector<std::size_t> chunk_sizes;
                chunk_files.reserve(entries.size());
                chunk_sizes.reserve(entries.size());
                for (const auto& e : entries) {
                    chunk_files.push_back(e.path.string());
                    chunk_sizes.push_back(e.size);
                }

                const std::size_t per_file_bytes = estimate_per_file_bytes(
                    chunk_sizes, override_per_file_bytes);
                const std::size_t flush_every = compute_file_batch_size(
                    phase4_memory_budget, per_file_bytes, 4);
                std::printf(
                    "  %s: %zu chunks; per-file peak: %.2f GB; "
                    "flush_every: %zu chunks\n",
                    group.name.c_str(), chunk_files.size(),
                    static_cast<double>(per_file_bytes) /
                        (1024.0 * 1024.0 * 1024.0),
                    flush_every);
                DFTRACER_UTILS_LOG_INFO(
                    "Phase 4: indexing group '%s' (%zu chunks)",
                    group.name.c_str(), chunk_files.size());
                co_await run_group_indexing(
                    &ctx, group_dir, chunk_files, agg_ptr, checkpoint_size,
                    executor_threads, flush_every, &result_ptr->chunk_layouts);
                DFTRACER_UTILS_LOG_INFO("Phase 4: group '%s' complete",
                                        group.name.c_str());
            }
            DFTRACER_UTILS_LOG_INFO("%s", "Phase 4 complete");
        },
        "OrganizeIndexing");

    index_task->depends_on(organize_task);

    pipeline.set_source(organize_task);
    pipeline.set_destination(index_task);
    pipeline.execute();

    if (organize_status) {
        const std::string manifest_path = output_dir + "/manifest.json";
        std::ofstream manifest_out(manifest_path);
        if (manifest_out.is_open()) {
            auto escape = [](const std::string& s) {
                std::string out;
                out.reserve(s.size());
                for (char c : s) {
                    switch (c) {
                        case '"':
                            out += "\\\"";
                            break;
                        case '\\':
                            out += "\\\\";
                            break;
                        case '\n':
                            out += "\\n";
                            break;
                        case '\r':
                            out += "\\r";
                            break;
                        case '\t':
                            out += "\\t";
                            break;
                        default:
                            out += c;
                            break;
                    }
                }
                return out;
            };
            manifest_out << "{\n";
            manifest_out << "  \"version\": 1,\n";
            manifest_out << "  \"tool\": \"dftracer_organize\",\n";
            manifest_out << "  \"groups\": {\n";
            for (std::size_t i = 0; i < groups.size(); ++i) {
                manifest_out << "    \"" << escape(groups[i].name) << "\": \""
                             << escape(groups[i].name) << "\"";
                if (i + 1 < groups.size()) manifest_out << ",";
                manifest_out << "\n";
            }
            manifest_out << "  },\n";
            manifest_out << "  \"group_queries\": {\n";
            for (std::size_t i = 0; i < groups.size(); ++i) {
                manifest_out << "    \"" << escape(groups[i].name) << "\": \""
                             << escape(groups[i].query) << "\"";
                if (i + 1 < groups.size()) manifest_out << ",";
                manifest_out << "\n";
            }
            manifest_out << "  }\n";
            manifest_out << "}\n";
        } else {
            DFTRACER_UTILS_LOG_WARN("Failed to write manifest at %s",
                                    manifest_path.c_str());
        }
    }

    overall.stop();
    double duration_ms = static_cast<double>(overall.elapsed()) / 1e6;

    std::printf("\n==========================================\n");
    std::printf("Reorganization Complete\n");
    std::printf("==========================================\n");
    std::printf("  Time: %.2f seconds\n", duration_ms / 1000.0);
    std::printf("  Input files: %zu\n", result.source_files_processed);
    std::printf("  Events routed: %zu\n", result.total_events_written);
    std::printf("  Chunks created: %zu\n", result.chunks_created);
    if (organize_status) {
        std::printf("  Manifest: %s/manifest.json\n", output_dir.c_str());
    }
    std::printf("  Output files:\n");
    for (const auto& f : result.output_files) {
        if (fs::exists(f)) {
            std::printf(
                "    %s (%.2f MB)\n", f.c_str(),
                static_cast<double>(fs::file_size(f)) / (1024.0 * 1024.0));
        }
    }
    if (stages) {
        std::printf("\n  Stage Timing:\n");
        stages->print_stages("    ");
    }
    std::printf("==========================================\n");

    co_return organize_status ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    return cli::cli_main<OrganizeArgParse>(
        argc, argv, "dftracer_organize",
        "Reorganize DFTracer trace files by routing events to "
        "predicate-based groups with chunked output.",
        [](OrganizeArgParse& cli) { return run_organize(&cli).get(); });
}
