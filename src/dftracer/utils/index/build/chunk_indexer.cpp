// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/json/json_doc_guard.h>
#include <dftracer/utils/json/parser.h>
#include <dftracer/utils/trace/internal/chunk_line_scanner.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/utilities/fileio/file_process_types.h>
#include <dftracer/utils/utilities/fileio/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>

#include <cstring>

using dftracer::utils::json::JsonParser;
using dftracer::utils::json::ondemand_value_to_string;

namespace dftracer::utils::index::build {

namespace {

static const dstd::String DIM_HHASH = "hhash";
static const dstd::String DIM_FHASH = "fhash";
static const dstd::String DIM_SHASH = "shash";

static const dstd::String DIM_NAME = "name";
static const dstd::String DIM_CAT = "cat";
static const dstd::String DIM_PID = "pid";
static const dstd::String DIM_TID = "tid";

dstd::Vector<dstd::String> get_target_dimensions(
    const ChunkIndexerConfig& config) {
    dstd::Vector<dstd::String> dims;

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

coro::CoroTask<dstd::Result<ChunkIndexerOutput>> ChunkIndexer::operator()(
    const ChunkIndexerInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("index chunk");
    ChunkIndexerOutput output;
    output.checkpoint_idx = input.checkpoint_idx;
    output.events_processed = 0;

    const ChunkIndexState* existing = nullptr;
    if (input.existing_state) {
        existing = input.existing_state.get();
    }

    dstd::Vector<dstd::String> target_dims =
        get_target_dimensions(input.config);
    dstd::Vector<dstd::String> missing_dims;

    if (existing) {
        missing_dims = existing->indexed_dims.missing_dimensions(input.config);

        const std::uint64_t current_hash =
            input.config.params_hash(index::store::IndexExtension::Bloom);
        bool config_changed =
            existing->config_hash != 0 && existing->config_hash != current_hash;

        if (config_changed) {
            DFTRACER_UTILS_LOG_INFO(
                "ChunkIndexer: Config changed for checkpoint {}, forcing full "
                "re-index",
                static_cast<unsigned long long>(input.checkpoint_idx));
            missing_dims = target_dims;
        }

        if (missing_dims.empty()) {
            DFTRACER_UTILS_LOG_INFO(
                "ChunkIndexer: All dimensions already indexed for checkpoint "
                "{}, skipping re-scan",
                static_cast<unsigned long long>(input.checkpoint_idx));

            output.bloom_filters.clear();
            output.statistics = existing->statistics;
            output.events_processed = existing->events_processed;
            co_return output;
        }

        DFTRACER_UTILS_LOG_INFO(
            "ChunkIndexer: Incremental re-scan for checkpoint {}, missing {} "
            "dimensions",
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

    dstd::SharedPtr<utilities::reader::internal::Reader> reader;
    if (need_rescan) {
        auto reader_input =
            utilities::fileio::IndexedReadInput::from_file(input.file_path)
                .with_checkpoint_size(input.checkpoint_size)
                .with_index(input.index_path);

        utilities::fileio::IndexedFileReaderUtility reader_utility;
        auto opened = co_await reader_utility(reader_input);
        if (!opened) {
            co_return opened.error();
        }
        reader = std::move(*opened);

        if (!reader) {
            co_return common::make_error<
                "ChunkIndexer: no reader for {} checkpoint {}">(
                common::ErrorCode::Internal, input.file_path,
                static_cast<unsigned long long>(input.checkpoint_idx));
        }
    }

    // Statistics are recomputed from scratch to avoid inflating totals.

    if (!need_rescan) {
        if (existing) {
            output.statistics = existing->statistics;
            output.events_processed = existing->events_processed;
        }
        co_return output;
    }

    auto opened_stream = reader->stream(
        utilities::reader::internal::StreamConfig()
            .stream_type(
                utilities::reader::internal::StreamType::MultiLinesBytes)
            .range_type(utilities::reader::internal::RangeType::ByteRange)
            .buffer_size(input.batch_size)
            .from(input.start_byte)
            .to(input.end_byte));

    if (!opened_stream) {
        co_return opened_stream.error();
    }
    auto stream = std::move(*opened_stream);

    JsonParser parser;

    const bool need_name = output.bloom_filters.count(DIM_NAME) > 0;
    const bool need_cat = output.bloom_filters.count(DIM_CAT) > 0;
    const bool need_pid = output.bloom_filters.count(DIM_PID) > 0;
    const bool need_tid = output.bloom_filters.count(DIM_TID) > 0;
    const bool need_hhash = output.bloom_filters.count(DIM_HHASH) > 0;
    const bool need_fhash = output.bloom_filters.count(DIM_FHASH) > 0;
    const bool need_shash = output.bloom_filters.count(DIM_SHASH) > 0;
    const bool has_extra_dims = !input.config.extra_dimensions.empty();

    auto scanned = co_await dftracer::utils::trace::internal::scan_chunk_lines(
        *stream, [&](dstd::StringView line_sv,
                     [[maybe_unused]] std::uint32_t line_number) {
            if (!parser.parse(line_sv)) {
                return;
            }

            trace::RecordPhase phase = trace::read_phase(parser);
            if (phase == trace::RecordPhase::Unknown) {
                return;
            }

            bool is_metadata = (phase == trace::RecordPhase::Metadata);

            if (!is_metadata) {
                // The phase read above consumed the document's cursor. A
                // syntax error in what it read drops the line.
                if (!parser.raw_document().is_alive()) return;
                parser.rewind();
                auto name_opt = parser.get_string("name");
                dstd::StringView name = name_opt.value_or("");
                auto cat_opt = parser.get_string("cat");
                dstd::StringView cat = cat_opt.value_or("");

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
                        dstd::StringView(pid_buf, n));
                }

                if (need_tid) {
                    char tid_buf[32];
                    int n = std::snprintf(tid_buf, sizeof(tid_buf), "%llu",
                                          static_cast<unsigned long long>(tid));
                    output.bloom_filters[DIM_TID].add(
                        dstd::StringView(tid_buf, n));
                }

                if (need_hhash || need_fhash || need_shash || has_extra_dims) {
                    parser.for_each_field("args", [&](dstd::StringView key,
                                                      simdjson::ondemand::value
                                                          val) {
                        if (need_hhash && key == "hhash") {
                            auto s = val.get_string();
                            if (!s.error() && !s.value_unsafe().empty()) {
                                output.bloom_filters[DIM_HHASH].add(
                                    s.value_unsafe());
                            }
                        } else if (need_fhash && key == "fhash") {
                            auto s = val.get_string();
                            if (!s.error() && !s.value_unsafe().empty()) {
                                output.bloom_filters[DIM_FHASH].add(
                                    s.value_unsafe());
                            }
                        } else if (need_shash &&
                                   (key == "cmd_hash" || key == "exec_hash")) {
                            auto s = val.get_string();
                            if (!s.error() && !s.value_unsafe().empty()) {
                                output.bloom_filters[DIM_SHASH].add(
                                    s.value_unsafe());
                            }
                        } else if (has_extra_dims) {
                            for (const auto& dim :
                                 input.config.extra_dimensions) {
                                if (key == dim) {
                                    dstd::String str_val =
                                        ondemand_value_to_string(val);
                                    if (!str_val.empty()) {
                                        output.bloom_filters[dim].add(str_val);
                                    }
                                    break;
                                }
                                // Nested dimension, e.g. "io.size".
                                auto dot_pos = dim.find('.');
                                if (dot_pos != dstd::String::npos) {
                                    dstd::StringView prefix(dim.data(),
                                                            dot_pos);
                                    if (key == prefix) {
                                        dstd::StringView suffix(
                                            dim.data() + dot_pos + 1,
                                            dim.size() - dot_pos - 1);
                                        auto obj = val.get_object();
                                        if (!obj.error()) {
                                            for (auto field :
                                                 obj.value_unsafe()) {
                                                if (field.error()) continue;
                                                auto fkey =
                                                    field.unescaped_key();
                                                if (fkey.error()) continue;
                                                if (fkey.value_unsafe() ==
                                                    suffix) {
                                                    auto fval = field.value();
                                                    if (fval.error()) continue;
                                                    dstd::String str_val =
                                                        ondemand_value_to_string(
                                                            fval.value_unsafe());
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
    if (!scanned) {
        co_return scanned.error();
    }

    co_return output;
}

}  // namespace dftracer::utils::index::build
