#ifndef DFTRACER_UTILS_UTILITIES_TRACE_GEN_FAKE_TRACE_WRITER_H
#define DFTRACER_UTILS_UTILITIES_TRACE_GEN_FAKE_TRACE_WRITER_H

#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>
#include <dftracer/utils/utilities/fileio/streaming_file_writer_utility.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

// Synthetic chrome/Perfetto trace emission (gzip .pfw.gz). A production
// utility for generating trace files with known patterns; consumed by the
// dftracer_gen_fake_trace binary and available to tests that need fixtures.
namespace dftracer::utils::utilities::trace_gen {

// ---------------------------------------------------------------------------
// TraceWriter - compresses via ManualStreamingCompressorUtility and writes
//               via StreamingFileWriterUtility.  Natural deflate blocks
//               provide block boundaries for the gzip indexer.
// ---------------------------------------------------------------------------
class TraceWriter {
   public:
    explicit TraceWriter(const std::string& path,
                         std::size_t flush_threshold = 4 * 1024 * 1024)
        : writer_(path), flush_threshold_(flush_threshold) {
        buf_.reserve(flush_threshold * 2);
    }

    ~TraceWriter() { close(); }

    TraceWriter(const TraceWriter&) = delete;
    TraceWriter& operator=(const TraceWriter&) = delete;

    void write(const std::string& s) {
        buf_ += s;
        if (buf_.size() >= flush_threshold_) {
            flush();
        }
    }

    void flush() {
        if (buf_.empty()) return;
        [this]() -> coro::CoroTask<void> {
            auto gen = compressor_.compress(ByteView(buf_));
            while (auto chunk = co_await gen.next()) {
                co_await writer_.process(*chunk);
            }
        }()
                        .get();
        buf_.clear();
    }

    void close() {
        flush();
        [this]() -> coro::CoroTask<void> {
            auto gen = compressor_.finalize_stream();
            while (auto chunk = co_await gen.next()) {
                co_await writer_.process(*chunk);
            }
        }()
                        .get();
        writer_.close();
    }

   private:
    compression::zlib::ManualStreamingCompressorUtility compressor_;
    fileio::StreamingFileWriterUtility writer_;
    std::string buf_;
    std::size_t flush_threshold_;
};

// Deterministic 16-char hex hash using the project's HasherUtility
inline std::string make_hash(const std::string& name) {
    hash::HasherUtility hasher;
    hasher.reset();
    auto h = hasher.process(name).get();
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(h.value));
    return std::string(buf);
}

// +/-30% random variance around base
inline std::uint64_t jitter(std::mt19937_64& rng, std::uint64_t base) {
    std::uniform_real_distribution<double> dist(0.7, 1.3);
    return static_cast<std::uint64_t>(static_cast<double>(base) * dist(rng));
}

// Escape a JSON string value (no surrounding quotes)
inline void json_escape(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            default:
                out += c;
                break;
        }
    }
}

// Metadata event: {"name":"HH"/"FH"/"SH","ph":"M",
//                  "args":{"hhash":"...","name":"...","value":"..."}}
inline void emit_metadata(TraceWriter& w, const std::string& kind,
                          const std::string& hhash,
                          const std::string& resolved_name,
                          const std::string& hash_value) {
    std::string buf;
    buf.reserve(256);
    buf += R"({"name":")";
    buf += kind;
    buf += R"(","ph":"M","args":{"hhash":")";
    json_escape(buf, hhash);
    buf += R"(","name":")";
    json_escape(buf, resolved_name);
    buf += R"(","value":")";
    json_escape(buf, hash_value);
    buf += R"("}})";
    buf += '\n';
    w.write(buf);
}

// Regular event (duration, ph=X)
struct EventArgs {
    std::uint64_t id = 0;
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    std::string name;
    std::string cat;
    std::uint64_t ts = 0;
    std::uint64_t dur = 0;
    int level = 0;
    std::string hhash;
    std::string fhash;
    std::string cmd_hash;
    // Optional extra args appended verbatim (no leading comma)
    std::string extra;
};

inline void emit_event(TraceWriter& w, const EventArgs& a) {
    char num_buf[128];
    std::string buf;
    buf.reserve(512);

    buf += R"({"id":)";
    std::snprintf(num_buf, sizeof(num_buf), "%llu",
                  static_cast<unsigned long long>(a.id));
    buf += num_buf;

    buf += R"(,"name":")";
    json_escape(buf, a.name);
    buf += R"(","cat":")";
    json_escape(buf, a.cat);

    std::snprintf(num_buf, sizeof(num_buf),
                  R"(","pid":%llu,"tid":%llu,"ts":%llu,"dur":%llu,"ph":"X")",
                  static_cast<unsigned long long>(a.pid),
                  static_cast<unsigned long long>(a.tid),
                  static_cast<unsigned long long>(a.ts),
                  static_cast<unsigned long long>(a.dur));
    buf += num_buf;

    buf += R"(,"args":{)";

    buf += R"("hhash":")";
    json_escape(buf, a.hhash);
    buf += '"';

    std::snprintf(num_buf, sizeof(num_buf), R"(,"level":%d)", a.level);
    buf += num_buf;

    if (!a.fhash.empty()) {
        buf += R"(,"fhash":")";
        json_escape(buf, a.fhash);
        buf += '"';
    }
    if (!a.cmd_hash.empty()) {
        buf += R"(,"cmd_hash":")";
        json_escape(buf, a.cmd_hash);
        buf += '"';
    }
    if (!a.extra.empty()) {
        buf += ',';
        buf += a.extra;
    }

    buf += R"(}})";
    buf += '\n';
    w.write(buf);
}

}  // namespace dftracer::utils::utilities::trace_gen

#endif  // DFTRACER_UTILS_UTILITIES_TRACE_GEN_FAKE_TRACE_WRITER_H
