#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/common/json/json.h>
#include <dftracer/utils/utilities/composites/dft/event.h>
#include <dftracer/utils/utilities/composites/dft/statistics/chunk_detail_scanner_utility.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <yyjson.h>

#include <array>
#include <charconv>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>

// Import JsonValue from common json namespace
using dftracer::utils::utilities::common::json::JsonValue;
using dftracer::utils::utilities::composites::dft::DFTracerEvent;

namespace dftracer::utils::utilities::composites::dft::statistics {

// Constant for global (non-grouped) I/O metrics
inline constexpr std::string_view GLOBAL_GROUP_KEY = "__global__";

// Event names where args.ret represents bytes transferred (actual I/O).
// Other syscalls like lseek64 (returns offset) and fork (returns PID)
// have ret with different semantics and must be excluded.
static constexpr auto IO_EVENT_NAMES =
    std::to_array<std::string_view>({"read", "write", "pread", "pwrite",
                                     "pread64", "pwrite64", "readv", "writev"});

static bool is_io_event(std::string_view name) {
    return std::find(IO_EVENT_NAMES.begin(), IO_EVENT_NAMES.end(), name) !=
           IO_EVENT_NAMES.end();
}

// Build a composite group key from the requested dimensions.
static void build_group_key(std::string& key,
                            const std::vector<std::string>& group_by,
                            const JsonValue& json, const JsonValue& args) {
    key.clear();

    for (std::size_t i = 0; i < group_by.size(); ++i) {
        if (i > 0) key.push_back('|');
        const auto& dim = group_by[i];
        if (dim == "name") {
            key += json["name"].get<std::string_view>();
        } else if (dim == "cat") {
            key += json["cat"].get<std::string_view>();
        } else if (dim == "pid" || dim == "tid") {
            std::uint64_t val = json[dim].get<std::uint64_t>();
            char buf[32];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val);
            if (ec == std::errc()) {
                key.append(buf, ptr - buf);
            }
        } else if (dim == "pid_tid") {
            std::uint64_t pid = json["pid"].get<std::uint64_t>();
            std::uint64_t tid = json["tid"].get<std::uint64_t>();
            char buf[64];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), pid);
            if (ec == std::errc()) {
                key.append(buf, ptr - buf);
                key.push_back(':');
                auto [ptr2, ec2] = std::to_chars(ptr, buf + sizeof(buf), tid);
                if (ec2 == std::errc()) {
                    key.append(ptr, ptr2 - ptr);
                }
            }
        } else if (dim == "fhash") {
            if (args.exists()) {
                key += args["fhash"].get<std::string_view>();
            }
        } else if (dim == "hhash") {
            if (args.exists()) {
                key += args["hhash"].get<std::string_view>();
            }
        }
    }
}

coro::CoroTask<ChunkDetailScanOutput> ChunkDetailScannerUtility::process(
    const ChunkDetailScanInput& input) {
    ChunkDetailScanOutput output;
    output.success = false;

    // Build filter sets for O(1) lookup
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

    // Create reader (same pattern as chunk_indexer_utility.cpp)
    auto reader_input = composites::IndexedReadInput::from_file(input.file_path)
                            .with_checkpoint_size(input.checkpoint_size)
                            .with_index(input.idx_path);

    composites::IndexedFileReaderUtility reader_utility;
    auto reader = co_await reader_utility.process(reader_input);

    if (!reader) {
        DFTRACER_UTILS_LOG_ERROR(
            "ChunkDetailScanner: Failed to create reader for %s checkpoint "
            "%llu",
            input.file_path.c_str(),
            static_cast<unsigned long long>(input.checkpoint_idx));
        co_return output;
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
        co_return output;
    }

    std::string group_key_buf;
    group_key_buf.reserve(128);
    static const std::string global_key{GLOBAL_GROUP_KEY};

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
                        JsonValue json(root);
                        DFTracerEvent ev;
                        if (!DFTracerEvent::parse(json, ev)) {
                            yyjson_doc_free(doc);
                            pos = (newline - data) + 1;
                            continue;
                        }

                        if (!ev.is_metadata()) {
                            // Regular event

                            // Apply filters
                            bool passes = true;
                            if (has_name_filter && name_filter.find(ev.name) ==
                                                       name_filter.end()) {
                                passes = false;
                            }
                            if (passes && has_cat_filter &&
                                cat_filter.find(ev.cat) == cat_filter.end()) {
                                passes = false;
                            }

                            if (passes) {
                                double dur = static_cast<double>(ev.dur);

                                // Global duration
                                output.stats.duration.update(dur);

                                // Determine I/O key for this event
                                bool is_io = is_io_event(ev.name);
                                const std::string* io_key_ptr;

                                if (has_grouping) {
                                    build_group_key(group_key_buf,
                                                    *input.group_by, json,
                                                    ev.args);

                                    output.stats.grouped_duration[group_key_buf]
                                        .update(dur);
                                    // Only insert category on first occurrence
                                    output.stats.group_key_category.try_emplace(
                                        group_key_buf, ev.cat);
                                    io_key_ptr = &group_key_buf;
                                } else {
                                    io_key_ptr = &global_key;
                                }

                                // I/O metrics: only for actual I/O events
                                if (is_io && ev.args.exists()) {
                                    auto ret_opt =
                                        ev.args["ret"]
                                            .get_optional<std::int64_t>();
                                    if (ret_opt.has_value() &&
                                        ret_opt.value() > 0) {
                                        double ret = static_cast<double>(
                                            ret_opt.value());
                                        auto& io = output.stats
                                                       .grouped_io[*io_key_ptr];
                                        io.duration.update(dur);
                                        io.size.update(ret);
                                        if (dur > 0) {
                                            io.bandwidth.update(ret * 1e6 /
                                                                dur);
                                        }
                                        auto offset_opt =
                                            ev.args["offset"]
                                                .get_optional<std::uint64_t>();
                                        if (offset_opt.has_value()) {
                                            io.offset.update(
                                                static_cast<double>(
                                                    offset_opt.value()));
                                        }
                                    }
                                }

                                output.stats.events_scanned++;
                            }
                        }
                    }
                    yyjson_doc_free(doc);
                }
            }

            pos = (newline - data) + 1;
        }
    }

    output.stats.chunks_scanned = 1;
    output.success = true;
    co_return output;
}

}  // namespace dftracer::utils::utilities::composites::dft::statistics
