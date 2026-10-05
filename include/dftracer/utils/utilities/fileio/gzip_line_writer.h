#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_GZIP_LINE_WRITER_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_GZIP_LINE_WRITER_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/fileio/json_line_format.h>
#include <dftracer/utils/utilities/fileio/line_format.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::fileio {

/// One member of the output, as the member table of an index build sees it.
/// `index` counts members across all parts; offsets and `first_line` (1-based)
/// restart in each part.
struct MemberInfo {
    std::size_t part;
    std::uint64_t index;
    std::uint64_t c_offset;
    std::uint64_t c_size;
    std::uint64_t uc_offset;
    std::uint64_t uc_size;
    std::uint64_t first_line;
    std::uint64_t lines;
};

/// Where a member lands. `index` counts members across all parts, `part_index`
/// within its part; unordered mode has one part, so `part_index == index`.
struct MemberRef {
    std::uint64_t index;
    std::size_t part;
    std::uint64_t part_index;
};

struct GzipWriterOptions {
    std::size_t member_size = constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    int level = 6;
    std::size_t workers = 0;    ///< 0: the machine's parallelism
    std::size_t part_size = 0;  ///< 0: one file
    bool compress = true;       ///< false: the same members as plain bytes
    /// Bytes the writer may hold: producer, shared and append_with buffers
    /// plus the plaintext and compressed buffers of in-flight members. 0 is
    /// auto (resolve_spill_budget(0)); NO_SPILL_BUDGET is no limit. A call
    /// that would pass the budget waits. The budget is kept in two halves,
    /// one for buffers being filled and one for in-flight members, and the
    /// writer holds at least one member of each, so a budget below one
    /// member plus its compressed bound is exceeded by that member, and one
    /// append batch or append_with `max_bytes` larger than its half by that
    /// call.
    std::uint64_t memory_budget = 0;
    /// false: each compress worker writes its own members, in no total order.
    /// on_member, on_part, part_size and part_name are refused at open;
    /// part_header and part_footer still bracket the file.
    bool ordered = true;
    /// Path of part `i`; empty uses gzip_part_path.
    std::function<std::string(std::size_t)> part_name;
    /// Each is a member of its own at the start / end of every part. Not
    /// lines, not counted in part_size or MemberInfo, not sent to on_member.
    std::string part_header;
    std::string part_footer;
    /// Once per part, after its file is closed. `uc_bytes` and `lines` count
    /// appended data only. Must not throw.
    std::function<void(std::size_t part, const std::string& path,
                       std::uint64_t uc_bytes, std::uint64_t lines)>
        on_part;
    /// In member order, on the writer step. Must not throw. Refused with
    /// ordered = false.
    std::function<void(const MemberInfo&)> on_member;
    /// Once per member on a compress worker, concurrently across members.
    /// `plaintext` is valid only during the call.
    std::function<void(const MemberRef& ref, std::string_view plaintext)> fold;
};

struct WriteSummary {
    std::size_t parts = 0;
    std::uint64_t members = 0;
    std::uint64_t lines = 0;
    std::uint64_t uc_bytes = 0;
    std::uint64_t c_bytes = 0;
    std::size_t peak_in_flight = 0;  ///< most members held at once
    std::uint64_t peak_bytes = 0;    ///< most bytes reserved against the budget
};

/// `<stem>-<idx>.pfw[.gz]` from a base like `<stem>.pfw[.gz]`; `base` itself
/// when `multi` is false.
std::string gzip_part_path(const std::string& base, int idx, bool multi);

/// Memory: the writer reserves bytes against `memory_budget` (see
/// GzipWriterOptions) and waits instead of growing past it. Reserved are one
/// shared buffer of member_size, each producer's pending bytes, each
/// unordered append_with call's `max_bytes`, and each in-flight member's
/// plaintext plus compressed bound. In-flight members are at most
/// min(workers + 2, budget / (member_size + compressed bound)), at least 1.
/// WriteSummary::peak_bytes is the most reserved at once. The shared append
/// path copies one caller batch outside the reservation.
///
/// Appended lines become gzip members of at least `member_size` bytes, cut at
/// the first '\n' at or after that size, compressed in parallel and written in
/// line order. At most workers + 2 members are held; append waits for a slot.
/// The first error is kept and returned by every later call, and the files this
/// writer created are removed. Call close() to finish; destroying a writer
/// without close() aborts it the same way (blocking until in-flight members
/// end). Not thread-safe, except that in unordered mode append, append_with
/// and cut may be called from several coroutines at once.
class GzipLineWriter {
   public:
    struct Impl;

