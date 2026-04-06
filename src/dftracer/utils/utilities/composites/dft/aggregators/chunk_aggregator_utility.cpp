#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/utilities/common/json/json.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/chunk_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/event.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <yyjson.h>

#include <chrono>
#include <cstring>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft::aggregators {

using dftracer::utils::utilities::composites::dft::DFTracerEvent;

namespace {

void apply_preaggregated_metric(MetricStats& stats, std::uint64_t count,
                                const JsonValue& sum_val,
                                const JsonValue& min_val,
                                const JsonValue& max_val) {
    if (!sum_val.exists()) return;

    const auto total = sum_val.get<std::uint64_t>();
    stats.total += total;
    if (min_val.exists()) {
        stats.min = std::min(stats.min, min_val.get<std::uint64_t>());
    }
    if (max_val.exists()) {
        stats.max = std::max(stats.max, max_val.get<std::uint64_t>());
    }

    if (count > 0) {
        stats.mean =
            static_cast<double>(stats.total) / static_cast<double>(count);
        // Counter rows carry pre-aggregated totals/min/max. Higher moments
        // are not available from the source trace, so stddev/skew/kurtosis
        // remain 0 unless enough information is accumulated elsewhere.
        stats.m2 = 0.0;
        stats.m3 = 0.0;
        stats.m4 = 0.0;
    }
}

}  // namespace

std::uint64_t ChunkAggregatorUtility::compute_time_bucket(
    std::uint64_t timestamp, std::uint64_t duration,
    const AggregationConfig& config) const {
    std::uint64_t midpoint = timestamp + (duration / 2);

    if (config.use_relative_time) {
        midpoint -= config.reference_timestamp;
    }
    if (config.time_interval_us == 0) return midpoint;
    return (midpoint / config.time_interval_us) * config.time_interval_us;
}

