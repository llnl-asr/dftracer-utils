#ifndef DFTRACER_UTILS_TRACE_PARSE_INFLATED_H
#define DFTRACER_UTILS_TRACE_PARSE_INFLATED_H

#include <dftracer/utils/json/json.h>
#include <dftracer/utils/trace/event.h>
#include <simdjson.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace dftracer::utils::trace {

/// Replace "[\n" / "]\n" delimiter lines with spaces in-place so parse_buffer
/// can consume the stripped buffer with parse_many. Public so the
/// direct-organize path can reuse it on inflated buffers it accumulates itself.
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

/// Calls `emit(root, source)` for every JSON document on the complete lines
/// of `base[0, len)`, with parse_buffer's batch-then-line-by-line recovery.
/// Returns the bytes after the last newline, which the caller carries.
template <typename Emit>
std::size_t parse_lines(simdjson::dom::parser& parser, const char* base,
                        std::size_t len, Emit&& emit) {
    std::size_t parse_len = 0;
    for (std::size_t i = len; i-- > 0;) {
        if (base[i] == '\n') {
            parse_len = i + 1;
            break;
        }
    }
    // A record longer than the whole chunk: carry it to the next.
    if (parse_len == 0) return len;

    std::size_t covered = 0;
    {
        simdjson::dom::document_stream stream;
        auto err = parser.parse_many(base, parse_len, parse_len).get(stream);
        if (!err) {
            for (auto it = stream.begin(); it != stream.end(); ++it) {
                if ((*it).error()) continue;
                std::string_view src = it.source();
                emit((*it).value_unsafe(), src);
                covered =
                    static_cast<std::size_t>(src.data() - base) + src.size();
            }
        }
    }

    // A bad record can desync parse_many's single structural scan and swallow
    // the rest of the batch as one rejected document. Re-parse whatever it did
    // not reach, line by line, so a malformed record drops only itself.
    if (covered < parse_len) {
        std::size_t pos = covered;
        if (pos > 0) {  // resume at the line after the last record parsed
            while (pos < parse_len && base[pos] != '\n') ++pos;
            if (pos < parse_len) ++pos;
        }
        while (pos < parse_len) {
            std::size_t end = pos;
            while (end < parse_len && base[end] != '\n') ++end;
            if (end > pos) {
                auto r = parser.parse(base + pos, end - pos);
                if (!r.error())
                    emit(r.value_unsafe(),
                         std::string_view(base + pos, end - pos));
            }
            pos = end + 1;
        }
    }

    return len - parse_len;
}

/// Iterate parsed dftracer events from one inflated buffer (NDJSON plus
/// simdjson padding; the caller strips any "[" / "]" delimiter lines).
/// `chunk_buffer` is held shared so an EventRecord's string_view can outlive
/// the loop. `needs_args_map` takes the slower parse path that materializes the
/// args map. Returns the trailing incomplete line for the caller to carry into
/// the next buffer.
///
/// Parsed only through the last newline, not by parse_many's own truncation
/// (which reads zero when a chunk is cut mid-string, leaving the next chunk to
/// start mid-string and be rejected whole). parse_many derives all boundaries
/// from one scan of the batch, so a single unescaped character in a string
/// collapses it into one rejected document; on that (nothing delivered) the
/// chunk is re-parsed line by line so a bad record drops only itself.
template <typename Cb>
std::size_t parse_buffer(simdjson::dom::parser& parser,
                         std::shared_ptr<std::string> chunk_buffer,
                         std::size_t len, std::size_t checkpoint_idx,
                         std::size_t& line_number, bool needs_args_map,
                         Cb&& cb) {
    if (!chunk_buffer || len == 0) return 0;
    return parse_lines(parser, chunk_buffer->data(), len,
                       [&](simdjson::dom::element root, std::string_view src) {
                           if (!root.is_object()) return;
                           json::JsonValue json(root);
                           DFTracerEvent ev;
                           simdjson::dom::element args_dom{};
                           bool has_args = false;
                           bool ok = needs_args_map
                                         ? DFTracerEvent::parse(
                                               json, ev, args_dom, has_args)
                                         : DFTracerEvent::parse_scalars(
                                               root, ev, args_dom, has_args);
                           if (!ok) return;
                           std::size_t ln = line_number++;
                           EventRecord record{ev,           json,           src,
                                              chunk_buffer, checkpoint_idx, ln,
                                              args_dom,     has_args};
                           cb(record);
                       });
}

}  // namespace dftracer::utils::trace

#endif  // DFTRACER_UTILS_TRACE_PARSE_INFLATED_H
