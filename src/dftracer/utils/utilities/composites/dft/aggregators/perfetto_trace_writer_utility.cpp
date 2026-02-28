#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/perfetto_trace_writer_utility.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>
#include <dftracer/utils/utilities/fileio/streaming_file_writer_utility.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>
#include <fcntl.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace dftracer::utils::utilities::composites::dft::aggregators {

std::uint64_t PerfettoTraceWriterUtility::generate_synthetic_tid(
    const AggregationKey& key) const {
    dftracer::utils::utilities::hash::HasherUtility hasher;
    std::string key_str = key.cat + ":" + key.name + ":" +
                          std::to_string(key.pid) + ":" +
                          std::to_string(key.time_bucket);

    if (!key.fhash.empty()) {
        key_str += ":" + key.fhash;
    }

    for (const auto& [k, v] : key.extra_keys) {
        key_str += ":" + k + "=" + v;
    }

    // CPU-bound hash — .get() intentional
    std::size_t hash = hasher.process(key_str).get().value;
    return 1000000000ULL + (hash % 1000000ULL);
}

void PerfettoTraceWriterUtility::append_json_string(
    std::string& buffer, const std::string& str) const {
    for (char c : str) {
        switch (c) {
            case '"':
                buffer += "\\\"";
                break;
            case '\\':
                buffer += "\\\\";
                break;
            case '\b':
                buffer += "\\b";
                break;
            case '\f':
                buffer += "\\f";
                break;
            case '\n':
                buffer += "\\n";
                break;
            case '\r':
                buffer += "\\r";
                break;
            case '\t':
                buffer += "\\t";
                break;
            default:
                if (c >= 32 && c < 127) {
                    buffer += c;
                } else {
                    char hex[7];
                    std::snprintf(hex, sizeof(hex), "\\u%04x",
                                  (unsigned char)c);
                    buffer += hex;
                }
                break;
        }
    }
}

void PerfettoTraceWriterUtility::append_double(std::string& buffer,
                                               double value) const {
    char temp[64];
    if (std::abs(value - std::round(value)) < 1e-9) {
        std::snprintf(temp, sizeof(temp), "%lld",
                      static_cast<long long>(std::round(value)));
    } else {
        std::snprintf(temp, sizeof(temp), "%.2f", value);
    }
    buffer += temp;
}

void PerfettoTraceWriterUtility::append_metric_stats(
    std::string& buffer, const MetricStats& stats, std::uint64_t count,
    bool compute_statistics, bool compute_percentiles,
    const std::vector<double>& percentiles) const {
    char temp[256];

    std::snprintf(temp, sizeof(temp), "\"sum\":%llu",
                  static_cast<unsigned long long>(stats.total));
    buffer += temp;

    buffer += ",\"avg\":";
    append_double(buffer, stats.mean);

    if (stats.min != std::numeric_limits<std::uint64_t>::max()) {
        std::snprintf(temp, sizeof(temp), ",\"min\":%llu",
                      static_cast<unsigned long long>(stats.min));
        buffer += temp;
    }

    if (stats.max > 0) {
        std::snprintf(temp, sizeof(temp), ",\"max\":%llu",
                      static_cast<unsigned long long>(stats.max));
        buffer += temp;
    }

    if (compute_statistics && count >= 2) {
        buffer += ",\"std\":";
        append_double(buffer, stats.get_stddev(count));
    }
    if (compute_statistics && count >= 3) {
        buffer += ",\"skw\":";
        append_double(buffer, stats.get_skewness(count));
    }
    if (compute_statistics && count >= 4) {
        buffer += ",\"krt\":";
        append_double(buffer, stats.get_kurtosis(count));
    }

    if (compute_percentiles && (!stats.sketch.empty())) {
        for (double p : percentiles) {
            double percentile_value = stats.sketch.quantile(p);
            int p_percent = static_cast<int>(p * 100);
            std::snprintf(temp, sizeof(temp), ",\"p%d\":", p_percent);
            buffer += temp;
            append_double(buffer, percentile_value);
        }
    }
}