    /// A producer's own member buffer, for unordered mode. Full members are
    /// submitted without touching any state shared with other producers; only
    /// the submit path is shared. One coroutine uses a Producer at a time, and
    /// it must not outlive the writer. flush() submits the pending remainder;
    /// close() fails (and removes the output) when any producer still holds
    /// unflushed bytes.
    /// A producer holds a reservation while its buffer is non-empty, at most
    /// the pending bytes rounded up to 1 KiB, and waits for one when the
    /// budget is spent. Its buffer stays under member_size + one line unless
    /// one call appends more.
    class Producer {
       public:
        Producer(Producer&&) noexcept;
        Producer& operator=(Producer&&) noexcept;
        ~Producer();

        /// Like GzipLineWriter::append, into this producer's buffer.
        coro::CoroTask<Result<void>> append(std::string_view lines);

        /// Like GzipLineWriter::append_with, formatting at the tail of this
        /// producer's buffer.
        coro::CoroTask<Result<void>> append_with(
            std::size_t max_bytes,
            std::function<std::size_t(char*, std::size_t)> fill);

        /// append_with of one line; the call adds the '\n', so a format has
        /// none. `Fmt` is a compile-time format (a wrong argument count or type
        /// does not compile); `fmt` a parsed format (a wrong count returns
        /// INVALID_ARGUMENT). append_fmt copies string arguments, append_json
        /// JSON-escapes them; raw() bypasses both. A null `const char*`
        /// argument returns INVALID_ARGUMENT. Arguments are read until the call
        /// returns, so temporaries in the awaiting expression are fine.
        template <line_format::FixedString Fmt, line_format::LineArg... A>
            requires(Fmt.line_safe() &&
                     sizeof...(A) == line_format::detail::HOLES<Fmt>)
        coro::CoroTask<Result<void>> append_fmt(const A&... args) {
            co_return co_await append_view<line_format::PlainEscape>(
                line_format::view<Fmt>(), args...);
        }
        template <line_format::LineArg... A>
        coro::CoroTask<Result<void>> append_fmt(
            const line_format::LineFormat& fmt, const A&... args) {
            co_return co_await append_view<line_format::PlainEscape>(fmt.view(),
                                                                     args...);
        }
        template <line_format::FixedString Fmt, line_format::LineArg... A>
            requires(Fmt.line_safe() &&
                     sizeof...(A) == line_format::detail::HOLES<Fmt>)
        coro::CoroTask<Result<void>> append_json(const A&... args) {
            co_return co_await append_view<line_format::JsonEscape>(
                line_format::view<Fmt>(), args...);
        }
        template <line_format::LineArg... A>
        coro::CoroTask<Result<void>> append_json(
            const line_format::JsonLineFormat& fmt, const A&... args) {
            co_return co_await append_view<line_format::JsonEscape>(fmt.view(),
                                                                    args...);
        }

        template <class E, line_format::LineArg... A>
        coro::CoroTask<Result<void>> append_view(line_format::detail::View f,
                                                 const A&... args) {
            if (f.holes != sizeof...(A))
                co_return make_error(ErrorCode::INVALID_ARGUMENT,
                                     "line format argument count mismatch");
            if (line_format::any_null(args...))
                co_return make_error(ErrorCode::INVALID_ARGUMENT,
                                     "null string argument");
            co_return co_await append_with(
                line_format::max_line_bytes<E>(f, args...),
                [&](char* dst, std::size_t) {
                    return line_format::format_line<E>(f, dst, args...);
                });
        }

        /// Submits the pending bytes as a member; no-op when empty.
        coro::CoroTask<Result<void>> flush();

       private:
        friend class GzipLineWriter;
        explicit Producer(Impl* impl);
        coro::CoroTask<void> reserve_for(std::size_t extra);
        coro::CoroTask<Result<void>> submit_full();
        void release_extra();
        Impl* impl_;
        std::string buf_;
        std::size_t units_ = 0;  ///< reservation held, in 1 KiB units
    };

    /// A new producer. Fails with INVALID_ARGUMENT in ordered mode.
    Result<Producer> producer();

    static coro::CoroTask<Result<GzipLineWriter>> open(std::string path,
                                                       GzipWriterOptions opts);

    /// Empty; only open() produces a usable writer.
    GzipLineWriter();
    GzipLineWriter(GzipLineWriter&&) noexcept;
    GzipLineWriter& operator=(GzipLineWriter&&) noexcept;
    ~GzipLineWriter();

