// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_PARSE_INFLATED_H
#define DFTRACER_UTILS_TRACE_PARSE_INFLATED_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/json/json.h>
#include <dftracer/utils/trace/event.h>
#include <simdjson.h>

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

namespace dftracer::utils::trace {

/// Replace "[\n" / "]\n" delimiter lines with spaces in-place so parse_buffer
/// can consume the stripped buffer with parse_many. Public so the
/// direct-organize path can reuse it on inflated buffers it accumulates itself.
inline void strip_array_delimiters(char* buf, std::size_t len) {
    for (std::size_t i = 0; i < len;) {
        std::size_t line_start = i;
        const void* nl = std::memchr(buf + i, '\n', len - i);
        std::size_t line_end =
            nl ? static_cast<std::size_t>(static_cast<const char*>(nl) - buf)
               : len;

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

/// Bytes of lines one parse_many batch covers. The parser's capacity follows
/// the batch, not the buffer. A line longer than half of it is parsed alone.
inline constexpr std::size_t PARSE_BATCH_BYTES = std::size_t{128} << 10;

/// Stands for a refused-line callback that does nothing, so a caller that
/// wants none pays for none.
struct NoRefused {
    void operator()(dstd::StringView, std::size_t) const {}
};

/// Hands the line `base[begin, end)`, trimmed of blanks, to
/// `refused(line, capacity)` when it starts with '{'; `capacity` is the bytes
/// readable from the line in a buffer of `len` bytes plus the simdjson padding.
template <typename Refused>
void hand_over_refused(const char* base, std::size_t len, std::size_t begin,
                       std::size_t end, Refused& refused) {
    while (begin < end &&
           (base[begin] == ' ' || base[begin] == '\t' || base[begin] == '\r'))
        ++begin;
    while (end > begin && (base[end - 1] == ' ' || base[end - 1] == '\t' ||
                           base[end - 1] == '\r'))
        --end;
    if (end > begin && base[begin] == '{')
        refused(dstd::StringView(base + begin, end - begin),
                len + simdjson::SIMDJSON_PADDING - begin);
}

/// Calls `emit(root, source)` for every JSON document on the complete lines
/// of `base[0, len)`, with parse_buffer's batch-then-line-by-line recovery.
/// Returns the bytes after the last newline, which the caller carries.
/// Documents are read in batches of PARSE_BATCH_BYTES on the calling thread,
/// so a buffer of any size grows the parser by at most its longest line.
/// `base` needs `simdjson::SIMDJSON_PADDING` readable bytes after `len`.
///
/// A line the DOM parser refuses, with a number too large for a double, an
/// invalid escape, a bad byte or a nesting past its limit, goes to
/// `refused(line, capacity)` instead of being dropped, in its place among the
/// documents handed to `emit`. `line` is the text of the line without the
/// blanks around it and starts with '{'; `capacity` is the bytes readable from
/// it, including the padding. A line that parses is read once, by the DOM
/// parser, and never reaches `refused`. Two records on one line are two
/// documents and a record over two lines is one, as parse_many reads them.
template <typename Emit, typename Refused>
std::size_t parse_lines(simdjson::dom::parser& parser, const char* base,
                        std::size_t len, Emit&& emit, Refused&& refused) {
    constexpr bool HAS_REFUSED =
        !std::is_same_v<std::decay_t<Refused>, NoRefused>;
    std::size_t parse_len = 0;
    for (std::size_t i = len; i-- > 0;) {
        if (base[i] == '\n') {
            parse_len = i + 1;
            break;
        }
    }
    // A record longer than the whole chunk: carry it to the next.
    if (parse_len == 0) return len;

    parser.threaded = false;
    // `pos` is the start of the lines not yet delivered.
    std::size_t pos = 0;
    while (pos < parse_len) {
        std::size_t covered = pos;
        {
            simdjson::dom::document_stream stream;
            auto err =
                parser
                    .parse_many(base + pos, parse_len - pos, PARSE_BATCH_BYTES)
                    .get(stream);
            if (!err) {
                for (auto it = stream.begin(); it != stream.end(); ++it) {
                    if ((*it).error()) continue;
                    dstd::StringView src = it.source();
                    emit((*it).value_unsafe(), src);
                    covered = static_cast<std::size_t>(src.data() - base) +
                              src.size();
                }
            }
        }
        if (covered >= parse_len) break;

        // A bad record can desync the structural scan of the batch and
        // swallow the rest of it as one rejected document.
        std::size_t start = covered;
        if (start > pos) {  // resume at the line after the last record parsed
            std::size_t rest = start;
            while (start < parse_len && base[start] != '\n') ++start;
            if constexpr (HAS_REFUSED) {
                // More records after the last one parsed, on its line.
                while (rest < start &&
                       (base[rest] == ' ' || base[rest] == '\t' ||
                        base[rest] == '\r' || base[rest] == ','))
                    ++rest;
                if (rest < start)
                    hand_over_refused(base, len, rest, start, refused);
            }
            if (start < parse_len) ++start;
        }
        std::size_t end = start;
        while (end < parse_len && base[end] != '\n') ++end;
        const bool long_line = end - start > PARSE_BATCH_BYTES / 2;
        if (long_line) {
            if (end > start) {
                auto r = parser.parse(base + start, end - start);
                if (!r.error())
                    emit(r.value_unsafe(),
                         dstd::StringView(base + start, end - start));
                else if constexpr (HAS_REFUSED)
                    hand_over_refused(base, len, start, end, refused);
            }
            pos = end + 1;
            continue;
        }
        // Re-parse whatever it did not reach, line by line, so a malformed
        // record drops only itself.
        for (std::size_t at = start; at < parse_len;) {
            std::size_t stop = at;
            while (stop < parse_len && base[stop] != '\n') ++stop;
            if (stop > at) {
                auto r = parser.parse(base + at, stop - at);
                if (!r.error())
                    emit(r.value_unsafe(),
                         dstd::StringView(base + at, stop - at));
                else if constexpr (HAS_REFUSED)
                    hand_over_refused(base, len, at, stop, refused);
            }
            at = stop + 1;
        }
        break;
    }

    return len - parse_len;
}

template <typename Emit>
std::size_t parse_lines(simdjson::dom::parser& parser, const char* base,
                        std::size_t len, Emit&& emit) {
    return parse_lines(parser, base, len, std::forward<Emit>(emit),
                       NoRefused{});
}

/// Iterate parsed dftracer events from one inflated buffer (NDJSON plus
/// simdjson padding; the caller strips any "[" / "]" delimiter lines).
/// `chunk_buffer` is held shared so an @ref EventRecord's string_view can
/// outlive the loop. Args stay in the record's DOM element. Returns the
/// trailing incomplete line for the caller to carry into the next buffer.
///
/// Parsed only through the last newline, not by parse_many's own truncation
/// (which reads zero when a chunk is cut mid-string, leaving the next chunk to
/// start mid-string and be rejected whole). parse_many derives all boundaries
/// from one scan of the batch, so a single unescaped character in a string
/// collapses it into one rejected document; on that (nothing delivered) the
/// chunk is re-parsed line by line so a bad record drops only itself.
template <typename Cb>
std::size_t parse_buffer(simdjson::dom::parser& parser,
                         dstd::SharedPtr<dstd::String> chunk_buffer,
                         std::size_t len, std::size_t checkpoint_idx,
                         std::size_t& line_number, Cb&& cb) {
    if (!chunk_buffer || len == 0) return 0;
    return parse_lines(
        parser, chunk_buffer->data(), len,
        [&](simdjson::dom::element root, dstd::StringView src) {
            if (!root.is_object()) return;
            json::JsonValue json(root);
            DFTracerEvent ev;
            simdjson::dom::element args_dom{};
            bool has_args = false;
            if (!DFTracerEvent::parse_scalars(root, ev, args_dom, has_args))
                return;
            std::size_t ln = line_number++;
            EventRecord record{ev,           json,           src,
                               chunk_buffer, checkpoint_idx, ln,
                               args_dom,     has_args};
            cb(record);
        });
}

}  // namespace dftracer::utils::trace

#endif  // DFTRACER_UTILS_TRACE_PARSE_INFLATED_H
