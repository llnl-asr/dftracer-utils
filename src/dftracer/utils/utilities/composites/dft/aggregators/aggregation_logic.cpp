#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_logic.h>
#include <dftracer/utils/utilities/composites/dft/args_map.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>

namespace dftracer::utils::utilities::composites::dft::aggregators {

namespace {

void apply_preaggregated_metric(MetricStats& stats, std::uint64_t ev_count,
                                const ArgsValueProxy& sum_val,
                                const ArgsValueProxy& min_val,
                                const ArgsValueProxy& max_val) {
    if (!sum_val.exists()) return;

    const auto total = sum_val.get<std::uint64_t>();
    stats.count += ev_count;
    stats.total += total;
    if (min_val.exists()) {
        stats.min = std::min(stats.min, min_val.get<std::uint64_t>());
    }
    if (max_val.exists()) {
        stats.max = std::max(stats.max, max_val.get<std::uint64_t>());
    }

    if (stats.count > 0) {
        stats.mean =
            static_cast<double>(stats.total) / static_cast<double>(stats.count);
        stats.m2 = 0.0;
    }
}

}  // namespace

std::uint64_t compute_time_bucket(std::uint64_t timestamp,
                                  std::uint64_t duration,
                                  const AggregationConfig& config) {
    std::uint64_t midpoint = timestamp + (duration / 2);

    if (config.use_relative_time) {
        midpoint -= config.reference_timestamp;
    }
    if (config.time_interval_us == 0) return midpoint;
    return (midpoint / config.time_interval_us) * config.time_interval_us;
}

AggregationKey build_aggregation_key(const DFTracerEvent& ev,
                                     const AggregationConfig& config) {
    auto& intern = aggregation_intern();

    AggregationKey key;
    key.cat_id = intern.get_or_insert(ev.cat);
    key.name_id = intern.get_or_insert(ev.name);
    key.pid = ev.pid;
    key.tid = ev.tid;

    auto hhash_sv = ev.args["hhash"].get<std::string_view>();
    if (!hhash_sv.empty()) {
        key.hhash_id = intern.get_or_insert(hhash_sv);
    }
    auto fhash_sv = ev.args["fhash"].get<std::string_view>();
    if (!fhash_sv.empty()) {
        key.fhash_id = intern.get_or_insert(fhash_sv);
    }

    key.time_bucket = compute_time_bucket(ev.ts, ev.dur, config);

    if (!config.extra_group_keys.empty()) {
        key.extra_keys = std::make_unique<
            std::vector<std::pair<std::uint32_t, std::uint32_t>>>();
        for (const auto& extra_key : config.extra_group_keys) {
            std::string_view value = ev.args[extra_key].get<std::string_view>();
            if (!value.empty()) {
                key.extra_keys->emplace_back(intern.get_or_insert(extra_key),
                                             intern.get_or_insert(value));
            }
        }
    }

    return key;
}

void update_aggregation_entry(const DFTracerEvent& ev,
                              const AggregationConfig& config,
                              AggregationMap& aggregations,
                              const AggregationKey& key) {
    auto it = aggregations.find(key);
    if (it == aggregations.end()) {
        it = aggregations
                 .emplace(key, AggregationMetrics(config.sketch_accuracy))
                 .first;
    }
    auto& metrics = it->second;

    std::uint64_t ev_count = 0;

    if (ev.is_counter()) {
        auto a_count = ev.args["dft_cnt"];
        if (!a_count.exists()) a_count = ev.args["count"];
        ev_count = a_count.exists() ? a_count.get<std::uint64_t>() : 1;
        metrics.count += ev_count;

        auto a_dur = ev.args["dur_sum"];
        if (!a_dur.exists()) a_dur = ev.args["dur"];
        auto a_dur_min = ev.args["dur_min"];
        if (!a_dur_min.exists()) a_dur_min = ev.args["dur"];
        auto a_dur_max = ev.args["dur_max"];
        if (!a_dur_max.exists()) a_dur_max = ev.args["dur"];
        apply_preaggregated_metric(metrics.duration, ev_count, a_dur, a_dur_min,
                                   a_dur_max);

        auto a_size_sum = ev.args["ret_sum"];
        if (!a_size_sum.exists()) a_size_sum = ev.args["ret"];
        auto a_size_min = ev.args["ret_min"];
        if (!a_size_min.exists()) a_size_min = ev.args["ret"];
        auto a_size_max = ev.args["ret_max"];
        if (!a_size_max.exists()) a_size_max = ev.args["ret"];
        apply_preaggregated_metric(metrics.size, ev_count, a_size_sum,
                                   a_size_min, a_size_max);

        metrics.update_timestamp(ev.ts, config.time_interval_us);
    } else {
        metrics.update_duration(ev.dur, config.compute_percentiles);
        metrics.update_timestamp(ev.ts, ev.dur);

        auto ret = ev.args["ret"];
        if (ret.exists() &&
            internal::is_data_transfer_op(key.cat(), key.name())) {
            std::uint64_t size = ret.get<std::uint64_t>();
            metrics.update_size(size, config.compute_percentiles);
        }
    }

    auto track_metric_field = [&](std::string_view field) {
        if (ev.is_counter()) {
            std::string sum_key = std::string(field) + "_sum";
            auto a_sum = ev.args[sum_key];
            if (!a_sum.exists()) a_sum = ev.args[field];
            std::string min_key = std::string(field) + "_min";
            auto a_min = ev.args[min_key];
            if (!a_min.exists()) a_min = ev.args[field];
            std::string max_key = std::string(field) + "_max";
            auto a_max = ev.args[max_key];
            if (!a_max.exists()) a_max = ev.args[field];
            if (a_sum.exists() || a_min.exists() || a_max.exists()) {
                if (!metrics.custom_metrics) {
                    metrics.custom_metrics =
                        std::make_unique<CustomMetricsMap>();
                }
                auto& cm = *metrics.custom_metrics;
                auto cm_it = cm.find(field);
                if (cm_it == cm.end()) {
                    cm_it = cm.emplace(std::string(field),
                                       MetricStats(metrics.sketch_accuracy))
                                .first;
                }
                apply_preaggregated_metric(cm_it->second, ev_count, a_sum,
                                           a_min, a_max);
            }
        } else {
            auto field_val = ev.args[field];
            if (field_val.exists()) {
                std::uint64_t value = field_val.get<std::uint64_t>();
                metrics.update_custom_metric(field, value,
                                             config.compute_percentiles);
            }
        }
    };

    for (const auto& field : config.custom_metric_fields) {
        track_metric_field(field);
    }

    if (config.track_default_args) {
        auto is_reserved = [](std::string_view k) {
            return k == "hhash" || k == "fhash" || k == "dft_cnt" ||
                   k == "dur" || k == "dur_sum" || k == "dur_min" ||
                   k == "dur_max" || k == "ret" || k == "ret_sum" ||
                   k == "ret_min" || k == "ret_max";
        };

        auto is_preagg_suffix = [](std::string_view k) {
            return k.size() > 4 && (k.substr(k.size() - 4) == "_sum" ||
                                    k.substr(k.size() - 4) == "_min" ||
                                    k.substr(k.size() - 4) == "_max");
        };

        auto is_extra_group_key = [&](std::string_view k) {
            for (const auto& gk : config.extra_group_keys) {
                if (gk == k) return true;
            }
            return false;
        };

        auto is_custom_field = [&](std::string_view k) {
            for (const auto& cf : config.custom_metric_fields) {
                if (cf == k) return true;
            }
            return false;
        };

        ev.args.for_each_member([&](std::string_view k, ArgsValueProxy v) {
            if (is_reserved(k) || is_extra_group_key(k) || is_custom_field(k))
                return;
            if (ev.is_counter() && is_preagg_suffix(k)) return;
            if (!v.is_number()) return;
            track_metric_field(k);
        });
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
