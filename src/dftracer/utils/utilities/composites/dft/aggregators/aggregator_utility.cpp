#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/rocksdb/column_families.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_augmentation.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_drain.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_output.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_runner.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_visitor.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/association_resolver_utility.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/event_aggregator.h>
#include <dftracer/utils/utilities/composites/dft/indexing/index_resolver_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#endif

#include <dftracer/utils/core/common/transparent_string_hash.h>

#include <algorithm>
#include <cinttypes>
#include <set>
#include <unordered_set>

namespace dftracer::utils::utilities::composites::dft::aggregators {

// ---------------------------------------------------------------------------
// AggregatorInput fluent builders
// ---------------------------------------------------------------------------

AggregatorInput& AggregatorInput::with_directory(const std::string& dir) {
    directory = dir;
    return *this;
}

AggregatorInput& AggregatorInput::with_config(const AggregationConfig& cfg) {
    config = cfg;
    return *this;
}

AggregatorInput& AggregatorInput::with_checkpoint_size(std::size_t sz) {
    checkpoint_size = sz;
    return *this;
}

AggregatorInput& AggregatorInput::with_index_dir(const std::string& dir) {
    index_dir = dir;
    return *this;
}

AggregatorInput& AggregatorInput::with_force_rebuild(bool force) {
    force_rebuild = force;
    return *this;
}

AggregatorInput& AggregatorInput::with_parallelism(std::size_t n) {
    parallelism = n;
    return *this;
}

AggregatorInput& AggregatorInput::with_event_batch_size(std::size_t sz) {
    event_batch_size = sz;
    return *this;
}

// ---------------------------------------------------------------------------
// AggregationBatch::to_arrow
// ---------------------------------------------------------------------------

#ifdef DFTRACER_UTILS_ENABLE_ARROW
using common::arrow::ArrowExportResult;
using common::arrow::ColumnType;
using common::arrow::RecordBatchBuilder;

ArrowExportResult AggregationBatch::to_arrow() const {
    RecordBatchBuilder builder;

    // Use precomputed global columns if available, otherwise discover locally.
    std::vector<std::uint32_t> local_extra_key_ids;
    std::vector<std::string> local_custom_metric_names;
    if (!global_extra_key_ids || !global_custom_metric_names) {
        std::set<std::uint32_t> extra_key_id_set;
        std::set<std::string_view, std::less<>> custom_metric_name_set;
        for (const auto& entry : entries) {
            if (entry.key.extra_keys && !entry.key.extra_keys->empty()) {
                for (const auto& [k, v] : *entry.key.extra_keys) {
                    extra_key_id_set.insert(k);
                }
            }
            if (entry.metrics.custom_metrics &&
                !entry.metrics.custom_metrics->empty()) {
                for (const auto& [name, _] : *entry.metrics.custom_metrics) {
                    custom_metric_name_set.insert(name);
                }
            }
        }
        local_extra_key_ids.assign(extra_key_id_set.begin(),
                                   extra_key_id_set.end());
        local_custom_metric_names.assign(custom_metric_name_set.begin(),
                                         custom_metric_name_set.end());
    }
    const auto& extra_key_ids =
        global_extra_key_ids ? *global_extra_key_ids : local_extra_key_ids;
    const auto& custom_metric_names = global_custom_metric_names
                                          ? *global_custom_metric_names
                                          : local_custom_metric_names;

    // Build schema: batch_type + fixed columns + extra keys + custom metrics
    std::vector<common::arrow::ColumnSpec> schema = {
        {"batch_type", ColumnType::INT64},  {"cat", ColumnType::STRING},
        {"name", ColumnType::STRING},       {"pid", ColumnType::UINT64},
        {"tid", ColumnType::UINT64},        {"hhash", ColumnType::STRING},
        {"fhash", ColumnType::STRING},      {"time_bucket", ColumnType::UINT64},
        {"count", ColumnType::UINT64},      {"dur_total", ColumnType::UINT64},
        {"dur_min", ColumnType::UINT64},    {"dur_max", ColumnType::UINT64},
        {"dur_mean", ColumnType::DOUBLE},   {"dur_std", ColumnType::DOUBLE},
        {"size_total", ColumnType::UINT64}, {"size_min", ColumnType::UINT64},
        {"size_max", ColumnType::UINT64},   {"size_mean", ColumnType::DOUBLE},
        {"size_std", ColumnType::DOUBLE},   {"ts", ColumnType::UINT64},
        {"te", ColumnType::UINT64},
    };
    // Add CI columns when batch has approximated entries
    if (has_approximated_entries) {
        schema.push_back({"count_ci_lower", ColumnType::DOUBLE});
        schema.push_back({"count_ci_upper", ColumnType::DOUBLE});
    }
    for (auto id : extra_key_ids) {
        schema.push_back({std::string(aggregation_intern().resolve(id)),
                          ColumnType::STRING});
    }
    // Custom metric suffixed names
    struct MetricSuffix {
        const char* suffix;
        ColumnType type;
    };
    static constexpr MetricSuffix cm_schema[] = {
        {"_total", ColumnType::UINT64}, {"_min", ColumnType::UINT64},
        {"_max", ColumnType::UINT64},   {"_mean", ColumnType::DOUBLE},
        {"_std", ColumnType::DOUBLE},
    };
    for (auto cm : custom_metric_names) {
        for (const auto& [suffix, type] : cm_schema) {
            std::string col_name;
            col_name.reserve(cm.size() + 8);
            col_name.append(cm);
            col_name.append(suffix);
            schema.push_back({std::move(col_name), type});
        }
    }

    builder.declare_schema(schema);
    builder.reserve(entries.size());

    for (const auto& entry : entries) {
        const auto& key = entry.key;
        const auto& metrics = entry.metrics;
        std::size_t ci = 0;
        builder.append_int64(ci++, static_cast<int64_t>(batch_type));
        builder.append_string(ci++, key.cat());
        builder.append_string(ci++, key.name());
        builder.append_uint64(ci++, key.pid);
        builder.append_uint64(ci++, key.tid);
        builder.append_string(ci++, key.hhash());
        builder.append_string(ci++, key.fhash());
        builder.append_uint64(ci++, key.time_bucket);
        builder.append_uint64(ci++, metrics.count);
        builder.append_uint64(ci++, metrics.duration.total);
        builder.append_uint64(ci++,
                              metrics.count > 0 ? metrics.duration.min : 0);
        builder.append_uint64(ci++, metrics.duration.max);
        builder.append_double(ci++, metrics.duration.mean);
        builder.append_double(ci++, metrics.duration.get_stddev());
        builder.append_uint64(ci++, metrics.size.total);
        builder.append_uint64(ci++, metrics.count > 0 ? metrics.size.min : 0);
        builder.append_uint64(ci++, metrics.size.max);
        builder.append_double(ci++, metrics.size.mean);
        builder.append_double(ci++, metrics.size.get_stddev());
        builder.append_uint64(ci++, metrics.ts);
        builder.append_uint64(ci++, metrics.te);

        for (auto extra_key_id : extra_key_ids) {
            bool found_extra_key = false;
            if (key.extra_keys) {
                for (const auto& [present_id, value_id] : *key.extra_keys) {
                    if (present_id == extra_key_id) {
                        builder.append_string(
                            ci++, aggregation_intern().resolve(value_id));
                        found_extra_key = true;
                        break;
                    }
                }
            }
            if (!found_extra_key) {
                builder.append_null(ci++);
            }
        }

        for (auto cm : custom_metric_names) {
            if (metrics.custom_metrics) {
                auto it = metrics.custom_metrics->find(cm);
                if (it != metrics.custom_metrics->end()) {
                    const auto& ms = it->second;
                    builder.append_uint64(ci++, ms.total);
                    builder.append_uint64(ci++, metrics.count > 0 ? ms.min : 0);
                    builder.append_uint64(ci++, ms.max);
                    builder.append_double(ci++, ms.mean);
                    builder.append_double(ci++, ms.get_stddev());
                    continue;
                }
            }
            for (std::size_t j = 0; j < std::size(cm_schema); ++j)
                builder.append_null(ci++);
        }

        // Add CI columns when batch has approximated entries
        if (has_approximated_entries) {
            builder.append_double(ci++, entry.count_ci.lower);
            builder.append_double(ci++, entry.count_ci.upper);
        }

        builder.end_row();
    }

    return builder.finish();
}

// ---------------------------------------------------------------------------
// AggregationBatch::to_dfanalyzer_arrow
// ---------------------------------------------------------------------------

namespace {

// IO category constants matching dfanalyzer IOCategory enum
enum class IOCategory : std::int8_t {
    READ = 1,
    WRITE = 2,
    METADATA = 3,
    PCTL = 4,
    IPC = 5,
    OTHER = 6,
    SYNC = 7,
};

IOCategory get_io_category(std::string_view func_name) {
    using namespace dftracer::utils::utilities::composites::dft::internal;
    for (auto op : posix_ops::READ)
        if (op == func_name) return IOCategory::READ;
    for (auto op : posix_ops::WRITE)
        if (op == func_name) return IOCategory::WRITE;
    for (auto op : posix_ops::SYNC)
        if (op == func_name) return IOCategory::SYNC;
    for (auto op : posix_ops::METADATA)
        if (op == func_name) return IOCategory::METADATA;
    return IOCategory::OTHER;
}

std::string resolve_hash(
    const std::unordered_map<std::string, std::string>* hash_table,
    std::string_view hash) {
    if (!hash_table || hash.empty()) return std::string(hash);
    auto it = hash_table->find(std::string(hash));
    if (it != hash_table->end()) return it->second;
    return std::string(hash);
}

std::string build_proc_name(std::string_view host_name, std::string_view hhash,
                            std::uint64_t pid, std::uint64_t tid) {
    std::string result = "app#";
    if (!host_name.empty()) {
        result.append(host_name);
    } else if (!hhash.empty()) {
        result.append(hhash);
    } else {
        result.append("unknown");
    }
    result.push_back('#');
    result.append(std::to_string(pid));
    result.push_back('#');
    result.append(std::to_string(tid));
    return result;
}

}  // namespace

ArrowExportResult AggregationBatch::to_dfanalyzer_arrow(
    const DfanalyzerContext& ctx) const {
    RecordBatchBuilder builder;

    // Bucket width in microseconds
    auto bucket_width_us =
        static_cast<std::uint64_t>(ctx.time_granularity * ctx.time_resolution);

    if (batch_type == AggregationBatchType::SYSTEM) {
        // System metrics schema
        std::vector<common::arrow::ColumnSpec> schema = {
            {"host_hash", ColumnType::STRING},
            {"time_range", ColumnType::INT64},
            {"sys_cpu_iowait_pct", ColumnType::DOUBLE},
            {"sys_cpu_user_pct", ColumnType::DOUBLE},
            {"sys_cpu_system_pct", ColumnType::DOUBLE},
            {"sys_cpu_idle_pct", ColumnType::DOUBLE},
            {"sys_core_iowait_pct_max", ColumnType::DOUBLE},
            {"sys_core_iowait_pct_p95", ColumnType::DOUBLE},
            {"sys_mem_dirty", ColumnType::DOUBLE},
            {"sys_mem_cached", ColumnType::DOUBLE},
            {"sys_mem_available", ColumnType::DOUBLE},
        };
        builder.declare_schema(schema);
        builder.reserve(entries.size());

        for (const auto& entry : entries) {
            const auto& key = entry.key;
            const auto& metrics = entry.metrics;
            std::size_t ci = 0;

            builder.append_string(ci++, key.hhash());
            auto time_range =
                bucket_width_us > 0
                    ? static_cast<std::int64_t>(
                          (key.time_bucket - ctx.time_origin) / bucket_width_us)
                    : 0;
            builder.append_int64(ci++, time_range);

            // Extract system metrics from custom_metrics
            auto get_metric = [&](const char* name) -> double {
                if (!metrics.custom_metrics) return 0.0;
                auto it = metrics.custom_metrics->find(name);
                if (it == metrics.custom_metrics->end()) return 0.0;
                return it->second.mean;
            };
            auto get_metric_max = [&](const char* name) -> double {
                if (!metrics.custom_metrics) return 0.0;
                auto it = metrics.custom_metrics->find(name);
                if (it == metrics.custom_metrics->end()) return 0.0;
                return static_cast<double>(it->second.max);
            };

            builder.append_double(ci++, get_metric("iowait_pct"));
            builder.append_double(ci++, get_metric("user_pct"));
            builder.append_double(ci++, get_metric("system_pct"));
            builder.append_double(ci++, get_metric("idle_pct"));
            builder.append_double(ci++, get_metric_max("iowait_pct"));
            builder.append_double(ci++,
                                  get_metric("iowait_pct"));  // p95 approx
            builder.append_double(ci++, get_metric("Dirty"));
            builder.append_double(ci++, get_metric("Cached"));
            builder.append_double(ci++, get_metric("MemAvailable"));

            builder.end_row();
        }
    } else {
        // Events/Profiles schema
        std::vector<common::arrow::ColumnSpec> schema = {
            {"cat", ColumnType::STRING},
            {"func_name", ColumnType::STRING},
            {"pid", ColumnType::INT64},
            {"tid", ColumnType::INT64},
            {"file_hash", ColumnType::STRING},
            {"host_hash", ColumnType::STRING},
            {"file_name", ColumnType::STRING},
            {"host_name", ColumnType::STRING},
            {"proc_name", ColumnType::STRING},
            {"io_cat", ColumnType::INT64},
            {"acc_pat", ColumnType::INT64},
            {"count", ColumnType::INT64},
            {"time", ColumnType::DOUBLE},
            {"size", ColumnType::INT64},
            {"time_min", ColumnType::DOUBLE},
            {"time_max", ColumnType::DOUBLE},
            {"size_min", ColumnType::INT64},
            {"size_max", ColumnType::INT64},
            {"time_range", ColumnType::INT64},
            {"time_start", ColumnType::INT64},
            {"time_end", ColumnType::INT64},
        };
        builder.declare_schema(schema);
        builder.reserve(entries.size());

        for (const auto& entry : entries) {
            const auto& key = entry.key;
            const auto& metrics = entry.metrics;
            std::size_t ci = 0;

            auto fhash = key.fhash();
            auto hhash = key.hhash();
            auto file_name = resolve_hash(ctx.file_hashes, fhash);
            auto host_name = resolve_hash(ctx.host_hashes, hhash);
            auto proc_name =
                build_proc_name(host_name, hhash, key.pid, key.tid);
            auto io_cat = get_io_category(key.name());

            builder.append_string(ci++, key.cat());
            builder.append_string(ci++, key.name());
            builder.append_int64(ci++, static_cast<std::int64_t>(key.pid));
            builder.append_int64(ci++, static_cast<std::int64_t>(key.tid));
            builder.append_string(ci++, fhash);
            builder.append_string(ci++, hhash);
            builder.append_string(ci++, file_name);
            builder.append_string(ci++, host_name);
            builder.append_string(ci++, proc_name);
            builder.append_int64(ci++, static_cast<std::int64_t>(io_cat));
            builder.append_int64(ci++, 0);  // acc_pat always 0

            builder.append_int64(ci++,
                                 static_cast<std::int64_t>(metrics.count));
            // time: duration in seconds (dur_total is in us)
            builder.append_double(ci++,
                                  static_cast<double>(metrics.duration.total) /
                                      ctx.time_resolution);
            // size: nullable (0 means null)
            if (metrics.size.total > 0) {
                builder.append_int64(
                    ci++, static_cast<std::int64_t>(metrics.size.total));
            } else {
                builder.append_null(ci++);
            }
            // time_min/max in seconds
            builder.append_double(
                ci++, metrics.count > 0
                          ? static_cast<double>(metrics.duration.min) /
                                ctx.time_resolution
                          : 0.0);
            builder.append_double(ci++,
                                  static_cast<double>(metrics.duration.max) /
                                      ctx.time_resolution);
            // size_min/max: nullable
            if (metrics.size.total > 0 && metrics.count > 0) {
                builder.append_int64(
                    ci++, static_cast<std::int64_t>(metrics.size.min));
                builder.append_int64(
                    ci++, static_cast<std::int64_t>(metrics.size.max));
            } else {
                builder.append_null(ci++);
                builder.append_null(ci++);
            }

            // time_range: normalized bucket index
            auto time_range =
                bucket_width_us > 0
                    ? static_cast<std::int64_t>(
                          (key.time_bucket - ctx.time_origin) / bucket_width_us)
                    : 0;
            builder.append_int64(ci++, time_range);
            // time_start/end: relative to time_origin (still in us)
            builder.append_int64(
                ci++, static_cast<std::int64_t>(metrics.ts - ctx.time_origin));
            builder.append_int64(
                ci++, static_cast<std::int64_t>(metrics.te - ctx.time_origin));

            builder.end_row();
        }
    }

    return builder.finish();
}
#endif  // DFTRACER_UTILS_ENABLE_ARROW

// ---------------------------------------------------------------------------
// AggregatorUtility::process - parallel, RocksDB-backed, fused pipeline
// ---------------------------------------------------------------------------

coro::AsyncGenerator<AggregationBatch> AggregatorUtility::process(
    const AggregatorInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("aggregate");
    CoroScope& scope = context();

    // Determine parallelism
    std::size_t parallelism = input.parallelism;
    if (parallelism == 0) {
        parallelism = dftracer_utils_hardware_concurrency();
    }

    // Resolve files and index path with aggregation cache check
    bool force_rebuild = input.force_rebuild;
    auto resolve = [&](bool force) -> coro::CoroTask<indexing::ResolverResult> {
        indexing::IndexResolverUtility resolver;
        indexing::ResolverInput resolver_input;
        resolver_input.directory = input.directory;
        resolver_input.index_dir = input.index_dir;
        resolver_input.require_aggregation = !force;
        resolver_input.aggregation_config = input.config;
        co_return co_await scope.spawn(resolver, resolver_input);
    };

    auto scan_result = co_await resolve(force_rebuild);

    if (scan_result.all_files.empty()) {
        DFTRACER_UTILS_LOG_WARN("No .pfw or .pfw.gz files found in: %s",
                                input.directory.c_str());
        co_return;
    }

    // A changed source pollutes the merged aggregation, which cannot be
    // partially invalidated. Force a full rebuild: wipe the store and
    // re-resolve so every file is re-indexed and re-aggregated from scratch.
    if (scan_result.stale_detected && !force_rebuild) {
        DFTRACER_UTILS_LOG_WARN(
            "Stale index detected; forcing a full rebuild of %s",
            scan_result.index_path.c_str());
        if (fs::exists(scan_result.index_path)) {
            fs::remove_all(scan_result.index_path);
        }
        force_rebuild = true;
        scan_result = co_await resolve(force_rebuild);
    }

    DFTRACER_UTILS_LOG_INFO(
        "Found %zu files (%zu need checkpoint, %zu need aggregation, %zu "
        "cached)",
        scan_result.all_files.size(), scan_result.needs_checkpoint.size(),
        scan_result.needs_aggregation.size(), scan_result.cached.size());

    const auto& shared_index_path = scan_result.index_path;

    // Force rebuild: clear existing index
    if (force_rebuild && fs::exists(shared_index_path)) {
        DFTRACER_UTILS_LOG_INFO("Clearing shared index store: %s",
                                shared_index_path.c_str());
        fs::remove_all(shared_index_path);
    }

    // Open RocksDB-backed aggregator with merge operator
    auto agg_db = EventAggregator::open_with_merge_operator(shared_index_path);
    auto merger = std::make_unique<EventAggregator>(agg_db, 0);

    // Build list of files needing work (checkpoint or aggregation)
    std::vector<std::string> files_needing_work;
    files_needing_work.reserve(scan_result.needs_checkpoint.size() +
                               scan_result.needs_aggregation.size());
    for (auto& item : scan_result.needs_checkpoint) {
        files_needing_work.push_back(std::move(item.file_path));
    }
    for (auto& item : scan_result.needs_aggregation) {
        files_needing_work.push_back(std::move(item.file_path));
    }

    // Index and aggregate in parallel using fused pipeline
    if (!files_needing_work.empty()) {
        auto agg_config_ptr = std::make_shared<AggregationConfig>(input.config);

        auto batch_config = std::make_shared<indexer::IndexBuildBatchConfig>();
        batch_config->file_paths = std::move(files_needing_work);
        batch_config->index_dir = input.index_dir;
        batch_config->checkpoint_size = input.checkpoint_size;
        batch_config->parallelism = parallelism;
        batch_config->force_rebuild = false;  // Already handled above
        batch_config->use_batch_write = true;

        // Attach AggregationVisitor to each file during parsing
        batch_config->dft_visitor_factory =
            [agg_db, agg_config_ptr](const std::string& file_path)
            -> std::vector<std::unique_ptr<composites::dft::DftEventVisitor>> {
            std::vector<std::unique_ptr<composites::dft::DftEventVisitor>>
                visitors;
            visitors.push_back(std::make_unique<AggregationVisitor>(
                agg_db, 0, *agg_config_ptr, file_path));
            return visitors;
        };

        auto batch_result = co_await indexer::IndexBatchBuilderUtility::process(
            &scope, std::move(batch_config));

        // Drain visitors and merge results
        std::vector<std::string> processed_files = merge_aggregation_visitors(
            batch_result.extra_visitors, merger.get());

        // Write global config and per-file markers for cache detection
        if (!processed_files.empty()) {
            write_aggregation_tracking(agg_db.get(), input.config,
                                       processed_files, shared_index_path,
                                       input.config.compute_hash());
        }
    }

    // Get observed columns for consistent Arrow schema
    auto obs = merger->observed_columns();
    auto global_extra_key_ids =
        std::make_shared<std::vector<std::uint32_t>>(obs.extra_key_ids);
    auto global_custom_metric_names =
        std::make_shared<std::vector<std::string>>(obs.custom_metric_names);

    // Stable, deterministic schema ordering
    std::sort(global_extra_key_ids->begin(), global_extra_key_ids->end());
    std::sort(global_custom_metric_names->begin(),
              global_custom_metric_names->end());

    // Yield batches by scanning the merged aggregator
    const std::size_t batch_sz = input.event_batch_size;

    const std::size_t total_events = merger->total_events();
    const std::size_t total_files = merger->total_files();

    auto make_batch = [&](AggregationBatchType type) {
        AggregationBatch b;
        b.batch_type = type;
        b.total_events_processed = total_events;
        b.total_files_processed = total_files;
        b.global_extra_key_ids = global_extra_key_ids.get();
        b.global_custom_metric_names = global_custom_metric_names.get();
        return b;
    };

    // Collect entries grouped by type (scan callback is synchronous)
    std::vector<AggregationEntry> event_entries;
    std::vector<AggregationEntry> profile_entries;
    std::vector<AggregationEntry> system_entries;

    std::size_t total_keys = 0;
    merger->scan([&](AggMapType map_type, const AggregationKey& key,
                     AggregationMetrics& metrics) {
        total_keys++;
        switch (map_type) {
            case AggMapType::EVENT:
                event_entries.emplace_back(key, std::move(metrics));
                break;
            case AggMapType::PROFILE:
                profile_entries.emplace_back(key, std::move(metrics));
                break;
            case AggMapType::SYSTEM:
                system_entries.emplace_back(key, std::move(metrics));
                break;
        }
        return true;
    });

    // Setup augmentation if needed
    std::optional<AugmentationConfig> aug_config;
    if (scan_result.needs_augmentation) {
        aug_config = AugmentationConfig{scan_result.stored_time_interval_us,
                                        input.config.time_interval_us};
        DFTRACER_UTILS_LOG_INFO(
            "Augmenting time interval: %" PRIu64 " us -> %" PRIu64 " us",
            scan_result.stored_time_interval_us, input.config.time_interval_us);
    }

    auto yield_batch = [&](AggregationBatch batch) -> AggregationBatch {
        if (aug_config) {
            return augment_batch(batch, *aug_config);
        }
        return batch;
    };

    // Yield event batches
    for (std::size_t i = 0; i < event_entries.size(); i += batch_sz) {
        AggregationBatch batch = make_batch(AggregationBatchType::EVENT);
        std::size_t end = std::min(i + batch_sz, event_entries.size());
        for (std::size_t j = i; j < end; ++j) {
            batch.entries.push_back(std::move(event_entries[j]));
        }
        co_yield yield_batch(std::move(batch));
    }

    // Yield profile batches
    for (std::size_t i = 0; i < profile_entries.size(); i += batch_sz) {
        AggregationBatch batch = make_batch(AggregationBatchType::PROFILE);
        std::size_t end = std::min(i + batch_sz, profile_entries.size());
        for (std::size_t j = i; j < end; ++j) {
            batch.entries.push_back(std::move(profile_entries[j]));
        }
        co_yield yield_batch(std::move(batch));
    }

    // Yield system batches
    for (std::size_t i = 0; i < system_entries.size(); i += batch_sz) {
        AggregationBatch batch = make_batch(AggregationBatchType::SYSTEM);
        std::size_t end = std::min(i + batch_sz, system_entries.size());
        for (std::size_t j = i; j < end; ++j) {
            batch.entries.push_back(std::move(system_entries[j]));
        }
        co_yield yield_batch(std::move(batch));
    }

    DFTRACER_UTILS_LOG_INFO("Aggregation complete: %zu keys", total_keys);
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
