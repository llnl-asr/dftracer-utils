// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_TRACE_GEN_FAKE_TRACE_WRITER_H
#define DFTRACER_UTILS_UTILITIES_TRACE_GEN_FAKE_TRACE_WRITER_H

#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/json/fmt.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <cstdint>
#include <random>

/// Synthetic chrome/Perfetto trace emission (gzip .pfw.gz). A production
/// utility for generating trace files with known patterns; consumed by the
/// dftracer_gen_fake_trace binary and available to tests that need fixtures.
namespace dftracer::utils::utilities::trace_gen {

// ---------------------------------------------------------------------------
// TraceWriter - appends whole lines to a GzipLineWriterBlocking, so the output
//               is multi-member (parallel-inflatable/indexable) with members
//               of about flush_threshold bytes. The first open or write error
//               is kept: later writes do nothing, and flush() and close()
//               return it.
// ---------------------------------------------------------------------------
class TraceWriter {
   public:
    explicit TraceWriter(dstd::StringView path,
                         std::size_t flush_threshold = 4 * 1024 * 1024)
        : flush_threshold_(flush_threshold) {
        fileio::GzipWriterOptions opts;
        opts.member_size = flush_threshold;
        auto opened = fileio::GzipLineWriterBlocking::open(dstd::String(path),
                                                           std::move(opts));
        if (!opened) {
            error_ = opened.error();
            return;
        }
        writer_.emplace(std::move(*opened));
        buf_.reserve(flush_threshold * 2);
    }

    ~TraceWriter() { close().discard(); }

    TraceWriter(const TraceWriter&) = delete;
    TraceWriter& operator=(const TraceWriter&) = delete;

    /// `s` must hold whole lines.
    void write(dstd::StringView s) {
        if (error_) return;
        buf_ += s;
        if (buf_.size() >= flush_threshold_) drain();
    }

    /// Writes the buffered lines and ends the gzip member.
    dstd::Result<void> flush() {
        drain();
        if (error_) return *error_;
        auto cut = writer_->cut();
        if (!cut) error_ = cut.error();
        if (error_) return *error_;
        return {};
    }

    /// Writes the buffered lines and closes the file. Safe to call twice.
    dstd::Result<void> close() {
        if (writer_) {
            drain();
            auto w = std::move(*writer_);
            writer_.reset();
            auto closed = w.close();
            if (!closed && !error_) error_ = closed.error();
        }
        if (error_) return *error_;
        return {};
    }

   private:
    void drain() {
        if (error_ || buf_.empty()) return;
        auto appended = writer_->append(buf_);
        if (!appended) error_ = appended.error();
        buf_.clear();
    }

    dstd::Optional<fileio::GzipLineWriterBlocking> writer_;
    dstd::Optional<dstd::Error> error_;
    dstd::String buf_;
    std::size_t flush_threshold_;
};

/// Deterministic 16-char hex hash using the project's HasherUtility
inline dstd::String make_hash(dstd::StringView name) {
    hash::HasherUtility hasher;
    hasher.reset();
    auto h = hasher.process(name);
    char buf[16];
    const auto text = dstd::fmt_to<"{:016x}">(buf, sizeof(buf), h.value);
    return dstd::String(buf, text.size);
}

/// +/-30% random variance around base
inline std::uint64_t jitter(std::mt19937_64& rng, std::uint64_t base) {
    std::uniform_real_distribution<double> dist(0.7, 1.3);
    return static_cast<std::uint64_t>(static_cast<double>(base) * dist(rng));
}

/// Metadata event: {"name":"HH"/"FH"/"SH","ph":4,"type":1,
///                  "args":{"hhash":"...","name":"...","value":"..."}}
/// Hash and tracer-bookkeeping metadata is the DFTRACER layer.
inline void emit_metadata(TraceWriter& w, dstd::StringView kind,
                          dstd::StringView hhash,
                          dstd::StringView resolved_name,
                          dstd::StringView hash_value) {
    static constexpr int PH_M =
        trace::phase_to_int(trace::RecordPhase::Metadata);
    static constexpr int TY_DFT =
        trace::event_type_to_int(trace::EventType::Dftracer);
    dstd::String buf = json::fmt<
        R"({"name":"{}","ph":{},"type":{},"args":{"hhash":"{}","name":"{}",)"
        R"("value":"{}"}})"
        "\n">(dstd::fmt_detail::RawText{kind}, PH_M, TY_DFT, hhash,
              resolved_name, hash_value);
    w.write(buf);
}

/// Regular complete event (duration, ph=1)
struct EventArgs {
    std::uint64_t id = 0;
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    dstd::String name;
    dstd::String cat;
    std::uint64_t ts = 0;
    std::uint64_t dur = 0;
    int level = 0;
    dstd::String hhash;
    dstd::String fhash;
    dstd::String cmd_hash;
    /// Instrumentation layer. Unknown lets emit_event infer it from cat.
    trace::EventType type = trace::EventType::Unknown;
    /// Optional extra args appended verbatim (no leading comma)
    dstd::String extra;
};

inline void emit_event(TraceWriter& w, const EventArgs& a) {
    const trace::EventType ty = a.type != trace::EventType::Unknown
                                    ? a.type
                                    : trace::event_type_from_cat(a.cat);
    dstd::String buf = json::fmt<
        R"({"id":{},"name":"{}","cat":"{}","pid":{},"tid":{},"ts":{},"dur":{},)"
        R"("ph":{},"type":{},"args":{"hhash":"{}","level":{})">(
        a.id, a.name, a.cat, a.pid, a.tid, a.ts, a.dur,
        trace::phase_to_int(trace::RecordPhase::Complete),
        trace::event_type_to_int(ty), a.hhash, a.level);

    if (!a.fhash.empty()) buf += json::fmt<R"(,"fhash":"{}")">(a.fhash);
    if (!a.cmd_hash.empty())
        buf += json::fmt<R"(,"cmd_hash":"{}")">(a.cmd_hash);
    if (!a.extra.empty()) {
        buf += ',';
        buf += a.extra;
    }

    buf += "}}\n";
    w.write(buf);
}

}  // namespace dftracer::utils::utilities::trace_gen

#endif  // DFTRACER_UTILS_UTILITIES_TRACE_GEN_FAKE_TRACE_WRITER_H
