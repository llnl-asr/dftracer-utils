#ifndef DFTRACER_UTILS_UTILITIES_TRACE_GEN_FAKE_TRACE_WRITER_H
#define DFTRACER_UTILS_UTILITIES_TRACE_GEN_FAKE_TRACE_WRITER_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <vector>

/// Synthetic chrome/Perfetto trace emission (gzip .pfw.gz). A production
/// utility for generating trace files with known patterns; consumed by the
/// dftracer_gen_fake_trace binary and available to tests that need fixtures.
namespace dftracer::utils::utilities::trace_gen {

// ---------------------------------------------------------------------------
// TraceWriter - appends whole lines to a GzipLineWriterBlocking, so the output
//               is multi-member (parallel-inflatable/indexable) with members
//               of about flush_threshold bytes.
// ---------------------------------------------------------------------------
class TraceWriter {
   public:
    explicit TraceWriter(const std::string& path,
                         std::size_t flush_threshold = 4 * 1024 * 1024)
        : flush_threshold_(flush_threshold) {
        fileio::GzipWriterOptions opts;
        opts.member_size = flush_threshold;
        writer_.emplace(unwrap(
            fileio::GzipLineWriterBlocking::open(path, std::move(opts))));
        buf_.reserve(flush_threshold * 2);
    }

    ~TraceWriter() {
        try {
            close();
        } catch (...) {
        }
    }

    TraceWriter(const TraceWriter&) = delete;
    TraceWriter& operator=(const TraceWriter&) = delete;

    /// `s` must hold whole lines.
    void write(const std::string& s) {
        buf_ += s;
        if (buf_.size() >= flush_threshold_) drain();
    }

    void flush() {
        drain();
        unwrap(writer_->cut());
    }

    void close() {
        if (!writer_) return;
        drain();
        auto w = std::move(*writer_);
        writer_.reset();
        unwrap(w.close());
    }

   private:
    void drain() {
        if (buf_.empty()) return;
        unwrap(writer_->append(buf_));
        buf_.clear();
    }

    std::optional<fileio::GzipLineWriterBlocking> writer_;
    std::string buf_;
    std::size_t flush_threshold_;
};

/// Deterministic 16-char hex hash using the project's HasherUtility
inline std::string make_hash(const std::string& name) {
    hash::HasherUtility hasher;
    hasher.reset();
    auto h = hasher.process(name);
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(h.value));
    return std::string(buf);
}

/// +/-30% random variance around base
inline std::uint64_t jitter(std::mt19937_64& rng, std::uint64_t base) {
    std::uniform_real_distribution<double> dist(0.7, 1.3);
    return static_cast<std::uint64_t>(static_cast<double>(base) * dist(rng));
}

/// Escape a JSON string value (no surrounding quotes)
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

namespace dft = trace;

/// Metadata event: {"name":"HH"/"FH"/"SH","ph":4,"type":1,
///                  "args":{"hhash":"...","name":"...","value":"..."}}
/// Hash and tracer-bookkeeping metadata is the DFTRACER layer.
inline void emit_metadata(TraceWriter& w, const std::string& kind,
                          const std::string& hhash,
                          const std::string& resolved_name,
                          const std::string& hash_value) {
    static constexpr int PH_M = dft::phase_to_int(dft::RecordPhase::METADATA);
    static constexpr int TY_DFT =
        dft::event_type_to_int(dft::EventType::DFTRACER);
    std::string buf;
    buf.reserve(256);
    buf += R"({"name":")";
    buf += kind;
    buf += R"(","ph":)";
    buf += std::to_string(PH_M);
    buf += R"(,"type":)";
    buf += std::to_string(TY_DFT);
    buf += R"(,"args":{"hhash":")";
    json_escape(buf, hhash);
    buf += R"(","name":")";
    json_escape(buf, resolved_name);
    buf += R"(","value":")";
    json_escape(buf, hash_value);
    buf += R"("}})";
    buf += '\n';
    w.write(buf);
}

/// Regular complete event (duration, ph=1)
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
    /// Instrumentation layer. Unknown lets emit_event infer it from cat.
    dft::EventType type = dft::EventType::UNKNOWN;
    /// Optional extra args appended verbatim (no leading comma)
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

    dft::EventType ty = a.type != dft::EventType::UNKNOWN
                            ? a.type
                            : dft::event_type_from_cat(a.cat);
    std::snprintf(
        num_buf, sizeof(num_buf),
        R"(","pid":%llu,"tid":%llu,"ts":%llu,"dur":%llu,"ph":%d,"type":%d)",
        static_cast<unsigned long long>(a.pid),
        static_cast<unsigned long long>(a.tid),
        static_cast<unsigned long long>(a.ts),
        static_cast<unsigned long long>(a.dur),
        dft::phase_to_int(dft::RecordPhase::COMPLETE),
        dft::event_type_to_int(ty));
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
