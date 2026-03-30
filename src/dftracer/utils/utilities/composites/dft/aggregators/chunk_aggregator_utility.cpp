#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/utilities/common/json/json.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/chunk_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/event.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <yyjson.h>

#include <chrono>
#include <cstring>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft::aggregators {

using dftracer::utils::utilities::composites::dft::DFTracerEvent;

std::uint64_t ChunkAggregatorUtility::compute_time_bucket(
    std::uint64_t timestamp, std::uint64_t duration,
    const AggregationConfig& config) const {
    std::uint64_t midpoint = timestamp + (duration / 2);

    if (config.use_relative_time) {
        midpoint -= config.reference_timestamp;
    }
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

    metrics.update_duration(ev.dur, config.compute_percentiles);
    metrics.update_timestamp(ev.ts, ev.dur);

    JsonValue ret = ev.args["ret"];
    if (ret.exists() && internal::is_data_transfer_op(key.cat(), key.name())) {
        std::uint64_t size = ret.get<std::uint64_t>();
        metrics.update_size(size, config.compute_percentiles);
    }

    if (!config.custom_metric_fields.empty()) {
        for (const auto& field : config.custom_metric_fields) {
            JsonValue field_val = ev.args[field];
            if (field_val.exists()) {
                std::uint64_t value = field_val.get<std::uint64_t>();
                metrics.update_custom_metric(field, value,
                                             config.compute_percentiles);
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

    auto reader_input =
        utilities::composites::IndexedReadInput::from_file(input.file_path)
            .with_checkpoint_size(input.checkpoint_size)
            .with_index(input.idx_path);

    utilities::composites::IndexedFileReaderUtility reader_utility;
    auto reader = co_await reader_utility.process(reader_input);

    if (!reader) {
        DFTRACER_UTILS_LOG_ERROR("Chunk %d: Failed to create reader for %s",
                                 input.chunk_index, input.file_path.c_str());
        co_return output;
    }

    auto stream = reader->stream(
        utilities::reader::internal::StreamConfig()
            .stream_type(
                utilities::reader::internal::StreamType::MULTI_LINES_BYTES)
            .range_type(utilities::reader::internal::RangeType::BYTE_RANGE)
            .buffer_size(input.batch_size)
            .from(input.start_byte)
            .to(input.end_byte));

    if (!stream) {
        DFTRACER_UTILS_LOG_ERROR("Chunk %d: Failed to create stream for %s",
                                 input.chunk_index, input.file_path.c_str());
        co_return output;
    }

    AggregationMap local_aggregations;

    std::shared_ptr<AssociationTracker> local_tracker;
    if (input.config.track_process_parents ||
        !input.config.boundary_events.empty()) {
        local_tracker = std::make_shared<AssociationTracker>();
    }

    char yy_buf[common::json::YYJSON_LINE_POOL_SIZE];
    yyjson_alc yy_alc;
    yyjson_alc_pool_init(&yy_alc, yy_buf, sizeof(yy_buf));

    while (!stream->done()) {
        auto chunk = co_await stream->read_async();

        if (chunk.empty()) {
            break;
        }

        std::size_t bytes_read = chunk.size();
        const char* data = chunk.data();
        std::size_t pos = 0;

        while (pos < bytes_read) {
            const char* line_start = data + pos;
            const char* newline = static_cast<const char*>(
                memchr(line_start, '\n', bytes_read - pos));

            if (!newline) {
                break;
            }

            std::size_t line_len = newline - line_start;

            if (line_len > 0) {
                yyjson_read_flag flg = YYJSON_READ_NOFLAG;
                yyjson_doc* doc =
                    yyjson_read_opts(const_cast<char*>(line_start), line_len,
                                     flg, &yy_alc, nullptr);

                if (doc) {
                    yyjson_val* root = yyjson_doc_get_root(doc);
                    if (root && yyjson_is_obj(root)) {
                        bool pass = true;
                        if (input.query) {
                            JsonValue json(root);
                            std::string_view ph =
                                json["ph"].get<std::string_view>();
                            if (ph != "M") {
                                pass = input.query->evaluate(json);
                            }
                        }
                        if (pass) {
                            DFTracerEvent ev;
                            if (DFTracerEvent::parse(root, ev) &&
                                !ev.is_metadata()) {
                                if (local_tracker) {
                                    JsonValue json(root);
                                    local_tracker->extract_from_event(
                                        json, ev.args, input.config);
                                }
                                auto key = build_key(ev, input.config);
                                update_entry(ev, input.config,
                                             local_aggregations, key);
                                output.events_processed++;
                            }
                        }
                    }
                    yyjson_doc_free(doc);
                }
            }

            pos = (newline - data) + 1;
        }
    }

    if (local_tracker) {
        local_tracker->finalize();
        output.local_tracker = std::move(local_tracker);
    }
    output.aggregations = std::move(local_aggregations);
    output.success = true;

    co_return output;
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
