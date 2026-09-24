#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/resolver.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_augmentation.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_drain.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_output.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_runner.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_serialization.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregator.h>
#include <dftracer/utils/index/schemas/dft/agg/event_aggregator.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/trace/internal/utils.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#endif

#include <dftracer/utils/core/common/transparent_string_hash.h>

#include <algorithm>
#include <cinttypes>
#include <set>
#include <unordered_set>

namespace dftracer::utils::index::schemas::dft::agg {

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

AggregatorInput& AggregatorInput::with_event_batch_size(std::size_t sz) {
    event_batch_size = sz;
    return *this;
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW
using utilities::common::arrow::ArrowExportResult;
using utilities::common::arrow::ColumnType;
using utilities::common::arrow::RecordBatchBuilder;

ArrowExportResult AggregationBatch::to_arrow() const {
    RecordBatchBuilder builder;

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

    std::vector<utilities::common::arrow::ColumnSpec> schema = {
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
    if (has_approximated_entries) {
        schema.push_back({"count_ci_lower", ColumnType::DOUBLE});
        schema.push_back({"count_ci_upper", ColumnType::DOUBLE});
    }
    for (auto id : extra_key_ids) {
        schema.push_back(
            {std::string(strings().resolve(id)), ColumnType::STRING});
    }
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
        builder.append_string(ci++, key.cat(strings()));
        builder.append_string(ci++, key.name(strings()));
        builder.append_uint64(ci++, key.pid);
        builder.append_uint64(ci++, key.tid);
        builder.append_string(ci++, key.hhash(strings()));
        char fbuf[::dftracer::utils::hash::HEX64_DIGITS];
        builder.append_string(ci++, key.fhash_str(strings(), fbuf));
        builder.append_uint64(ci++, key.time_bucket);
        builder.append_uint64(ci++, metrics.count);
        builder.append_uint64(
            ci++, static_cast<std::uint64_t>(metrics.duration.total()));
        builder.append_uint64(
            ci++, static_cast<std::uint64_t>(
                      metrics.count > 0 ? metrics.duration.min() : 0));
        builder.append_uint64(
            ci++, static_cast<std::uint64_t>(metrics.duration.max()));
        builder.append_double(ci++, metrics.duration.mean());
        builder.append_double(ci++, metrics.duration.get_stddev());
        builder.append_uint64(ci++,
                              static_cast<std::uint64_t>(metrics.size.total()));
        builder.append_uint64(ci++,
                              static_cast<std::uint64_t>(
                                  metrics.count > 0 ? metrics.size.min() : 0));
        builder.append_uint64(ci++,
                              static_cast<std::uint64_t>(metrics.size.max()));
        builder.append_double(ci++, metrics.size.mean());
        builder.append_double(ci++, metrics.size.get_stddev());
        builder.append_uint64(ci++, metrics.ts);
        builder.append_uint64(ci++, metrics.te);

        for (auto extra_key_id : extra_key_ids) {
            bool found_extra_key = false;
            if (key.extra_keys) {
                for (const auto& [present_id, value_id] : *key.extra_keys) {
                    if (present_id == extra_key_id) {
                        builder.append_string(ci++,
                                              strings().resolve(value_id));
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
                    builder.append_uint64(
                        ci++, static_cast<std::uint64_t>(ms.total()));
                    builder.append_uint64(
                        ci++, static_cast<std::uint64_t>(
                                  metrics.count > 0 ? ms.min() : 0));
                    builder.append_uint64(ci++,
                                          static_cast<std::uint64_t>(ms.max()));
                    builder.append_double(ci++, ms.mean());
                    builder.append_double(ci++, ms.get_stddev());
                    continue;
                }
            }
            for (std::size_t j = 0; j < std::size(cm_schema); ++j)
                builder.append_null(ci++);
        }

        if (has_approximated_entries) {
            builder.append_double(ci++, entry.count_ci.lower);
            builder.append_double(ci++, entry.count_ci.upper);
        }

        builder.end_row();
    }

    return builder.finish();
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW

coro::AsyncGenerator<AggregationBatch> Aggregator::operator()(
    CoroScope& scope, const AggregatorInput& input) const {
    DFTRACER_UTILS_TRACE_SCOPE("aggregate");

    std::size_t parallelism = input.parallelism;
    if (parallelism == 0) {
        parallelism = hardware_concurrency();
    }

    const bool force_rebuild = input.force_rebuild;
    // The config the tier is built with: the requested one, or the stored one
    // when only the interval differs, whose rows are then re-bucketed.
    AggregationConfig config = input.config;
    auto resolve = [&]() -> coro::CoroTask<index::build::ResolverResult> {
        index::build::Resolver resolver;
        index::build::ResolverInput resolver_input;
        resolver_input.directory = input.directory;
        resolver_input.index_dir = input.index_dir;
        resolver_input.require_aggregation = !force_rebuild;
        resolver_input.aggregation_config = config;
        co_return co_await resolver(scope, resolver_input);
    };

    auto scan_result = co_await resolve();
    std::optional<AugmentationConfig> aug_config;
    if (scan_result.needs_augmentation) {
        aug_config = AugmentationConfig{scan_result.stored_time_interval_us,
                                        input.config.time_interval_us};
        config.time_interval_us = scan_result.stored_time_interval_us;
        scan_result = co_await resolve();
    }

    if (scan_result.all_files.empty()) {
        DFTRACER_UTILS_LOG_WARN("No .pfw or .pfw.gz files found in: %s",
                                input.directory.c_str());
        co_return;
    }

    // Only an index in another format is rebuilt whole.
    if (!scan_result.outdated_roots.empty()) {
        for (const auto& root : scan_result.outdated_roots) {
            DFTRACER_UTILS_LOG_WARN("Index %s is outdated; rebuilding",
                                    root.c_str());
            index::store::RocksDBManager::instance().reset(root);
            fs::remove_all(root);
        }
        scan_result = co_await resolve();
    }

    DFTRACER_UTILS_LOG_INFO(
        "Found %zu files (%zu need checkpoint, %zu need aggregation, %zu "
        "cached)",
        scan_result.all_files.size(), scan_result.needs_checkpoint.size(),
        scan_result.needs_aggregation.size(), scan_result.cached.size());

    const auto& shared_index_path = scan_result.index_path;

    auto agg_db = tier::open(shared_index_path,
                             index::store::RocksDatabase::OpenMode::ReadWrite);
    // A forced run rebuilds every file whole, so the tier is stale too.
    if (force_rebuild || !scan_result.stale_aggregation_roots.empty())
        tier::clear(*agg_db);
    tier::write_config(*agg_db, {config.time_interval_us, config.params_hash(),
                                 config.group_by_file});
    auto merger = std::make_unique<EventAggregator>(agg_db);

    std::vector<std::string> files_needing_work;
    if (force_rebuild) {
        files_needing_work = scan_result.all_files;
    } else {
        for (auto& item : scan_result.needs_checkpoint)
            files_needing_work.push_back(std::move(item.file_path));
        for (auto& item : scan_result.needs_aggregation)
            files_needing_work.push_back(std::move(item.file_path));
    }

    if (!files_needing_work.empty()) {
        auto agg_config_ptr = std::make_shared<AggregationConfig>(config);

        auto batch_config =
            std::make_shared<index::build::IndexBuildBatchConfig>();
        batch_config->file_paths = std::move(files_needing_work);
        batch_config->index_dir = input.index_dir;
        batch_config->checkpoint_size = input.checkpoint_size;
        batch_config->parallelism = parallelism;
        batch_config->force_rebuild = force_rebuild;

        batch_config->agg_fold_factory =
            [agg_config_ptr, intern = merger->intern_table()](
                dftracer::utils::StringIntern& build_intern, int file_id)
            -> std::unique_ptr<index::schemas::dft::agg::AggregationFold> {
            return std::make_unique<index::schemas::dft::agg::AggregationFold>(
                build_intern, intern, *agg_config_ptr, file_id);
        };

        auto batch_result = co_await index::build::BatchBuilder::process(
            &scope, std::move(batch_config));

        merge_aggregation_folds(batch_result.agg_outputs, merger.get());
    }

    auto obs = merger->observed_columns();
    auto global_extra_key_ids =
        std::make_shared<std::vector<std::uint32_t>>(obs.extra_key_ids);
    auto global_custom_metric_names =
        std::make_shared<std::vector<std::string>>(obs.custom_metric_names);

    std::sort(global_extra_key_ids->begin(), global_extra_key_ids->end());
    std::sort(global_custom_metric_names->begin(),
              global_custom_metric_names->end());

    const std::size_t batch_sz = input.event_batch_size;

    const std::size_t total_events = merger->total_events();
    const std::size_t total_files = merger->total_files();

    auto make_batch = [&](AggregationBatchType type) {
        AggregationBatch b;
        b.batch_type = type;
        b.intern = merger->intern_table();
        b.total_events_processed = total_events;
        b.total_files_processed = total_files;
        b.global_extra_key_ids = global_extra_key_ids.get();
        b.global_custom_metric_names = global_custom_metric_names.get();
        return b;
    };

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

    if (aug_config) {
        DFTRACER_UTILS_LOG_INFO(
            "Augmenting time interval: %" PRIu64 " us -> %" PRIu64 " us",
            aug_config->source_interval_us, aug_config->target_interval_us);
    }

    auto yield_batch = [&](AggregationBatch batch) -> AggregationBatch {
        if (aug_config) {
            return augment_batch(batch, *aug_config);
        }
        return batch;
    };

    for (std::size_t i = 0; i < event_entries.size(); i += batch_sz) {
        AggregationBatch batch = make_batch(AggregationBatchType::EVENT);
        std::size_t end = std::min(i + batch_sz, event_entries.size());
        for (std::size_t j = i; j < end; ++j) {
            batch.entries.push_back(std::move(event_entries[j]));
        }
        co_yield yield_batch(std::move(batch));
    }

    for (std::size_t i = 0; i < profile_entries.size(); i += batch_sz) {
        AggregationBatch batch = make_batch(AggregationBatchType::PROFILE);
        std::size_t end = std::min(i + batch_sz, profile_entries.size());
        for (std::size_t j = i; j < end; ++j) {
            batch.entries.push_back(std::move(profile_entries[j]));
        }
        co_yield yield_batch(std::move(batch));
    }

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

}  // namespace dftracer::utils::index::schemas::dft::agg
