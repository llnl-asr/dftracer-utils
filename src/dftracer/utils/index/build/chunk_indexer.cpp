#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/json/json_doc_guard.h>
#include <dftracer/utils/json/parser.h>
#include <dftracer/utils/trace/internal/chunk_line_scanner.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/utilities/fileio/file_process_types.h>
#include <dftracer/utils/utilities/fileio/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>

#include <cstring>
#include <map>
#include <string_view>

using dftracer::utils::json::JsonParser;
using dftracer::utils::json::ondemand_value_to_string;

namespace dftracer::utils::index::build {

namespace {

static const std::string DIM_HHASH = "hhash";
static const std::string DIM_FHASH = "fhash";
static const std::string DIM_SHASH = "shash";

static const std::string DIM_NAME = "name";
static const std::string DIM_CAT = "cat";
static const std::string DIM_PID = "pid";
static const std::string DIM_TID = "tid";

std::vector<std::string> get_target_dimensions(
    const ChunkIndexerConfig& config) {
    std::vector<std::string> dims;

    if (config.index_name) dims.push_back(DIM_NAME);
    if (config.index_cat) dims.push_back(DIM_CAT);
    if (config.index_pid) dims.push_back(DIM_PID);
    if (config.index_tid) dims.push_back(DIM_TID);
    if (config.index_hhash) dims.push_back(DIM_HHASH);
    if (config.index_fhash) dims.push_back(DIM_FHASH);
    if (config.index_shash) dims.push_back(DIM_SHASH);

    for (const auto& extra : config.extra_dimensions) {
        dims.push_back(extra);
    }

    return dims;
}

}  // namespace

coro::CoroTask<ChunkIndexerOutput> ChunkIndexer::operator()(
    const ChunkIndexerInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("index chunk");
    ChunkIndexerOutput output;
    output.checkpoint_idx = input.checkpoint_idx;
    output.events_processed = 0;
    output.success = false;

    const ChunkIndexState* existing = nullptr;
    if (input.existing_state) {
        existing = input.existing_state.get();
    }

    std::vector<std::string> target_dims = get_target_dimensions(input.config);
    std::vector<std::string> missing_dims;

    if (existing) {
        missing_dims = existing->indexed_dims.missing_dimensions(input.config);

        const std::uint64_t current_hash =
            input.config.params_hash(index::store::IndexExtension::BLOOM);
        bool config_changed =
            existing->config_hash != 0 && existing->config_hash != current_hash;

        if (config_changed) {
            DFTRACER_UTILS_LOG_INFO(
                "ChunkIndexer: Config changed for checkpoint %llu, "
                "forcing full re-index",
                static_cast<unsigned long long>(input.checkpoint_idx));
            missing_dims = target_dims;
        }

        if (missing_dims.empty()) {
            DFTRACER_UTILS_LOG_INFO(
                "ChunkIndexer: All dimensions already indexed for checkpoint "
                "%llu, skipping re-scan",
                static_cast<unsigned long long>(input.checkpoint_idx));

            output.bloom_filters.clear();
            output.statistics = existing->statistics;
            output.events_processed = existing->events_processed;
            output.success = true;
            co_return output;
        }

        DFTRACER_UTILS_LOG_INFO(
            "ChunkIndexer: Incremental re-scan for checkpoint %llu, "
            "missing %zu dimensions",
            static_cast<unsigned long long>(input.checkpoint_idx),
            missing_dims.size());
    } else {
        missing_dims = target_dims;
    }

    auto make_bloom = [&]() {
        return index::extensions::ScalableBloomFilter(
            input.config.expected_entries_per_chunk,
            input.config.false_positive_rate);
    };

    for (const auto& dim : missing_dims) {
        output.bloom_filters.emplace(dim, make_bloom());
    }

    bool need_rescan = !missing_dims.empty();

    std::shared_ptr<utilities::reader::internal::Reader> reader;
    if (need_rescan) {
        auto reader_input =
            utilities::fileio::IndexedReadInput::from_file(input.file_path)
                .with_checkpoint_size(input.checkpoint_size)
                .with_index(input.index_path);

        utilities::fileio::IndexedFileReaderUtility reader_utility;
        reader = co_await reader_utility(reader_input);

        if (!reader) {
            DFTRACER_UTILS_LOG_ERROR(
                "ChunkIndexer: Failed to create reader for %s checkpoint %llu",
                input.file_path.c_str(),
                static_cast<unsigned long long>(input.checkpoint_idx));
            co_return output;
        }
    }

    // Statistics are recomputed from scratch to avoid inflating totals.

    if (!need_rescan) {
        if (existing) {
            output.statistics = existing->statistics;
            output.events_processed = existing->events_processed;
        }
        output.success = true;
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
        DFTRACER_UTILS_LOG_ERROR(
            "ChunkIndexer: Failed to create stream for %s checkpoint %llu",
            input.file_path.c_str(),
            static_cast<unsigned long long>(input.checkpoint_idx));
        co_return output;
    }

    JsonParser parser;

    const bool need_name = output.bloom_filters.count(DIM_NAME) > 0;
    const bool need_cat = output.bloom_filters.count(DIM_CAT) > 0;
    const bool need_pid = output.bloom_filters.count(DIM_PID) > 0;
    const bool need_tid = output.bloom_filters.count(DIM_TID) > 0;
    const bool need_hhash = output.bloom_filters.count(DIM_HHASH) > 0;
    const bool need_fhash = output.bloom_filters.count(DIM_FHASH) > 0;
    const bool need_shash = output.bloom_filters.count(DIM_SHASH) > 0;
    const bool has_extra_dims = !input.config.extra_dimensions.empty();

    co_await dftracer::utils::trace::internal::scan_chunk_lines(
        *stream, [&](std::string_view line_sv,
                     [[maybe_unused]] std::uint32_t line_number) {
            if (!parser.parse(line_sv)) {
                return;
            }

            trace::RecordPhase phase = trace::read_phase(parser);
            if (phase == trace::RecordPhase::UNKNOWN) {
                return;
            }

            bool is_metadata = (phase == trace::RecordPhase::METADATA);

            if (!is_metadata) {
                // Re-parse: the phase read above already consumed the
                // document's cursor state.
                parser.parse(line_sv);
                auto name_opt = parser.get_string("name");
                std::string_view name = name_opt.value_or("");
                auto cat_opt = parser.get_string("cat");
                std::string_view cat = cat_opt.value_or("");

                auto pid = parser.get_uint64("pid").value_or(0);
                auto tid = parser.get_uint64("tid").value_or(0);
                auto ts = parser.get_uint64("ts").value_or(0);
                auto dur_opt = parser.get_uint64("dur");
                auto dur = dur_opt.value_or(0);

                output.statistics.update_from_event(name, cat, pid, tid, ts,
                                                    dur, dur_opt.has_value());

                if (need_name && !name.empty()) {
                    output.bloom_filters[DIM_NAME].add(name);
                }

                if (need_cat && !cat.empty()) {
                    output.bloom_filters[DIM_CAT].add(cat);
                }

                if (need_pid) {
                    char pid_buf[32];
                    int n = std::snprintf(pid_buf, sizeof(pid_buf), "%llu",
                                          static_cast<unsigned long long>(pid));
                    output.bloom_filters[DIM_PID].add(
                        std::string_view(pid_buf, n));
                }

                if (need_tid) {
                    char tid_buf[32];
                    int n = std::snprintf(tid_buf, sizeof(tid_buf), "%llu",
                                          static_cast<unsigned long long>(tid));
                    output.bloom_filters[DIM_TID].add(
                        std::string_view(tid_buf, n));
                }

                if (need_hhash || need_fhash || need_shash || has_extra_dims) {
                    parser.for_each_field("args", [&](std::string_view key,
                                                      simdjson::ondemand::value
                                                          val) {
                        if (need_hhash && key == "hhash") {
                            auto s = val.get_string();
                            if (!s.error() && !s.value().empty()) {
                                output.bloom_filters[DIM_HHASH].add(s.value());
                            }
                        } else if (need_fhash && key == "fhash") {
                            auto s = val.get_string();
                            if (!s.error() && !s.value().empty()) {
                                output.bloom_filters[DIM_FHASH].add(s.value());
                            }
                        } else if (need_shash &&
                                   (key == "cmd_hash" || key == "exec_hash")) {
                            auto s = val.get_string();
                            if (!s.error() && !s.value().empty()) {
                                output.bloom_filters[DIM_SHASH].add(s.value());
                            }
                        } else if (has_extra_dims) {
                            for (const auto& dim :
                                 input.config.extra_dimensions) {
                                if (key == dim) {
                                    std::string str_val =
                                        ondemand_value_to_string(val);
                                    if (!str_val.empty()) {
                                        output.bloom_filters[dim].add(str_val);
                                    }
                                    break;
                                }
                                // Nested dimension, e.g. "io.size".
                                auto dot_pos = dim.find('.');
                                if (dot_pos != std::string::npos) {
                                    std::string_view prefix(dim.data(),
                                                            dot_pos);
                                    if (key == prefix) {
                                        std::string_view suffix(
                                            dim.data() + dot_pos + 1,
                                            dim.size() - dot_pos - 1);
                                        auto obj = val.get_object();
                                        if (!obj.error()) {
                                            for (auto field : obj.value()) {
                                                if (field.error()) continue;
                                                auto fkey =
                                                    field.unescaped_key();
                                                if (fkey.error()) continue;
                                                if (fkey.value() == suffix) {
                                                    auto fval = field.value();
                                                    if (fval.error()) continue;
                                                    std::string str_val =
                                                        ondemand_value_to_string(
                                                            fval.value());
                                                    if (!str_val.empty()) {
                                                        output
                                                            .bloom_filters[dim]
                                                            .add(str_val);
                                                    }
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    });
                }

                output.events_processed++;
            }
        });

    output.success = true;
    co_return output;
}

}  // namespace dftracer::utils::index::build
