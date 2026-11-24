// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/trace/writer/trace_writer.h>

#include <cmath>
#include <utility>

namespace dftracer::utils::trace::writer {

namespace {

constexpr int PH_EVENT = phase_to_int(RecordPhase::Complete);
constexpr int PH_COUNTER = phase_to_int(RecordPhase::Counter);
constexpr int PH_AGGREGATED = phase_to_int(RecordPhase::Aggregated);
constexpr int PH_METADATA = phase_to_int(RecordPhase::Metadata);

void write_value(json::Writer& w, const ArgValue& v) {
    switch (v.kind) {
        case ArgValue::Kind::Null:
            w.null();
            break;
        case ArgValue::Kind::Bool:
            w.value(v.b);
            break;
        case ArgValue::Kind::Int:
            w.value(v.i);
            break;
        case ArgValue::Kind::UInt:
            w.value(v.u);
            break;
        case ArgValue::Kind::Double:
            write_double(w, v.d);
            break;
        case ArgValue::Kind::String:
            w.value(v.s);
            break;
        case ArgValue::Kind::Raw:
            w.raw_value(v.s);
            break;
    }
}

void write_id(json::Writer& w, const RecordExtras& extras) {
    if (extras.has_id) w.member("id", extras.id);
}

void write_type(json::Writer& w, const RecordExtras& extras) {
    if (extras.type != EventType::Unknown)
        w.member("type", event_type_to_int(extras.type));
}

void write_args(json::Writer& w, Args args) {
    if (args.empty()) return;
    w.key("args");
    w.begin_object();
    for (const Arg& a : args) {
        w.key(a.key);
        write_value(w, a.value);
    }
    w.end_object();
}

}  // namespace

void write_double(json::Writer& w, double v) {
    if (std::isnan(v))
        w.value("NaN");
    else if (std::isinf(v))
        w.value(dstd::StringView(v > 0 ? "Infinity" : "-Infinity"));
    else
        w.value(v);
}

TraceWriter::TraceWriter(fileio::GzipLineWriterBlocking writer,
                         std::size_t drain_size)
    : drain_size_(drain_size) {
    writer_.emplace(std::move(writer));
}

TraceWriter::TraceWriter(TraceWriter&&) noexcept = default;
TraceWriter& TraceWriter::operator=(TraceWriter&&) noexcept = default;
TraceWriter::~TraceWriter() = default;

dstd::Result<TraceWriter> TraceWriter::open(dstd::StringView path,
                                            TraceWriterOptions opts) {
    fileio::GzipWriterOptions gz;
    gz.member_size = opts.member_size;
    gz.level = opts.level;
    gz.workers = opts.workers;
    gz.compress = opts.compress;
    auto opened =
        fileio::GzipLineWriterBlocking::open(dstd::String(path), std::move(gz));
    if (!opened) return opened.error();
    return TraceWriter(std::move(*opened), opts.member_size);
}

bool TraceWriter::needs_drain() const { return buf_.size() >= drain_size_; }

dstd::Result<void> TraceWriter::drain() {
    if (error_) return *error_;
    if (closed_ || buf_.empty()) return {};
    auto appended = writer_->append(buf_);
    buf_.clear();
    if (!appended) {
        error_ = appended.error();
        return *error_;
    }
    return {};
}

dstd::Result<void> TraceWriter::begin_line() {
    if (error_) return *error_;
    if (closed_)
        return common::make_error(common::ErrorCode::InvalidArgument,
                                  "trace writer is closed");
    if (needs_drain()) return drain();
    return {};
}

void format_event(dstd::String& out, dstd::StringView name,
                  dstd::StringView cat, std::uint64_t ts, std::uint64_t dur,
                  std::uint64_t pid, std::uint64_t tid, Args args,
                  RecordExtras extras) {
    {
        json::Writer w(out);
        w.begin_object();
        write_id(w, extras);
        w.member("name", name);
        w.member("cat", cat);
        write_type(w, extras);
        w.member("pid", pid);
        w.member("tid", tid);
        w.member("ts", ts);
        w.member("dur", dur);
        w.member("ph", PH_EVENT);
        write_args(w, args);
        w.end_object();
    }
    out += '\n';
}

void format_counter(dstd::String& out, dstd::StringView name,
                    dstd::StringView cat, std::uint64_t ts, std::uint64_t pid,
                    std::uint64_t tid, Args args, RecordExtras extras) {
    {
        json::Writer w(out);
        w.begin_object();
        write_id(w, extras);
        w.member("name", name);
        w.member("cat", cat);
        write_type(w, extras);
        w.member("ts", ts);
        w.member("ph", PH_COUNTER);
        w.member("pid", pid);
        w.member("tid", tid);
        write_args(w, args);
        w.end_object();
    }
    out += '\n';
}

void format_aggregated(dstd::String& out, dstd::StringView name,
                       dstd::StringView cat, std::uint64_t ts,
                       std::uint64_t pid, std::uint64_t tid, Args args,
                       RecordExtras extras) {
    {
        json::Writer w(out);
        w.begin_object();
        write_id(w, extras);
        w.member("name", name);
        w.member("cat", cat);
        write_type(w, extras);
        w.member("ts", ts);
        w.member("ph", PH_AGGREGATED);
        w.member("pid", pid);
        w.member("tid", tid);
        write_args(w, args);
        w.end_object();
    }
    out += '\n';
}

void format_metadata(dstd::String& out, dstd::StringView name,
                     std::uint64_t pid, std::uint64_t tid, Args args,
                     RecordExtras extras) {
    {
        json::Writer w(out);
        w.begin_object();
        write_id(w, extras);
        w.member("name", name);
        write_type(w, extras);
        w.member("pid", pid);
        w.member("tid", tid);
        w.member("ts", std::uint64_t{0});
        w.member("ph", PH_METADATA);
        write_args(w, args);
        w.end_object();
    }
    out += '\n';
}

dstd::Result<void> TraceWriter::write_event(dstd::StringView name,
                                            dstd::StringView cat,
                                            std::uint64_t ts, std::uint64_t dur,
                                            std::uint64_t pid,
                                            std::uint64_t tid, Args args,
                                            RecordExtras extras) {
    if (auto begun = begin_line(); !begun) return begun.error();
    format_event(buf_, name, cat, ts, dur, pid, tid, args, extras);
    return {};
}

dstd::Result<void> TraceWriter::write_counter(
    dstd::StringView name, dstd::StringView cat, std::uint64_t ts,
    std::uint64_t pid, std::uint64_t tid, Args args, RecordExtras extras) {
    if (auto begun = begin_line(); !begun) return begun.error();
    format_counter(buf_, name, cat, ts, pid, tid, args, extras);
    return {};
}

dstd::Result<void> TraceWriter::write_aggregated(
    dstd::StringView name, dstd::StringView cat, std::uint64_t ts,
    std::uint64_t pid, std::uint64_t tid, Args args, RecordExtras extras) {
    if (auto begun = begin_line(); !begun) return begun.error();
    format_aggregated(buf_, name, cat, ts, pid, tid, args, extras);
    return {};
}

dstd::Result<void> TraceWriter::write_metadata(dstd::StringView name,
                                               std::uint64_t pid,
                                               std::uint64_t tid, Args args,
                                               RecordExtras extras) {
    if (auto begun = begin_line(); !begun) return begun.error();
    format_metadata(buf_, name, pid, tid, args, extras);
    return {};
}

dstd::Result<void> TraceWriter::write_raw(dstd::StringView lines) {
    if (error_) return *error_;
    if (closed_)
        return common::make_error(common::ErrorCode::InvalidArgument,
                                  "trace writer is closed");
    if (lines.empty()) return {};
    if (lines.back() != '\n')
        return common::make_error(common::ErrorCode::InvalidArgument,
                                  "raw lines must end with a newline");
    if (lines.size() < drain_size_) {
        buf_ += lines;
        if (needs_drain()) return drain();
        return {};
    }
    if (auto drained = drain(); !drained) return drained;
    auto appended = writer_->append(lines);
    if (!appended) {
        error_ = appended.error();
        return *error_;
    }
    return {};
}

dstd::Result<void> TraceWriter::close() {
    if (!closed_) {
        drain().discard();
        closed_ = true;
        auto closed = writer_->close();
        writer_.reset();
        if (!closed && !error_) error_ = closed.error();
    }
    if (error_) return *error_;
    return {};
}

}  // namespace dftracer::utils::trace::writer
