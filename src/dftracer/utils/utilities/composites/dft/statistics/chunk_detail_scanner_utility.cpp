#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/common/json/json.h>
#include <dftracer/utils/utilities/composites/dft/args_map.h>
#include <dftracer/utils/utilities/composites/dft/event.h>
#include <dftracer/utils/utilities/composites/dft/internal/chunk_line_scanner.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/statistics/chunk_detail_scanner_utility.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <simdjson.h>

#include <charconv>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>

using dftracer::utils::utilities::common::json::JsonValue;
using dftracer::utils::utilities::composites::dft::DFTracerEvent;

namespace dftracer::utils::utilities::composites::dft::statistics {

inline constexpr std::string_view GLOBAL_GROUP_KEY = "__global__";

static bool is_io_event(std::string_view name) {
    using namespace dftracer::utils::utilities::composites::dft::internal;
    for (auto op : posix_ops::FILE_READ)
        if (op == name) return true;
    for (auto op : posix_ops::FILE_WRITE)
        if (op == name) return true;
    return false;
}

static void build_group_key(std::string& key,
                            const std::vector<std::string>& group_by,
                            const DFTracerEvent& ev) {
    key.clear();

    for (std::size_t i = 0; i < group_by.size(); ++i) {
        if (i > 0) key.push_back('|');
        const auto& dim = group_by[i];
        if (dim == "name") {
            key += ev.name;
        } else if (dim == "cat") {
            key += ev.cat;
        } else if (dim == "pid") {
            char buf[32];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), ev.pid);
            if (ec == std::errc()) {
                key.append(buf, ptr - buf);
            }
        } else if (dim == "tid") {
            char buf[32];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), ev.tid);
            if (ec == std::errc()) {
                key.append(buf, ptr - buf);
            }
        } else if (dim == "pid_tid") {
            char buf[64];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), ev.pid);
            if (ec == std::errc()) {
                key.append(buf, ptr - buf);
                key.push_back(':');
                auto [ptr2, ec2] =
                    std::to_chars(ptr, buf + sizeof(buf), ev.tid);
                if (ec2 == std::errc()) {
                    key.append(ptr, ptr2 - ptr);
                }
            }
        } else if (dim == "fhash") {
            if (ev.args.exists()) {
                key += ev.args["fhash"].get<std::string_view>();
            }
        } else if (dim == "hhash") {
            if (ev.args.exists()) {
                key += ev.args["hhash"].get<std::string_view>();
            }
        }
    }
}

coro::CoroTask<Result<ChunkDetailScanOutput>>
ChunkDetailScannerUtility::process(const ChunkDetailScanInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("scan chunk details");
    ChunkDetailScanOutput output;

    std::unordered_set<std::string_view> name_filter;
    std::unordered_set<std::string_view> cat_filter;
    if (input.filter_names) {
        for (const auto& n : *input.filter_names) {
            name_filter.insert(n);
        }
    }
    if (input.filter_categories) {
        for (const auto& c : *input.filter_categories) {
            cat_filter.insert(c);
        }
    }

    bool has_name_filter = !name_filter.empty();
    bool has_cat_filter = !cat_filter.empty();
    bool has_grouping = input.group_by && !input.group_by->empty();

    auto reader_input = composites::IndexedReadInput::from_file(input.file_path)
                            .with_checkpoint_size(input.checkpoint_size)
                            .with_index(input.index_path);

    composites::IndexedFileReaderUtility reader_utility;
    auto reader = co_await reader_utility.process(reader_input);

    if (!reader) {
        DFTRACER_UTILS_LOG_ERROR(
            "ChunkDetailScanner: Failed to create reader for %s checkpoint "
            "%llu",
            input.file_path.c_str(),
            static_cast<unsigned long long>(input.checkpoint_idx));
        co_return make_error(
            ErrorCode::READER,
            "ChunkDetailScanner: Failed to create reader for " +
                input.file_path + " checkpoint " +
                std::to_string(input.checkpoint_idx));
    }

    auto stream = reader->stream(
        reader::internal::StreamConfig()
            .stream_type(reader::internal::StreamType::MULTI_LINES_BYTES)
            .range_type(reader::internal::RangeType::BYTE_RANGE)
            .buffer_size(input.batch_size)
            .from(input.start_byte)
            .to(input.end_byte));

    if (!stream) {
        DFTRACER_UTILS_LOG_ERROR(
            "ChunkDetailScanner: Failed to create stream for %s checkpoint "
            "%llu",
            input.file_path.c_str(),
            static_cast<unsigned long long>(input.checkpoint_idx));
        co_return make_error(
            ErrorCode::READER,
            "ChunkDetailScanner: Failed to create stream for " +
                input.file_path + " checkpoint " +
                std::to_string(input.checkpoint_idx));
    }

    std::string group_key_buf;
    group_key_buf.reserve(128);
    static const std::string global_key{GLOBAL_GROUP_KEY};

    simdjson::dom::parser parser;

    co_await dft::internal::scan_chunk_lines(*stream, [&](std::string_view line,
                                                          std::uint32_t) {
        auto result = parser.parse(line.data(), line.size());
        if (!result.error()) {
            auto root = result.value_unsafe();
            if (root.is_object()) {
                JsonValue json(root);
                DFTracerEvent ev;
                if (!DFTracerEvent::parse(json, ev)) {
                    return;
                }

                if (!ev.is_metadata()) {
                    bool passes = true;
                    if (has_name_filter &&
                        name_filter.find(ev.name) == name_filter.end()) {
                        passes = false;
                    }
                    if (passes && has_cat_filter &&
                        cat_filter.find(ev.cat) == cat_filter.end()) {
                        passes = false;
                    }

                    if (passes) {
                        double dur = static_cast<double>(ev.dur);

                        output.stats.duration.update(dur);

                        bool is_io = is_io_event(ev.name);
                        const std::string* io_key_ptr;

                        if (has_grouping) {
                            build_group_key(group_key_buf, *input.group_by, ev);

                            output.stats.grouped_duration[group_key_buf].update(
                                dur);
                            output.stats.group_key_category.try_emplace(
                                group_key_buf, ev.cat);
                            io_key_ptr = &group_key_buf;
                        } else {
                            io_key_ptr = &global_key;
                        }

                        if (is_io && ev.args.exists()) {
                            auto ret_opt =
                                ev.args["ret"].get_optional<std::int64_t>();
                            if (ret_opt.has_value() && ret_opt.value() > 0) {
                                double ret =
                                    static_cast<double>(ret_opt.value());
                                auto& io = output.stats.grouped_io[*io_key_ptr];
                                io.duration.update(dur);
                                io.size.update(ret);
                                if (dur > 0) {
                                    io.bandwidth.update(ret * 1e6 / dur);
                                }
                                auto offset_opt =
                                    ev.args["offset"]
                                        .get_optional<std::uint64_t>();
                                if (offset_opt.has_value()) {
                                    io.offset.update(static_cast<double>(
                                        offset_opt.value()));
                                }
                            }
                        }

                        output.stats.events_scanned++;
                    }
                }
            }
        }
    });

    output.stats.chunks_scanned = 1;
    co_return output;
}

}  // namespace dftracer::utils::utilities::composites::dft::statistics
