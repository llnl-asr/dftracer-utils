#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/sqlite/async.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/chunk_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_query_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <yyjson.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <string_view>
#include <unordered_set>

namespace dftracer::utils::utilities::composites::dft::aggregators {

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
    const JsonValue& json, const JsonValue& args, std::uint64_t timestamp,
    std::uint64_t duration, const AggregationConfig& config,
    const std::shared_ptr<AssociationTracker>& /*local_tracker*/) const {
    AggregationKey key;

    key.cat = json["cat"].get<std::string_view>();
    key.name = json["name"].get<std::string_view>();
    key.pid = json["pid"].get<std::uint64_t>();
    key.tid = json["tid"].get<std::uint64_t>();

    key.hhash = args["hhash"].get<std::string_view>();
    key.fhash = args["fhash"].get<std::string_view>();

    key.time_bucket = compute_time_bucket(timestamp, duration, config);

    if (!config.extra_group_keys.empty()) {
        for (const auto& extra_key : config.extra_group_keys) {
            std::string_view value = args[extra_key].get<std::string_view>();
            if (!value.empty()) {
                key.extra_keys[extra_key] = value;
            }
        }
    }

    return key;
}

void ChunkAggregatorUtility::process_event(
    yyjson_val* event, const AggregationConfig& config,
    std::unordered_map<AggregationKey, AggregationMetrics, AggregationKeyHash>&
        local_aggregations,
    const std::shared_ptr<AssociationTracker>& local_tracker) {
    JsonValue json(event);

    std::string_view ph = json["ph"].get<std::string_view>();
    if (ph == "M") {
        return;
    }

    std::uint64_t timestamp = json["ts"].get<std::uint64_t>();
    JsonValue args = json["args"];

    if (local_tracker) {
        local_tracker->extract_from_event(json, args, config);
    }

    if (!config.include_categories.empty()) {
        std::string_view cat = json["cat"].get<std::string_view>();
        if (std::find(config.include_categories.begin(),
                      config.include_categories.end(),
                      cat) == config.include_categories.end()) {
            return;
        }
    }

    if (!config.include_names.empty()) {
        std::string_view name = json["name"].get<std::string_view>();
        if (std::find(config.include_names.begin(), config.include_names.end(),
                      name) == config.include_names.end()) {
            return;
        }
    }

    std::uint64_t duration = json["dur"].get<std::uint64_t>();

    AggregationKey key =
        build_key(json, args, timestamp, duration, config, local_tracker);

    auto it = local_aggregations.find(key);
    if (it == local_aggregations.end()) {
        it = local_aggregations
                 .emplace(key, AggregationMetrics(config.sketch_accuracy))
                 .first;
    }
    auto& metrics = it->second;

    metrics.update_duration(duration, config.compute_percentiles);
    metrics.update_timestamp(timestamp, duration);

    JsonValue ret = args["ret"];
    if (ret.exists()) {
        std::uint64_t size = ret.get<std::uint64_t>();
        metrics.update_size(size, config.compute_percentiles);
    }

    if (!config.custom_metric_fields.empty()) {
        for (const auto& field : config.custom_metric_fields) {
            JsonValue field_val = args[field];
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

    // --- Bloom Filter Chunk Skipping ---
    if (!input.bloom_predicates.empty() && !input.bidx_path.empty()) {
        using namespace dftracer::utils::utilities::composites::dft::indexing;

        auto bloom_check = [&input]() -> bool {
            try {
                BloomIndexDatabase bidx(input.bidx_path);
                int file_info_id = bidx.get_file_info_id(input.file_path);
                if (file_info_id < 0) return false;

                auto indexed_dims =
                    queries::query_index_dimensions(bidx.db(), file_info_id);
                std::unordered_set<std::string> indexed_set(
                    indexed_dims.begin(), indexed_dims.end());

                std::unordered_map<std::string, std::vector<std::string>>
                    effective_predicates;
                for (const auto& [dimension, values] : input.bloom_predicates) {
                    if (indexed_set.find(dimension) != indexed_set.end()) {
                        effective_predicates[dimension] = values;
                    }
                }

                if (effective_predicates.empty()) return false;

                std::size_t checkpoint_size = input.checkpoint_size;
                if (checkpoint_size == 0) {
                    checkpoint_size = 4 * 1024 * 1024;
                }
                std::uint64_t start_ckpt = input.start_byte / checkpoint_size;
                std::uint64_t end_ckpt =
                    (input.end_byte > input.start_byte)
                        ? (input.end_byte - 1) / checkpoint_size
                        : start_ckpt;

                bool chunk_may_match = false;
                for (const auto& [dimension, values] : effective_predicates) {
                    auto chunk_blooms = queries::query_chunk_bloom_filters(
                        bidx.db(), file_info_id, dimension);

                    for (const auto& cb : chunk_blooms) {
                        if (cb.checkpoint_idx < start_ckpt ||
                            cb.checkpoint_idx > end_ckpt) {
                            continue;
                        }
                        auto bloom = BloomFilter::from_blob(
                            cb.bloom_data.data(), cb.bloom_data.size());
                        for (const auto& val : values) {
                            if (bloom.possibly_contains(val)) {
                                chunk_may_match = true;
                                break;
                            }
                        }
                        if (chunk_may_match) break;
                    }
                    if (chunk_may_match) break;
                }

                return !chunk_may_match;  // true = skip
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_WARN(
                    "Chunk %d: bloom index error: %s, "
                    "processing chunk normally",
                    input.chunk_index, e.what());
                return false;  // don't skip on error
            }
        };

        bool should_skip = co_await sqlite::run(bloom_check);

        if (should_skip) {
            DFTRACER_UTILS_LOG_INFO(
                "Skipping chunk %d: no bloom filter match "
                "for predicates",
                input.chunk_index);
            output.success = true;
            output.aggregations.clear();
            co_return output;
        }
    }
    // --- End Bloom Filter Chunk Skipping ---

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

    std::unordered_map<AggregationKey, AggregationMetrics, AggregationKeyHash>
        local_aggregations;
    local_aggregations.reserve(10000);

    std::shared_ptr<AssociationTracker> local_tracker;
    if (input.config.track_process_parents ||
        !input.config.boundary_events.empty()) {
        local_tracker = std::make_shared<AssociationTracker>();
    }

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
                                     flg, nullptr, nullptr);

                if (doc) {
                    yyjson_val* root = yyjson_doc_get_root(doc);
                    if (root && yyjson_is_obj(root)) {
                        process_event(root, input.config, local_aggregations,
                                      local_tracker);
                        output.events_processed++;
                    }
                    yyjson_doc_free(doc);
                }
            }

            pos = (newline - data) + 1;
        }
    }

    output.aggregations = std::move(local_aggregations);
    if (local_tracker) {
        local_tracker->finalize();
        output.local_tracker = std::move(local_tracker);
    }
    output.success = true;

    co_return output;
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