    /// Copies `lines`. A non-empty span must end with '\n'; otherwise the call
    /// fails with INVALID_ARGUMENT and the writer stays usable.
    coro::CoroTask<Result<void>> append(std::string_view lines);

    /// Formats lines in place. `fill(dst, cap)` writes whole lines to `dst`
    /// (cap >= max_bytes) and returns the bytes written; a non-empty span must
    /// end with '\n', otherwise the call fails like append. Cuts as append
    /// does. In unordered mode the call fills a member of its own and submits
    /// it, so concurrent calls share no state.
    coro::CoroTask<Result<void>> append_with(
        std::size_t max_bytes,
        std::function<std::size_t(char*, std::size_t)> fill);

    /// append_with of one line; the call adds the '\n', so a format has none.
    /// `Fmt` is a compile-time format (a wrong argument count or type does not
    /// compile); `fmt` a parsed format (a wrong count returns
    /// INVALID_ARGUMENT). append_fmt copies string arguments, append_json
    /// JSON-escapes them; raw() bypasses both. A null `const char*` argument
    /// returns INVALID_ARGUMENT. Arguments are read until the call returns, so
    /// temporaries in the awaiting expression are fine.
    template <line_format::FixedString Fmt, line_format::LineArg... A>
        requires(Fmt.line_safe() &&
                 sizeof...(A) == line_format::detail::HOLES<Fmt>)
    coro::CoroTask<Result<void>> append_fmt(const A&... args) {
        co_return co_await append_view<line_format::PlainEscape>(
            line_format::view<Fmt>(), args...);
    }
    template <line_format::LineArg... A>
    coro::CoroTask<Result<void>> append_fmt(const line_format::LineFormat& fmt,
                                            const A&... args) {
        co_return co_await append_view<line_format::PlainEscape>(fmt.view(),
                                                                 args...);
    }
    template <line_format::FixedString Fmt, line_format::LineArg... A>
        requires(Fmt.line_safe() &&
                 sizeof...(A) == line_format::detail::HOLES<Fmt>)
    coro::CoroTask<Result<void>> append_json(const A&... args) {
        co_return co_await append_view<line_format::JsonEscape>(
            line_format::view<Fmt>(), args...);
    }
    template <line_format::LineArg... A>
    coro::CoroTask<Result<void>> append_json(
        const line_format::JsonLineFormat& fmt, const A&... args) {
        co_return co_await append_view<line_format::JsonEscape>(fmt.view(),
                                                                args...);
    }

    template <class E, line_format::LineArg... A>
    coro::CoroTask<Result<void>> append_view(line_format::detail::View f,
                                             const A&... args) {
        if (f.holes != sizeof...(A))
            co_return make_error(ErrorCode::INVALID_ARGUMENT,
                                 "line format argument count mismatch");
        if (line_format::any_null(args...))
            co_return make_error(ErrorCode::INVALID_ARGUMENT,
                                 "null string argument");
        co_return co_await append_with(
            line_format::max_line_bytes<E>(f, args...),
            [&](char* dst, std::size_t) {
                return line_format::format_line<E>(f, dst, args...);
            });
    }

    /// Ends the current member now; no-op when it is empty.
    coro::CoroTask<Result<void>> cut();

    coro::CoroTask<Result<WriteSummary>> close();

    /// Like close(), with `tail` (any bytes, may lack '\n') appended to the
    /// last member; it forms its own member when none is pending.
    coro::CoroTask<Result<WriteSummary>> close(std::string_view tail);

   private:
    explicit GzipLineWriter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// The same operations, blocking on the default runtime.
class GzipLineWriterBlocking {
   public:
    static Result<GzipLineWriterBlocking> open(std::string path,
                                               GzipWriterOptions opts);

    GzipLineWriterBlocking(GzipLineWriterBlocking&&) noexcept;
    GzipLineWriterBlocking& operator=(GzipLineWriterBlocking&&) noexcept;
    ~GzipLineWriterBlocking();

    Result<void> append(std::string_view lines);
    Result<void> append_with(
        std::size_t max_bytes,
        std::function<std::size_t(char*, std::size_t)> fill);
    Result<void> cut();
    Result<WriteSummary> close();
    Result<WriteSummary> close(std::string_view tail);

   private:
    explicit GzipLineWriterBlocking(GzipLineWriter w);
    std::unique_ptr<GzipLineWriter> w_;
};

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_GZIP_LINE_WRITER_H
