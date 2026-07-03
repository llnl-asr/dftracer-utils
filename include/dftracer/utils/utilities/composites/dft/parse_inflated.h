#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_PARSE_INFLATED_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_PARSE_INFLATED_H

#include <dftracer/utils/utilities/common/json/json.h>
#include <dftracer/utils/utilities/composites/dft/dft_event_visitor.h>
#include <dftracer/utils/utilities/composites/dft/event.h>
#include <simdjson.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft {

// Replace "[\n" / "]\n" delimiter lines with spaces in-place. Mirrors the
// behaviour used by DftEventDispatcher so parse_buffer can consume the
// stripped buffer with parse_many. Public so the direct-organize path can
// reuse it on inflated buffers it accumulates itself.
inline void strip_array_delimiters(char* buf, std::size_t len) {
    for (std::size_t i = 0; i < len;) {
        std::size_t line_start = i;
        std::size_t line_end = i;
        while (line_end < len && buf[line_end] != '\n') ++line_end;

        bool has_bracket = false;
        for (std::size_t j = line_start; j < line_end; ++j) {
            char c = buf[j];
            if (c == ' ' || c == '\t' || c == '\r') continue;
            if ((c == '[' || c == ']') && !has_bracket) {
                has_bracket = true;
            } else {
                has_bracket = false;
                break;
            }
        }

        if (has_bracket) {
            for (std::size_t j = line_start; j < line_end; ++j) buf[j] = ' ';
        }

        i = (line_end < len) ? line_end + 1 : len;
    }
}

// Iterate parsed dftracer events from a single inflated buffer. The buffer
// is assumed to hold concatenated NDJSON-ish events plus the simdjson
// padding required by parse_many; callers responsible for stripping any
// "[" / "]" delimiter lines (see strip_array_delimiters above).
//
// `chunk_buffer` is held shared so the EventRecord's string_view can outlive
// the loop body if a visitor stashes it. `len` excludes the simdjson
// padding tail. `line_number` is incremented for each successfully parsed
// event before being copied into the EventRecord. `needs_args_map` follows
// the existing dispatcher contract (when any consumer requires the args
// JSON object materialized, take the slower DFTracerEvent::parse path).
//
// Returns the number of trailing bytes parse_many reported as truncated, so
// the caller can carry them over to the next buffer.
template <typename Cb>
std::size_t parse_buffer(simdjson::dom::parser& parser,
                         std::shared_ptr<std::string> chunk_buffer,
                         std::size_t len, std::size_t checkpoint_idx,
                         std::size_t& line_number, bool needs_args_map,
                         Cb&& cb) {
    if (!chunk_buffer || len == 0) return 0;

    simdjson::dom::document_stream stream;
    auto err = parser.parse_many(chunk_buffer->data(), len, len).get(stream);
    if (err) return 0;

    for (auto it = stream.begin(); it != stream.end(); ++it) {
        if ((*it).error()) continue;
        auto root = (*it).value_unsafe();
        if (!root.is_object()) continue;
        common::json::JsonValue json(root);
        DFTracerEvent ev;
        simdjson::dom::element args_dom{};
        bool has_args = false;
        bool ok = false;
        if (needs_args_map) {
            ok = DFTracerEvent::parse(json, ev, args_dom, has_args);
        } else {
            ok = DFTracerEvent::parse_scalars(root, ev, args_dom, has_args);
        }
        if (!ok) continue;

        std::size_t ln = line_number++;
        std::string_view src = it.source();
        EventRecord record{ev, json,     src,     chunk_buffer, checkpoint_idx,
                           ln, args_dom, has_args};
        cb(record);
    }

    std::size_t truncated = stream.truncated_bytes();
    return (truncated > 0 && truncated <= len) ? truncated : 0;
}

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_PARSE_INFLATED_H
