#include <dftracer/utils/trace/event.h>
#include <dftracer/utils/trace/trace_config.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <simdjson.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::trace {

namespace {

constexpr std::size_t INITIAL_TAIL = 1u << 20;  // 1 MiB

// The last 4 bytes are the final member's uncompressed size; a backward scan
// for the gzip magic finds its start, validated by decompressing to exactly
// that size. Window grows until the member fits or the whole file is scanned.
bool decompress_last_member(std::ifstream& in, std::streamoff fsize,
                            std::vector<std::uint8_t>& out) {
    unsigned char isz[4];
    in.seekg(fsize - 4);
    in.read(reinterpret_cast<char*>(isz), 4);
    if (in.gcount() != 4) return false;
    const std::uint32_t isize = static_cast<std::uint32_t>(isz[0]) |
                                (static_cast<std::uint32_t>(isz[1]) << 8) |
                                (static_cast<std::uint32_t>(isz[2]) << 16) |
                                (static_cast<std::uint32_t>(isz[3]) << 24);

    utilities::fileio::compress::GzipMemberDecompressor dec;
    for (std::size_t window = INITIAL_TAIL;; window *= 4) {
        const std::size_t winlen =
            std::min<std::size_t>(window, static_cast<std::size_t>(fsize));
        std::vector<unsigned char> buf(winlen);
        in.seekg(fsize - static_cast<std::streamoff>(winlen));
        in.read(reinterpret_cast<char*>(buf.data()),
                static_cast<std::streamsize>(winlen));
        if (static_cast<std::size_t>(in.gcount()) != winlen) return false;

        for (std::size_t i = winlen >= 3 ? winlen - 3 : 0;; --i) {
            if (buf[i] == 0x1f && buf[i + 1] == 0x8b && buf[i + 2] == 0x08) {
                auto r =
                    dec.decompress_member(buf.data() + i, winlen - i, isize);
                if (r && r->size() == isize) {
                    out = std::move(*r);
                    return true;
                }
            }
            if (i == 0) break;
        }
        if (winlen >= static_cast<std::size_t>(fsize)) return false;
    }
}

// Decompress the file's first gzip member (the header, where CM lives). Reads a
// bounded prefix, finds the member's end at the next gzip magic (or EOF for a
// small single-member file), and decompresses it. Best-effort: fails (caller
// defaults to US) for a single-member file larger than the prefix.
bool decompress_first_member(std::ifstream& in, std::streamoff fsize,
                             std::vector<std::uint8_t>& out) {
    const std::size_t cap =
        std::min<std::size_t>(INITIAL_TAIL, static_cast<std::size_t>(fsize));
    std::vector<unsigned char> buf(cap);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(buf.data()),
            static_cast<std::streamsize>(cap));
    const std::size_t got = static_cast<std::size_t>(in.gcount());
    if (got < 18 || buf[0] != 0x1f || buf[1] != 0x8b || buf[2] != 0x08)
        return false;

    std::size_t end = got;  // member end: next magic, else EOF (if fully read)
    for (std::size_t i = 18; i + 3 <= got; ++i) {
        if (buf[i] == 0x1f && buf[i + 1] == 0x8b && buf[i + 2] == 0x08) {
            end = i;
            break;
        }
    }
    if (end == got && got != static_cast<std::size_t>(fsize))
        return false;  // single member bigger than our prefix
    if (end < 8) return false;
    const std::uint32_t isize =
        static_cast<std::uint32_t>(buf[end - 4]) |
        (static_cast<std::uint32_t>(buf[end - 3]) << 8) |
        (static_cast<std::uint32_t>(buf[end - 2]) << 16) |
        (static_cast<std::uint32_t>(buf[end - 1]) << 24);

    utilities::fileio::compress::GzipMemberDecompressor dec;
    auto r = dec.decompress_member(buf.data(), end, isize);
    if (r && r->size() == isize) {
        out = std::move(*r);
        return true;
    }
    return false;
}

}  // namespace

TimeMetric read_time_metric(const std::string& trace_path) {
    std::ifstream in(trace_path, std::ios::binary | std::ios::ate);
    if (!in) return TimeMetric::Us;
    const std::streamoff fsize = in.tellg();
    if (fsize < 18) return TimeMetric::Us;

    std::vector<std::uint8_t> uc;
    if (!decompress_first_member(in, fsize, uc)) return TimeMetric::Us;

    simdjson::dom::parser parser;
    std::size_t pos = 0, lines = 0;
    while (pos < uc.size() && lines < 256) {
        std::size_t nl = pos;
        while (nl < uc.size() && uc[nl] != '\n') ++nl;
        std::string_view line(reinterpret_cast<const char*>(uc.data() + pos),
                              nl - pos);
        // Only pay the parse for a candidate line.
        if (line.find("time_metric") != std::string_view::npos) {
            simdjson::padded_string padded(line);
            simdjson::dom::element doc;
            if (!parser.parse(padded).get(doc)) {
                auto args = doc["args"];
                if (!args.error()) {
                    std::string_view nm, val;
                    if (!args["name"].get_string().get(nm) &&
                        nm == "time_metric" &&
                        !args["value"].get_string().get(val))
                        return parse_time_metric(val);
                }
            }
        }
        pos = nl + 1;
        ++lines;
    }
    return TimeMetric::Us;
}

std::optional<TraceConfig> parse_end_event(std::string_view line) {
    static constexpr std::string_view END_MARK = "\"name\":\"end\"";
    if (line.empty() || line.find(END_MARK) == std::string_view::npos)
        return std::nullopt;
    thread_local simdjson::dom::parser parser;
    std::string padded(line);
    padded.resize(line.size() + simdjson::SIMDJSON_PADDING);
    auto doc = parser.parse(padded.data(), line.size(), false);
    if (doc.error()) return std::nullopt;
    DFTracerEvent ev;
    JsonValue jv(doc.value_unsafe());
    if (!DFTracerEvent::parse(jv, ev) || ev.name != "end") return std::nullopt;
    return TraceConfig{ev.pid, std::move(ev.args)};
}

std::vector<TraceConfig> read_trace_config(const std::string& trace_path) {
    std::vector<TraceConfig> out;
    std::ifstream in(trace_path, std::ios::binary | std::ios::ate);
    if (!in) return out;
    const std::streamoff fsize = in.tellg();
    if (fsize < 18) return out;  // smaller than an empty gzip member

    std::vector<std::uint8_t> uc;
    if (!decompress_last_member(in, fsize, uc)) return out;

    std::size_t pos = 0;
    while (pos < uc.size()) {
        std::size_t nl = pos;
        while (nl < uc.size() && uc[nl] != '\n') ++nl;
        std::string_view line(reinterpret_cast<const char*>(uc.data() + pos),
                              nl - pos);
        if (auto c = parse_end_event(line)) out.push_back(std::move(*c));
        pos = nl + 1;
    }
    return out;
}

}  // namespace dftracer::utils::trace