AggregationKey ChunkAggregatorUtility::build_key(
    const DFTracerEvent& ev, const AggregationConfig& config) const {
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

void ChunkAggregatorUtility::update_entry(const DFTracerEvent& ev,
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

    if (ev.is_counter()) {
        // Profile/system events carry pre-aggregated data in args.
        // Use args.count for the event count, args.dur_sum for total duration,
        // etc.
        JsonValue a_count = ev.args["dft_cnt"];
        if (!a_count.exists()) a_count = ev.args["count"];
        std::uint64_t ev_count =
            a_count.exists() ? a_count.get<std::uint64_t>() : 1;
        metrics.count += ev_count;

        JsonValue a_dur = ev.args["dur_sum"];
        if (!a_dur.exists()) a_dur = ev.args["dur"];
        JsonValue a_dur_min = ev.args["dur_min"];
        if (!a_dur_min.exists()) a_dur_min = ev.args["dur"];
        JsonValue a_dur_max = ev.args["dur_max"];
        if (!a_dur_max.exists()) a_dur_max = ev.args["dur"];
        apply_preaggregated_metric(metrics.duration, metrics.count, a_dur,
                                   a_dur_min, a_dur_max);

        JsonValue a_size_sum = ev.args["ret_sum"];
        if (!a_size_sum.exists()) a_size_sum = ev.args["ret"];
        JsonValue a_size_min = ev.args["ret_min"];
        if (!a_size_min.exists()) a_size_min = ev.args["ret"];
        JsonValue a_size_max = ev.args["ret_max"];
        if (!a_size_max.exists()) a_size_max = ev.args["ret"];
        apply_preaggregated_metric(metrics.size, metrics.count, a_size_sum,
                                   a_size_min, a_size_max);

        metrics.update_timestamp(ev.ts, config.time_interval_us);
    } else {
        // Regular events: count += 1, use event's own dur/size.
        metrics.update_duration(ev.dur, config.compute_percentiles);
        metrics.update_timestamp(ev.ts, ev.dur);

        JsonValue ret = ev.args["ret"];
        if (ret.exists() &&
            internal::is_data_transfer_op(key.cat(), key.name())) {
            std::uint64_t size = ret.get<std::uint64_t>();
            metrics.update_size(size, config.compute_percentiles);
        }
    }

    if (!config.custom_metric_fields.empty()) {
        for (const auto& field : config.custom_metric_fields) {
            if (ev.is_counter()) {
                // Profile/system: read pre-aggregated field_sum/min/max
                std::string sum_key = field + "_sum";
                JsonValue a_sum = ev.args[sum_key];
                if (!a_sum.exists()) a_sum = ev.args[field];
                std::string min_key = field + "_min";
                JsonValue a_min = ev.args[min_key];
                if (!a_min.exists()) a_min = ev.args[field];
                std::string max_key = field + "_max";
                JsonValue a_max = ev.args[max_key];
                if (!a_max.exists()) a_max = ev.args[field];
                if (a_sum.exists() || a_min.exists() || a_max.exists()) {
                    if (!metrics.custom_metrics) {
                        metrics.custom_metrics =
                            std::make_unique<CustomMetricsMap>();
                    }
                    auto& ms = (*metrics.custom_metrics)[field];
                    apply_preaggregated_metric(ms, metrics.count, a_sum, a_min,
                                               a_max);
                }
            } else {
                JsonValue field_val = ev.args[field];
                if (field_val.exists()) {
                    std::uint64_t value = field_val.get<std::uint64_t>();
                    metrics.update_custom_metric(field, value,
                                                 config.compute_percentiles);
                }
            }
        }
    }
}

coro::CoroTask<ChunkAggregationOutput> ChunkAggregatorUtility::process(
    const ChunkAggregatorInput& input) {
    ChunkAggregationOutput output;
    output.chunk_index = input.chunk_index;
    output.events_processed = 0;
    output.bytes_processed = input.end_byte - input.start_byte;
    output.file_path = input.file_path;
    output.success = false;

    if (input.chunk_index % 100 == 0) {
        DFTRACER_UTILS_LOG_INFO("Starting chunk %d: %s [bytes %zu-%zu]",
                                input.chunk_index, input.file_path.c_str(),
                                input.start_byte, input.end_byte);
    }

    using dftracer::utils::utilities::reader::ReadConfig;
    using dftracer::utils::utilities::reader::TraceReader;
    using dftracer::utils::utilities::reader::TraceReaderConfig;

    TraceReaderConfig reader_cfg;
    reader_cfg.file_path = input.file_path;
    if (!input.index_path.empty()) {
        reader_cfg.index_dir =
            input.index_path.substr(0, input.index_path.rfind('/'));
    }
    reader_cfg.checkpoint_size = input.checkpoint_size;
    TraceReader trace_reader(reader_cfg);

    ReadConfig rc;
    rc.start_byte = input.start_byte;
    rc.end_byte = input.end_byte;
    rc.buffer_size = input.batch_size;

    auto line_gen = trace_reader.read_lines(rc);

    AggregationMap local_aggregations;
    AggregationMap local_profiles;
    AggregationMap local_system;

    std::shared_ptr<AssociationTracker> local_tracker;
    if (input.config.track_process_parents ||
        !input.config.boundary_events.empty()) {
        local_tracker = std::make_shared<AssociationTracker>();
    }

    char yy_buf[common::json::YYJSON_LINE_POOL_SIZE];
    yyjson_alc yy_alc;
    yyjson_alc_pool_init(&yy_alc, yy_buf, sizeof(yy_buf));

    while (auto opt = co_await line_gen.next()) {
        const char* line_start = opt->content.data();
        std::size_t line_len = opt->content.size();
        if (line_len == 0) continue;

        yyjson_doc* doc =
            yyjson_read_opts(const_cast<char*>(line_start), line_len,
                             YYJSON_READ_NOFLAG, &yy_alc, nullptr);
        if (!doc) continue;

        yyjson_val* root = yyjson_doc_get_root(doc);
        if (root && yyjson_is_obj(root)) {
            bool pass = true;
            if (input.query) {
                JsonValue json(root);
                std::string_view ph = json["ph"].get<std::string_view>();
                if (ph != "M") {
                    pass = input.query->evaluate(json);
                }
            }
            if (pass) {
                DFTracerEvent ev;
                if (DFTracerEvent::parse(root, ev) && !ev.is_metadata()) {
                    if (local_tracker) {
                        JsonValue json(root);
                        local_tracker->extract_from_event(json, ev.args,
                                                          input.config);
                    }
                    auto key = build_key(ev, input.config);
                    if (ev.is_system()) {
                        update_entry(ev, input.config, local_system, key);
                    } else if (ev.is_profile()) {
                        update_entry(ev, input.config, local_profiles, key);
                    } else {
                        update_entry(ev, input.config, local_aggregations, key);
                    }
                    output.events_processed++;
                }
            }
        }
        yyjson_doc_free(doc);
    }

    if (local_tracker) {
        local_tracker->finalize();
        output.local_tracker = std::move(local_tracker);
    }
    output.aggregations = std::move(local_aggregations);
    output.profile_aggregations = std::move(local_profiles);
    output.system_aggregations = std::move(local_system);
    output.success = true;

    co_return output;
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