void PerfettoTraceWriterUtility::append_event_args(
    std::string& buffer, const AggregationKey& key,
    const AggregationMetrics& metrics, bool compute_statistics,
    bool compute_percentiles, const std::vector<double>& percentiles,
    std::uint64_t real_tid) const {
    char temp[512];

    buffer += "\"hhash\":\"";
    append_json_string(buffer, key.hhash);
    buffer += "\"";

    if (real_tid > 0) {
        std::snprintf(temp, sizeof(temp), ",\"real_tid\":%llu",
                      static_cast<unsigned long long>(real_tid));
        buffer += temp;
    }

    if (!key.fhash.empty()) {
        buffer += ",\"fhash\":\"";
        append_json_string(buffer, key.fhash);
        buffer += "\"";
    }

    for (const auto& [k, v] : key.extra_keys) {
        buffer += ",\"";
        append_json_string(buffer, k);
        buffer += "\":\"";
        append_json_string(buffer, v);
        buffer += "\"";
    }

    std::snprintf(temp, sizeof(temp), ",\"count\":%llu",
                  static_cast<unsigned long long>(metrics.count));
    buffer += temp;

    buffer += ",\"dur\":{";
    append_metric_stats(buffer, metrics.duration, metrics.count,
                        compute_statistics, compute_percentiles, percentiles);
    buffer += "}";

    if (metrics.size.total > 0) {
        buffer += ",\"size\":{";
        append_metric_stats(buffer, metrics.size, metrics.count,
                            compute_statistics, compute_percentiles,
                            percentiles);
        buffer += "}";
    }

    for (const auto& [metric_name, metric_stats] : metrics.custom_metrics) {
        buffer += ",\"";
        append_json_string(buffer, metric_name);
        buffer += "\":{";
        append_metric_stats(buffer, metric_stats, metrics.count,
                            compute_statistics, compute_percentiles,
                            percentiles);
        buffer += "}";
    }

    buffer += ",\"ts\":";
    std::snprintf(temp, sizeof(temp), "%llu",
                  static_cast<unsigned long long>(metrics.ts));
    buffer += temp;
    buffer += ",\"te\":";
    std::snprintf(temp, sizeof(temp), "%llu",
                  static_cast<unsigned long long>(metrics.te));
    buffer += temp;

    for (const auto& [assoc_name, assoc_value] :
         metrics.boundary_associations) {
        buffer += ",\"";
        append_json_string(buffer, assoc_name);
        buffer += "\":\"";
        append_json_string(buffer, assoc_value);
        buffer += "\"";
    }

    if (metrics.parent_pid > 0) {
        std::snprintf(temp, sizeof(temp), ",\"parent_pid\":%llu",
                      static_cast<unsigned long long>(metrics.parent_pid));
        buffer += temp;
    }
}

