// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_WRITER_TRACE_WRITER_H
#define DFTRACER_UTILS_TRACE_WRITER_TRACE_WRITER_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/fileio/gzip_line_writer.h>
#include <dftracer/utils/json/writer.h>
#include <dftracer/utils/trace/schema.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dftracer::utils::trace::writer {

/// One value of an args member. A string and a raw value borrow their text
/// until the record call returns.
struct ArgValue {
    enum class Kind : std::uint8_t {
        Null,
        Bool,
        Int,
        UInt,
        Double,
        String,
        Raw,  ///< JSON text written as is; the caller guarantees it is valid
    };

    Kind kind;
    union {
        bool b;
        std::int64_t i;
        std::uint64_t u;
        double d;
        dstd::StringView s;
    };

    constexpr ArgValue() : kind(Kind::Null), u(0) {}
    constexpr ArgValue(bool v) : kind(Kind::Bool), b(v) {}
    constexpr ArgValue(double v) : kind(Kind::Double), d(v) {}
    constexpr ArgValue(dstd::StringView v) : kind(Kind::String), s(v) {}
    template <std::size_t N>
    constexpr ArgValue(const char (&v)[N]) : kind(Kind::String), s(v) {}
    template <class T>
        requires(std::is_pointer_v<T>)
    ArgValue(T) = delete;
    template <class T>
        requires(std::is_integral_v<T> && std::is_signed_v<T> &&
                 !std::is_same_v<T, char>)
    constexpr ArgValue(T v)
        : kind(Kind::Int), i(static_cast<std::int64_t>(v)) {}
    template <class T>
        requires(std::is_integral_v<T> && std::is_unsigned_v<T> &&
                 !std::is_same_v<T, bool> && !std::is_same_v<T, char>)
    constexpr ArgValue(T v)
        : kind(Kind::UInt), u(static_cast<std::uint64_t>(v)) {}

    static constexpr ArgValue raw(dstd::StringView json) {
        ArgValue v;
        v.kind = Kind::Raw;
        v.s = json;
        return v;
    }
};

/// A member of the "args" object of a record.
struct Arg {
    dstd::StringView key;
    ArgValue value;
};

using Args = dstd::Span<const Arg>;

/// Top-level fields that only some records carry. `type` is omitted when
/// Unknown.
struct RecordExtras {
    std::uint64_t id = 0;
    bool has_id = false;
    EventType type = EventType::Unknown;
};

struct TraceWriterOptions {
    int level = 6;
    std::size_t workers = 0;  ///< 0: the machine's parallelism
    std::size_t member_size = 4 * 1024 * 1024;
    bool compress = true;     ///< false: a plain .pfw
};

/// A finite double as JSON text; NaN, +inf and -inf as the strings "NaN",
/// "Infinity" and "-Infinity", because JSON has no number for them.
void write_double(json::Writer& w, double v);

/// Append one record line, with its '\n', to `out`. These are the formats
/// that TraceWriter::write_event, write_counter, write_aggregated and
/// write_metadata write.
void format_event(dstd::String& out, dstd::StringView name,
                  dstd::StringView cat, std::uint64_t ts, std::uint64_t dur,
                  std::uint64_t pid, std::uint64_t tid, Args args = {},
                  RecordExtras extras = {});
void format_counter(dstd::String& out, dstd::StringView name,
                    dstd::StringView cat, std::uint64_t ts, std::uint64_t pid,
                    std::uint64_t tid, Args args = {},
                    RecordExtras extras = {});
void format_aggregated(dstd::String& out, dstd::StringView name,
                       dstd::StringView cat, std::uint64_t ts,
                       std::uint64_t pid, std::uint64_t tid, Args args = {},
                       RecordExtras extras = {});
void format_metadata(dstd::String& out, dstd::StringView name,
                     std::uint64_t pid, std::uint64_t tid, Args args = {},
                     RecordExtras extras = {});

/// Writes DFTracer records, one JSON line each, with "ph" as its integer.
/// Field order follows DFTracer: an event is
/// `id? name cat type? pid tid ts dur ph args?`, a counter or aggregated row
/// `name cat type? ts ph pid tid args?`, metadata
/// `name type? pid tid ts ph args?`. "args" is left out when `args` is empty.
///
/// Lines are buffered and appended in batches of about `member_size` bytes;
/// the file is gzip members of that size, or plain bytes with
/// TraceWriterOptions::compress false. The first error is kept: every later
/// call returns it, and close() returns it. A writer destroyed without
/// close() discards its output. Not thread-safe.
class TraceWriter {
   public:
    static dstd::Result<TraceWriter> open(dstd::StringView path,
                                          TraceWriterOptions opts = {});

    TraceWriter(TraceWriter&&) noexcept;
    TraceWriter& operator=(TraceWriter&&) noexcept;
    ~TraceWriter();

    /// ph 1.
    dstd::Result<void> write_event(dstd::StringView name, dstd::StringView cat,
                                   std::uint64_t ts, std::uint64_t dur,
                                   std::uint64_t pid, std::uint64_t tid,
                                   Args args = {}, RecordExtras extras = {});
    /// ph 2.
    dstd::Result<void> write_counter(dstd::StringView name,
                                     dstd::StringView cat, std::uint64_t ts,
                                     std::uint64_t pid, std::uint64_t tid,
                                     Args args = {}, RecordExtras extras = {});
    /// ph 3.
    dstd::Result<void> write_aggregated(dstd::StringView name,
                                        dstd::StringView cat, std::uint64_t ts,
                                        std::uint64_t pid, std::uint64_t tid,
                                        Args args = {},
                                        RecordExtras extras = {});
    /// ph 4, with ts 0.
    dstd::Result<void> write_metadata(dstd::StringView name, std::uint64_t pid,
                                      std::uint64_t tid, Args args = {},
                                      RecordExtras extras = {});

    /// Appends `lines` as they are; a batch of `member_size` bytes or more
    /// skips the buffer. A non-empty `lines` must end with '\n',
    /// otherwise the call fails with INVALID_ARGUMENT and the writer stays
    /// usable. Whole-file text such as a "[" line is the caller's to write.
    dstd::Result<void> write_raw(dstd::StringView lines);

    /// True when the next record call would write to the file; a caller that
    /// holds a lock around the writer can then drain first, outside it.
    bool needs_drain() const;

    /// Appends the buffered lines to the file.
    dstd::Result<void> drain();

    /// Writes the buffered lines and finishes the file. Safe to call twice;
    /// a record call after it fails with INVALID_ARGUMENT.
    dstd::Result<void> close();

   private:
    explicit TraceWriter(fileio::GzipLineWriterBlocking writer,
                         std::size_t drain_size);
    dstd::Result<void> begin_line();

    dstd::Optional<fileio::GzipLineWriterBlocking> writer_;
    dstd::Optional<dstd::Error> error_;
    dstd::String buf_;
    std::size_t drain_size_;
    bool closed_ = false;
};

}  // namespace dftracer::utils::trace::writer

#endif  // DFTRACER_UTILS_TRACE_WRITER_TRACE_WRITER_H