coro::CoroTask<bool> PerfettoTraceWriterUtility::process(
    const PerfettoTraceWriterInput& input) {
    const auto& aggregations = input.resolver_output.aggregations.aggregations;
    const auto& root_pids = input.resolver_output.root_pids;

    std::string buffer;
    buffer.reserve(1024 * 1024);

    buffer += "[\n";

    if (input.resolver_output.trace_duration > 0 ||
        !input.resolver_output.boundary_ranges.empty()) {
        buffer +=
            "{\"name\":\"trace_metadata\",\"cat\":\"metadata\",\"ph\":"
            "\"M\",\"args\":{";

        char temp[512];
        std::snprintf(temp, sizeof(temp), "\"trace_duration\":%llu",
                      static_cast<unsigned long long>(
                          input.resolver_output.trace_duration));
        buffer += temp;

        if (!input.resolver_output.boundary_ranges.empty()) {
            buffer += ",\"boundary_ranges\":{";
            bool first_boundary = true;

            for (const auto& [boundary_name, value_map] :
                 input.resolver_output.boundary_ranges) {
                if (!first_boundary) {
                    buffer += ",";
                }
                first_boundary = false;

                buffer += "\"";
                append_json_string(buffer, boundary_name);
                buffer += "\":{";

                bool first_value = true;
                for (const auto& [value, time_range] : value_map) {
                    if (!first_value) {
                        buffer += ",";
                    }
                    first_value = false;

                    buffer += "\"";
                    append_json_string(buffer, value);
                    buffer += "\":{";

                    std::snprintf(
                        temp, sizeof(temp), "\"ts\":%llu,\"te\":%llu",
                        static_cast<unsigned long long>(time_range.ts),
                        static_cast<unsigned long long>(time_range.te));
                    buffer += temp;

                    buffer += "}";
                }

                buffer += "}";
            }

            buffer += "}";
        }

        buffer += "}}\n";
    }

    if (!root_pids.empty()) {
        for (std::uint64_t pid : root_pids) {
            buffer +=
                "{\"name\":\"root_process\",\"cat\":\"dftracer\",\"ph\":"
                "\"M\",\"pid\":";
            buffer += std::to_string(pid);
            buffer += ",\"tid\":";
            buffer += std::to_string(pid);
            buffer += ",\"args\":{\"is_root\":\"true\"}}\n";
        }
    }

    for (const auto& [key, metrics] : aggregations) {
        char temp[512];

        if (input.format == PerfettoEventFormat::COUNTER) {
            buffer += "{\"name\":\"";
            append_json_string(buffer, key.name);
            buffer += "\",\"cat\":\"";
            append_json_string(buffer, key.cat);
            std::snprintf(temp, sizeof(temp),
                          "\",\"ts\":%llu,\"ph\":\"C\",\"pid\":%llu,"
                          "\"tid\":%llu,\"args\":{",
                          static_cast<unsigned long long>(key.time_bucket),
                          static_cast<unsigned long long>(key.pid),
                          static_cast<unsigned long long>(key.tid));
            buffer += temp;
            append_event_args(buffer, key, metrics, input.compute_statistics,
                              input.compute_percentiles, input.percentiles);
            buffer += "}}\n";

        } else if (input.format == PerfettoEventFormat::REGULAR) {
            std::uint64_t duration = metrics.te - metrics.ts;

            buffer += "{\"name\":\"";
            append_json_string(buffer, key.name);
            buffer += "\",\"cat\":\"";
            append_json_string(buffer, key.cat);
            std::snprintf(
                temp, sizeof(temp),
                "\",\"ts\":%llu,\"dur\":%llu,\"ph\":\"X\",\"pid\":%llu,"
                "\"tid\":%llu,\"args\":{",
                static_cast<unsigned long long>(metrics.ts),
                static_cast<unsigned long long>(duration),
                static_cast<unsigned long long>(key.pid),
                static_cast<unsigned long long>(key.tid));
            buffer += temp;
            append_event_args(buffer, key, metrics, input.compute_statistics,
                              input.compute_percentiles, input.percentiles);
            buffer += "}}\n";

        } else {
            std::string event_id =
                key.cat + ":" + key.name + ":" + std::to_string(key.pid) + ":" +
                std::to_string(key.tid) + ":" + std::to_string(key.time_bucket);
            if (!key.fhash.empty()) {
                event_id += ":" + key.fhash;
            }
            for (const auto& [k, v] : key.extra_keys) {
                event_id += ":" + k + "=" + v;
            }

            buffer += "{\"name\":\"";
            append_json_string(buffer, key.name);
            buffer += "\",\"cat\":\"";
            append_json_string(buffer, key.cat);
            std::snprintf(temp, sizeof(temp),
                          "\",\"ts\":%llu,\"ph\":\"b\",\"pid\":%llu,"
                          "\"tid\":%llu,\"id\":\"",
                          static_cast<unsigned long long>(metrics.ts),
                          static_cast<unsigned long long>(key.pid),
                          static_cast<unsigned long long>(key.tid));
            buffer += temp;
            append_json_string(buffer, event_id);
            buffer += "\",\"args\":{";
            append_event_args(buffer, key, metrics, input.compute_statistics,
                              input.compute_percentiles, input.percentiles);
            buffer += "}}\n";

            buffer += "{\"name\":\"";
            append_json_string(buffer, key.name);
            buffer += "\",\"cat\":\"";
            append_json_string(buffer, key.cat);
            std::snprintf(temp, sizeof(temp),
                          "\",\"ts\":%llu,\"ph\":\"e\",\"pid\":%llu,"
                          "\"tid\":%llu,\"id\":\"",
                          static_cast<unsigned long long>(metrics.te),
                          static_cast<unsigned long long>(key.pid),
                          static_cast<unsigned long long>(key.tid));
            buffer += temp;
            append_json_string(buffer, event_id);
            buffer += "\"}\n";
        }
    }

    buffer += "]\n";

    try {
        if (input.compress) {
            using namespace dftracer::utils::utilities;

            compression::zlib::ManualStreamingCompressorUtility compressor(
                input.compression_level,
                compression::zlib::CompressionFormat::GZIP);

            fileio::StreamingFileWriterUtility writer(input.output_path);

            fileio::RawData raw_data;
            raw_data.data.assign(buffer.begin(), buffer.end());
            auto compressed_chunks = co_await compressor.process(raw_data);

            for (const auto& chunk : compressed_chunks) {
                fileio::RawData raw_chunk{chunk.data};
                co_await writer.process(raw_chunk);
            }

            auto final_chunks = compressor.finalize();
            for (const auto& chunk : final_chunks) {
                fileio::RawData raw_chunk{chunk.data};
                co_await writer.process(raw_chunk);
            }

            writer.close();
        } else {
            ssize_t fd = co_await ::dftracer::utils::io::open(
                input.output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                DFTRACER_UTILS_LOG_ERROR("Failed to open output file: %s",
                                         input.output_path.c_str());
                co_return false;
            }
            co_await ::dftracer::utils::io::write(static_cast<int>(fd),
                                                  buffer.data(), buffer.size());
            co_await ::dftracer::utils::io::close(static_cast<int>(fd));
        }

        co_return true;
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to write output: %s", e.what());
        co_return false;
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
