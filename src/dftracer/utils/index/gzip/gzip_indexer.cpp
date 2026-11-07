// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/raw_buffer.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/index_visitor.h>
#include <dftracer/utils/index/gzip/checkpoint_size.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/gzip/gzip_member_scanner.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/store/error.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_stream_inflater.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <utility>

namespace dftracer::utils::index::gzip {

namespace {

// -- Parallel path ---------------------------------------------------------
//
// When the file is multi-member gzip (the dftracer runtime format), divide
// the members across N worker coroutines and stream inflated chunks through
// per-worker channels to a single dispatcher.
//
// Workers report the absolute end of every member they finish; the
// dispatcher turns those into records with global uc_offsets and line
// numbers and appends them to the shared `members` vector in order.

struct ParallelInflateMsg {
    dstd::UniquePtr<dftracer::utils::dstd::RawBuffer<unsigned char>> data;
    // Per-worker monotonic sequence. Load-bearing: moodycamel (our channel
    // backend) does not guarantee strict FIFO without producer tokens, so
    // the dispatcher reorders by this before handing chunks to visitors.
    std::uint64_t lines = 0;
    // Absolute compressed offset just past the member this chunk ended.
    bool member_ended = false;
    /// With member_ended: the member's last line has no newline yet.
    bool has_open_line = false;
    std::uint64_t member_c_end = 0;
    // Uncompressed bytes of this chunk; `data` holds them only when a visitor
    // reads them.
    std::uint64_t data_size = 0;
    // This chunk is the recovered prefix of a member cut at end of file.
    bool truncated = false;
    // Bytes of a partial last line that earlier pieces of a cut member sent
    // and the cut drops.
    std::uint64_t drop_tail = 0;
    // This chunk ends a piece of a member that continues at a restart point
    // (member_c_end is that point). The next piece starts with `bits` bits of
    // the byte before it and the 32 KiB `window`.
    bool restart = false;
    std::uint8_t bits = 0;
    dstd::String window;
};

using ParallelChan = dftracer::utils::coro::Channel<ParallelInflateMsg>;

using Bytes = dftracer::utils::dstd::RawBuffer<unsigned char>;

static void drop_front(Bytes& b, std::size_t n) noexcept {
    if (n == 0) return;
    std::memmove(b.data(), b.data() + n, b.size() - n);
    b.resize(b.size() - n);
}

// Decode the gzip member at compressed offset `c_off` into `out`, reading it
// through `comp`, which the caller reuses. Neither buffer is zeroed. Returns
// the compressed bytes the member consumed, or nullopt on read/decode failure.
// Peak memory is one member.
static dftracer::utils::coro::CoroTask<dstd::Optional<std::uint64_t>>
decode_member_at(int fd, std::uint64_t c_off, std::uint64_t file_size,
                 const utilities::fileio::compress::GzipMemberDecompressor& dec,
                 Bytes& out, Bytes& comp, std::size_t& out_size,
                 bool& truncated, std::size_t max_out, bool& too_big) {
    truncated = false;
    too_big = false;
    constexpr std::size_t READ_CHUNK = 1u << 20;
    if (out.size() < (1u << 20)) out.resize(1u << 20);
    comp.clear();
    std::uint64_t next_read = c_off;
    utilities::fileio::compress::DecompressResult res{};
    while (true) {
        if (!comp.empty()) {
            const utilities::fileio::compress::GzipDecode status =
                dec.decompress_status(comp.data(), comp.size(), out.data(),
                                      out.size(), res);
            if (status == utilities::fileio::compress::GzipDecode::Ok) break;
            if (status ==
                utilities::fileio::compress::GzipDecode::InsufficientSpace) {
                if (out.size() >= max_out) {
                    too_big = true;
                    co_return dstd::nullopt;
                }
                out.resize(std::min(out.size() * 2, max_out));
                continue;
            }
            // BadData may just mean the member is not fully buffered yet.
        }
        if (next_read >= file_size) {
            if (comp.empty()) co_return dstd::nullopt;
            dstd::Vector<std::uint8_t> cut;
            auto keep = utilities::fileio::compress::decode_truncated_member(
                comp.data(), comp.size(), cut);
            if (!keep) co_return dstd::nullopt;
            out.resize(cut.size());
            std::memcpy(out.data(), cut.data(), cut.size());
            out_size = *keep;
            truncated = true;
            co_return static_cast<std::uint64_t>(comp.size());
        }
        // libdeflate restarts the member on each retry, so at least double
        // the buffered input per retry to keep a large member linear.
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(
                std::max(READ_CHUNK, comp.size()), file_size - next_read));
        const std::size_t old = comp.size();
        comp.resize(old + want);
        const ssize_t n = co_await dftracer::utils::runtime::io::pread(
            fd, comp.data() + old, want, next_read);
        if (n <= 0) co_return dstd::nullopt;
        comp.resize(old + static_cast<std::size_t>(n));
        next_read += static_cast<std::uint64_t>(n);
    }
    out_size = res.out_bytes;
    co_return res.in_bytes;
}

// Byte-wide counters over blocks of at most 255 bytes let the compiler
// vectorize the compare-and-add; a 64-bit counter per byte does not.
static std::uint64_t count_newlines(const unsigned char* p, std::size_t n) {
    std::uint64_t lines = 0;
    while (n > 0) {
        const std::size_t m = std::min<std::size_t>(n, 255);
        std::uint8_t block = 0;
        for (std::size_t i = 0; i < m; ++i)
            block = static_cast<std::uint8_t>(block + (p[i] == '\n'));
        lines += block;
        p += m;
        n -= m;
    }
    return lines;
}

// Deflate random-access points follow Mark Adler's zran.c from zlib's examples
// (zlib license): https://github.com/madler/zlib/blob/master/examples/zran.c
constexpr std::size_t RESTART_WINDOW = 32768;
// A streamed member goes to the dispatcher in pieces of about this size, apart
// from its restart points, so a file in flight holds a few pieces and not a
// checkpoint's worth of text per message.
constexpr std::size_t SHIP_BYTES = std::size_t{1} << 20;
// A member that inflates past this is streamed rather than decoded whole, so a
// file in flight does not hold several whole members at once.
constexpr std::size_t WHOLE_MEMBER_BYTES = std::size_t{8} << 20;

// No deflate stream expands past 1032 output bytes per input byte.
constexpr std::uint64_t MAX_DEFLATE_RATIO = 1032;

// Zero bytes an older padded striped writer left before a stripe boundary;
// it wrote fewer than 25.
constexpr std::uint64_t MAX_ZERO_GAP = 32;

// Where the member after one that ends at `end` starts: past a short run of
// zero bytes that reaches the next candidate or the range end, else at `end`.
// Not every real member start is a candidate (the scanner rejects some
// writers' headers), so only zeros are skipped.
static dftracer::utils::coro::CoroTask<std::uint64_t> next_member_start(
    int fd, const dstd::Vector<GzipMember>& candidates, std::uint64_t end,
    std::uint64_t range_c_end) {
    if (end >= range_c_end) co_return end;
    const auto it = std::lower_bound(
        candidates.begin(), candidates.end(), end,
        [](const GzipMember& m, std::uint64_t v) { return m.c_offset < v; });
    const std::uint64_t to = it == candidates.end()
                                 ? range_c_end
                                 : std::min(it->c_offset, range_c_end);
    if (to == end || to - end > MAX_ZERO_GAP) co_return end;
    unsigned char gap[MAX_ZERO_GAP];
    const std::size_t len = static_cast<std::size_t>(to - end);
    const ssize_t n =
        co_await dftracer::utils::runtime::io::pread(fd, gap, len, end);
    if (n != static_cast<ssize_t>(len)) co_return end;
    for (std::size_t i = 0; i < len; ++i)
        if (gap[i] != 0) co_return end;
    co_return to;
}

using InflateBuffer = dstd::UniquePtr<Bytes>;

// The inflate buffers of one file. A message moves its buffer to the
// dispatcher, which hands it back once the visitors have read it: a fresh
// member-sized buffer per message left gigabytes of freed buffers with the
// allocator. A taken buffer keeps the size it was given back with.
class InflateBuffers {
   public:
    explicit InflateBuffers(std::size_t keep) : keep_(keep) {}
    InflateBuffer take() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!free_.empty()) {
                InflateBuffer b = std::move(free_.back());
                free_.pop_back();
                return b;
            }
        }
        return dstd::make_unique<Bytes>();
    }
    void give(InflateBuffer b) {
        if (!b) return;
        std::lock_guard<std::mutex> lock(mu_);
        if (free_.size() < keep_) free_.push_back(std::move(b));
    }

   private:
    std::mutex mu_;
    dstd::Vector<InflateBuffer> free_;
    std::size_t keep_;
};

// Stream the gzip member at `c_off` with zlib, emitting a message per piece:
// a restart point is taken at the first block boundary once the piece holds
// `ckpt_size` output bytes. A member that stays under that is one message.
// `in_bytes` is the compressed size consumed. `stop` is set when the consumer
// is gone or the member was cut with nothing to keep.
static dftracer::utils::coro::CoroTask<bool> stream_member(
    int fd, std::uint64_t c_off, std::uint64_t file_size,
    std::uint64_t ckpt_size, const dstd::Vector<GzipMember>& candidates,
    std::uint64_t range_c_end, bool strip_leading_partial, bool ship_bytes,
    InflateBuffers& buffers,
    dftracer::utils::coro::ChannelProducer<ParallelInflateMsg>& producer,
    std::uint64_t& in_bytes, bool& stop) {
    constexpr std::size_t READ_CHUNK = 1u << 20;
    constexpr std::size_t OUT_STEP = 1u << 20;
    const std::size_t split_at = static_cast<std::size_t>(
        std::max<std::uint64_t>(ckpt_size, RESTART_WINDOW));

    utilities::fileio::compress::GzipStreamInflater gz;
    if (!gz.init(31)) co_return false;

    Bytes in(READ_CHUNK);
    InflateBuffer buf = buffers.take();
    buf->clear();
    // Growing a piece by doubling would leave a buffer twice the piece.
    buf->reserve(SHIP_BYTES + OUT_STEP);
    std::uint64_t next_read = c_off;
    bool strip = strip_leading_partial;
    bool cut = false;
    std::uint64_t since_newline = 0;
    std::uint64_t since_restart = 0;
    // The last RESTART_WINDOW bytes of output already shipped, for the window
    // of a restart point whose piece holds less than that.
    dstd::String shipped_tail;
    stop = false;

    auto remember = [&](const Bytes& data) {
        const char* bytes = reinterpret_cast<const char*>(data.data());
        if (data.size() >= RESTART_WINDOW) {
            shipped_tail.assign(bytes + data.size() - RESTART_WINDOW,
                                RESTART_WINDOW);
            return;
        }
        shipped_tail.append(bytes, data.size());
        if (shipped_tail.size() > RESTART_WINDOW)
            shipped_tail.erase(0, shipped_tail.size() - RESTART_WINDOW);
    };

    auto emit = [&](ParallelInflateMsg& msg) {
        Bytes& data = *buf;
        remember(data);
        std::size_t start = 0;
        if (strip) {
            start = data.size();
            for (std::size_t i = 0; i < data.size(); ++i) {
                if (data[i] == '\n') {
                    start = i + 1;
                    strip = false;
                    break;
                }
            }
        }
        msg.lines = count_newlines(data.data() + start, data.size() - start);
        std::size_t last = data.size();
        while (last > start && data[last - 1] != '\n') --last;
        since_newline = last > start ? data.size() - last
                                     : since_newline + (data.size() - start);
        msg.data_size = data.size() - start;
        if (ship_bytes) {
            drop_front(data, start);
            msg.data = std::move(buf);
            buf = buffers.take();
            buf->reserve(SHIP_BYTES + OUT_STEP);
        }
        buf->clear();
    };

    while (true) {
        if (gz.avail_in() == 0 && next_read < file_size) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(READ_CHUNK, file_size - next_read));
            const ssize_t n = co_await dftracer::utils::runtime::io::pread(
                fd, in.data(), want, next_read);
            if (n <= 0) co_return false;
            gz.set_input(in.data(), static_cast<std::size_t>(n));
            next_read += static_cast<std::uint64_t>(n);
        }

        Bytes& piece = *buf;
        const std::size_t old = piece.size();
        piece.resize(old + OUT_STEP);
        gz.set_output(piece.data() + old, OUT_STEP);
        const utilities::fileio::compress::InflateStatus rc =
            gz.inflate(utilities::fileio::compress::InflateFlush::Block);
        piece.resize(old + (OUT_STEP - gz.avail_out()));
        since_restart += piece.size() - old;

        if (rc == utilities::fileio::compress::InflateStatus::StreamEnd) break;
        if (rc != utilities::fileio::compress::InflateStatus::Ok &&
            rc != utilities::fileio::compress::InflateStatus::BufError) {
            co_return false;
        }
        if (gz.avail_in() == 0 && next_read >= file_size &&
            gz.avail_out() != 0) {
            cut = true;
            break;
        }

        const int data_type = gz.data_type();
        if ((data_type & 128) && !(data_type & 64) &&
            since_restart >= split_at) {
            ParallelInflateMsg msg;
            msg.restart = true;
            msg.bits = static_cast<std::uint8_t>(data_type & 7);
            msg.member_c_end = c_off + gz.total_in();
            const char* bytes = reinterpret_cast<const char*>(piece.data());
            if (piece.size() >= RESTART_WINDOW) {
                msg.window.assign(bytes + piece.size() - RESTART_WINDOW,
                                  RESTART_WINDOW);
            } else {
                const std::size_t from_tail = RESTART_WINDOW - piece.size();
                msg.window.assign(shipped_tail, shipped_tail.size() - from_tail,
                                  from_tail);
                msg.window.append(bytes, piece.size());
            }
            since_restart = 0;
            emit(msg);
            const auto send_ok = co_await producer.send(std::move(msg));
            if (!send_ok) {
                stop = true;
                co_return true;
            }
        } else if (piece.size() >= SHIP_BYTES) {
            ParallelInflateMsg msg;
            emit(msg);
            const auto send_ok = co_await producer.send(std::move(msg));
            if (!send_ok) {
                stop = true;
                co_return true;
            }
        }
    }

    ParallelInflateMsg msg;
    Bytes& piece = *buf;
    if (cut) {
        std::size_t keep = piece.size();
        while (keep > 0 && piece[keep - 1] != '\n') --keep;
        if (keep == 0) {
            msg.truncated = true;
            msg.drop_tail = since_newline;
            co_await producer.send(std::move(msg));
            stop = true;
            co_return true;
        }
        piece.resize(keep);
        msg.truncated = true;
        in_bytes = file_size - c_off;
    } else {
        in_bytes = gz.total_in();
    }
    emit(msg);
    buffers.give(std::move(buf));
    msg.member_ended = true;
    msg.has_open_line = since_newline > 0;
    msg.member_c_end = co_await next_member_start(
        fd, candidates, c_off + in_bytes, range_c_end);
    const auto send_ok = co_await producer.send(std::move(msg));
    if (!send_ok) stop = true;
    co_return true;
}

// Each worker owns a member-aligned compressed range and decodes its members
// one at a time with libdeflate, emitting one message per member. strip and
// extend handle lines that straddle a slice boundary in the distributed
// (member_begin > 0) case; single-process workers use neither and rely on the
// dispatcher accumulator to reassemble lines across worker boundaries.
static dftracer::utils::coro::CoroTask<bool> parallel_worker(
    int fd, std::uint64_t file_size, std::uint64_t range_c_start,
    std::uint64_t range_c_end, std::uint64_t ckpt_size,
    const dstd::Vector<GzipMember>& candidates, bool strip_leading_partial,
    bool extend_to_newline_past_end, bool ship_bytes, InflateBuffers& buffers,
    dftracer::utils::coro::ChannelProducer<ParallelInflateMsg> producer) {
    auto guard = producer.guard();

    utilities::fileio::compress::GzipMemberDecompressor dec;
    if (!dec.valid()) co_return false;

    InflateBuffer buf = buffers.take();
    Bytes comp;
    std::uint64_t c_off = range_c_start;
    bool first = true;

    while (c_off < range_c_end) {
        const auto next =
            std::upper_bound(candidates.begin(), candidates.end(), c_off,
                             [](std::uint64_t v, const GzipMember& m) {
                                 return v < m.c_offset;
                             });
        const std::uint64_t span =
            (next != candidates.end() ? std::min(next->c_offset, range_c_end)
                                      : range_c_end) -
            c_off;
        // A member larger than a piece is streamed. The header scan gives its
        // size (ISIZE) when the trailer can be trusted. Otherwise the decode
        // below stops at the bound and the member is streamed from its start.
        const std::size_t bound =
            std::min(static_cast<std::size_t>(
                         std::max<std::uint64_t>(ckpt_size, RESTART_WINDOW)),
                     WHOLE_MEMBER_BYTES);
        const GzipMember* known = next != candidates.begin() &&
                                          next != candidates.end() &&
                                          std::prev(next)->c_offset == c_off
                                      ? &*std::prev(next)
                                      : nullptr;
        const bool size_known =
            known != nullptr &&
            known->isize <= known->c_size * MAX_DEFLATE_RATIO;
        bool stream_it =
            span >= ckpt_size / 8 || (size_known && known->isize > bound);
        std::size_t out_size = 0;
        bool truncated = false;
        Bytes& out = *buf;
        dstd::Optional<std::uint64_t> in_bytes;
        if (!stream_it) {
            // A buffer shorter than the member's ISIZE would make the decode
            // start over. The last member has no ISIZE to trust, since a cut
            // can end it anywhere.
            if (size_known && out.size() <= known->isize)
                out.resize(std::size_t{known->isize} + 1);
            bool too_big = false;
            in_bytes =
                co_await decode_member_at(fd, c_off, file_size, dec, out, comp,
                                          out_size, truncated, bound, too_big);
            if (too_big) {
                stream_it = true;
            } else if (!in_bytes) {
                co_return false;
            }
        }
        if (stream_it) {
            std::uint64_t consumed = 0;
            bool stop = false;
            const auto stream_member_ok = co_await stream_member(
                fd, c_off, file_size, ckpt_size, candidates, range_c_end,
                strip_leading_partial && first, ship_bytes, buffers, producer,
                consumed, stop);
            if (!stream_member_ok) {
                co_return false;
            }
            if (stop) co_return true;
            first = false;
            c_off = co_await next_member_start(fd, candidates, c_off + consumed,
                                               range_c_end);
            continue;
        }
        if (truncated && out_size == 0) {
            ParallelInflateMsg msg;
            msg.truncated = true;
            co_await producer.send(std::move(msg));
            co_return true;
        }

        std::size_t emit_start = 0;
        // The first member of a mid-file slice opens mid-line (the tail of a
        // line that began in the previous slice); drop up to its first \n so
        // the dispatcher sees a clean start.
        if (strip_leading_partial && first) {
            emit_start = out_size;
            for (std::size_t i = 0; i < out_size; ++i) {
                if (out[i] == '\n') {
                    emit_start = i + 1;
                    break;
                }
            }
        }
        first = false;

        const std::size_t emit_size = out_size - emit_start;
        ParallelInflateMsg msg;
        msg.lines = count_newlines(out.data() + emit_start, emit_size);
        msg.data_size = emit_size;
        if (ship_bytes) {
            out.resize(out_size);
            drop_front(out, emit_start);
            msg.data = std::move(buf);
            buf = buffers.take();
        }
        msg.member_ended = true;
        msg.has_open_line = out_size > 0 && out[out_size - 1] != '\n';
        msg.member_c_end = co_await next_member_start(
            fd, candidates, c_off + *in_bytes, range_c_end);
        msg.truncated = truncated;
        c_off = msg.member_c_end;
        const auto send_ok = co_await producer.send(std::move(msg));
        if (!send_ok) co_return true;
    }

    // Non-last slice: capture the line straddling this slice's end by decoding
    // the next slice's first member and emitting up to and including its first
    // \n. No member record for it (member_ended stays false).
    Bytes& out = *buf;
    if (extend_to_newline_past_end && range_c_end < file_size) {
        std::size_t out_size = 0;
        bool truncated = false;
        bool too_big = false;
        auto in_bytes = co_await decode_member_at(
            fd, range_c_end, file_size, dec, out, comp, out_size, truncated,
            std::numeric_limits<std::size_t>::max(), too_big);
        if (!in_bytes) co_return false;
        std::size_t take = out_size;
        for (std::size_t i = 0; i < out_size; ++i) {
            if (out[i] == '\n') {
                take = i + 1;
                break;
            }
        }
        ParallelInflateMsg msg;
        msg.lines = count_newlines(out.data(), take);
        msg.data_size = take;
        if (ship_bytes) {
            msg.data = dstd::make_unique<Bytes>(take);
            std::memcpy(msg.data->data(), out.data(), take);
        }
        msg.member_ended = false;
        co_await producer.send(std::move(msg));
    }
    buffers.give(std::move(buf));
    co_return true;
}
// A run of consecutive candidates that one worker inflates. `messages` is
// its member count, so a worker ahead of the dispatcher does not block on
// a range of small members.
struct InflateRange {
    std::uint64_t c_start = 0;
    std::uint64_t c_end = 0;
    std::size_t messages = 0;
};

// Ranges of about `target` uncompressed bytes, by ISIZE. A member counts at
// least its compressed size, so a garbage ISIZE or a member above 4 GiB still
// ends its range.
static dstd::Vector<InflateRange> plan_ranges(
    const dstd::Vector<GzipMember>& members, std::uint64_t slice_c_end,
    std::uint64_t target) {
    dstd::Vector<InflateRange> ranges;
    std::uint64_t acc = 0;
    for (const GzipMember& m : members) {
        if (ranges.empty() || acc >= target) {
            if (!ranges.empty()) ranges.back().c_end = m.c_offset;
            ranges.push_back({m.c_offset, 0, 0});
            acc = 0;
        }
        acc += std::max<std::uint64_t>(m.isize, m.c_size);
        ++ranges.back().messages;
    }
    ranges.back().c_end = slice_c_end;
    return ranges;
}

// A message without bytes is small, so a worker may run far ahead of the
// dispatcher when nothing reads the text.
constexpr std::size_t UNSHIPPED_MESSAGES = 1024;

struct RangeSlot {
    dstd::SharedPtr<ParallelChan> chan;
    dstd::Optional<dftracer::utils::coro::ChannelProducer<ParallelInflateMsg>>
        producer;
};

// Workers take ranges in order, each after a window permit; the dispatcher
// returns the permit when it has consumed the range. So at most `ring.size()`
// ranges are inflated and not consumed, and the range the dispatcher waits on
// always has a worker.
struct InflatePlan {
    InflatePlan(std::size_t window, std::size_t workers)
        : permits(window), ring(window), buffers(window + workers) {}

    int fd = -1;
    std::uint64_t file_size = 0;
    std::uint64_t ckpt_size = 0;
    bool ship_bytes = false;
    bool strip_first = false;
    bool extend_last = false;
    dstd::Vector<GzipMember> members;
    dstd::Vector<InflateRange> ranges;
    dftracer::utils::coro::AsyncSemaphore permits;
    dstd::Vector<RangeSlot> ring;
    InflateBuffers buffers;
    std::atomic<std::size_t> next{0};

    dstd::SharedPtr<ParallelChan> channel(std::size_t r) const {
        return dftracer::utils::coro::make_channel<ParallelInflateMsg>(
            ship_bytes ? ranges[r].messages + 1 : UNSHIPPED_MESSAGES);
    }
    void open(std::size_t r) {
        RangeSlot& slot = ring[r % ring.size()];
        slot.chan = channel(r);
        slot.producer.emplace(slot.chan->producer());
    }
    bool extend(std::size_t r) const {
        return extend_last && r + 1 == ranges.size();
    }
    // Ends every worker when the dispatcher stops early: a send to a closed
    // channel fails, and a worker woken by the permits finds no range left.
    void abandon(ParallelChan* redo) {
        next.store(ranges.size());
        for (RangeSlot& slot : ring)
            if (slot.chan) slot.chan->close();
        if (redo) redo->close();
        permits.release(ring.size());
    }
};

static dftracer::utils::coro::CoroTask<void> range_worker(InflatePlan* plan) {
    while (true) {
        co_await plan->permits.acquire(1);
        const std::size_t r = plan->next.fetch_add(1);
        if (r >= plan->ranges.size()) {
            plan->permits.release(1);
            co_return;
        }
        RangeSlot& slot = plan->ring[r % plan->ring.size()];
        auto producer = std::move(*slot.producer);
        slot.producer.reset();
        const InflateRange& range = plan->ranges[r];
        co_await parallel_worker(plan->fd, plan->file_size, range.c_start,
                                 range.c_end, plan->ckpt_size, plan->members,
                                 plan->strip_first && r == 0, plan->extend(r),
                                 plan->ship_bytes, plan->buffers,
                                 std::move(producer));
    }
}

static dftracer::utils::coro::CoroTask<bool> parallel_dispatcher(
    coro::Scope* scope, InflatePlan* plan, std::uint64_t member_idx_base,
    std::uint64_t& total_lines, std::uint64_t& total_uc_size,
    dstd::Vector<GzipMemberRecord>& members,
    dstd::Vector<dstd::Pair<std::uint64_t, dstd::String>>& restart_windows,
    const CheckpointIndexer::VisitorList& visitors, bool& truncated,
    std::uint64_t& recovered_bytes, bool& has_open_line) {
    const bool has_visitors = !visitors.empty();
    std::uint64_t global_uc = 0;

    std::uint64_t member_idx = member_idx_base;
    std::uint64_t member_c_start = plan->ranges.front().c_start;
    std::uint64_t member_uc_start = 0;
    std::uint64_t member_first_line = total_lines + 1;
    GzipRecordKind piece_kind = GzipRecordKind::Member;
    std::uint8_t piece_bits = 0;
    bool piece_opens_mid_line = false;
    dstd::String piece_window;

    std::uint64_t total_chunks_received = 0;

    auto process_msg =
        [&](ParallelInflateMsg& msg) -> dftracer::utils::coro::CoroTask<void> {
        const std::size_t data_size = static_cast<std::size_t>(msg.data_size);
        ++total_chunks_received;
        if (msg.truncated) {
            truncated = true;
            recovered_bytes += data_size;
        }
        if (msg.drop_tail > 0) {
            std::uint64_t left = msg.drop_tail;
            while (left > 0 && !members.empty()) {
                auto& last = members.back();
                const std::uint64_t cut = std::min(left, last.uc_size);
                last.uc_size -= cut;
                left -= cut;
                if (last.uc_size == 0) {
                    if (last.kind == GzipRecordKind::Restart)
                        std::erase_if(restart_windows, [&](const auto& w) {
                            return w.first == last.member_idx;
                        });
                    members.pop_back();
                    --member_idx;
                }
            }
            global_uc -= msg.drop_tail - left;
            member_uc_start = global_uc;
        }

        if (has_visitors && data_size > 0) {
            const char* data = reinterpret_cast<const char*>(msg.data->data());
            for (auto& v : visitors) {
                co_await v.get().on_chunk(data, data_size, member_idx);
            }
        }

        global_uc += data_size;
        total_lines += msg.lines;

        if (msg.member_ended) has_open_line = msg.has_open_line;
        if (msg.member_ended || msg.restart) {
            GzipRecordKind kind = piece_kind;
            if (msg.restart && kind == GzipRecordKind::Member) {
                kind = GzipRecordKind::Head;
            }
            members.push_back(GzipMemberRecord{
                .member_idx = member_idx,
                .c_offset = member_c_start,
                .c_size = msg.member_c_end - member_c_start,
                .uc_offset = member_uc_start,
                .uc_size = global_uc - member_uc_start,
                .first_line_num = member_first_line,
                .last_line_num = total_lines,
                .kind = kind,
                .bits = piece_bits,
                .opens_mid_line = piece_opens_mid_line,
            });
            if (kind == GzipRecordKind::Restart) {
                restart_windows.emplace_back(member_idx,
                                             std::move(piece_window));
            }
            piece_opens_mid_line =
                msg.restart ? msg.window.empty() || msg.window.back() != '\n'
                            : msg.has_open_line;
            if (msg.restart) {
                piece_kind = GzipRecordKind::Restart;
                piece_bits = msg.bits;
                piece_window = std::move(msg.window);
            } else {
                piece_kind = GzipRecordKind::Member;
                piece_bits = 0;
            }
            member_c_start = msg.member_c_end;
            member_uc_start = global_uc;
            member_first_line = total_lines + 1;

            if (has_visitors) {
                for (auto& v : visitors) {
                    co_await v.get().on_checkpoint(member_idx);
                }
            }
            ++member_idx;
        }

        co_return;
    };

    auto drain_visitors = [&]() -> dftracer::utils::coro::CoroTask<void> {
        for (auto& v : visitors) {
            if (v.get().wants_drain()) {
                co_await v.get().drain_pending();
            }
        }
    };

    auto consume =
        [&](ParallelChan& chan) -> dftracer::utils::coro::CoroTask<void> {
        for (;;) {
            auto msg_opt = co_await chan.receive();
            if (!msg_opt) break;
            co_await process_msg(*msg_opt);
            plan->buffers.give(std::move(msg_opt->data));
            co_await drain_visitors();
        }
    };
    auto discard =
        [&](ParallelChan& chan) -> dftracer::utils::coro::CoroTask<void> {
        while (true) {
            auto msg_opt = co_await chan.receive();
            if (!msg_opt) co_return;
            plan->buffers.give(std::move(msg_opt->data));
        }
    };

    dstd::SharedPtr<ParallelChan> redo;
    std::exception_ptr error;
    try {
        for (std::size_t r = 0; r < plan->ranges.size(); ++r) {
            const InflateRange& range = plan->ranges[r];
            auto chan = plan->ring[r % plan->ring.size()].chan;
            if (member_c_start > range.c_start) {
                // The member before runs past this range's start, so the start
                // is a false candidate: inflate the rest of the range from the
                // end of that member instead.
                co_await discard(*chan);
                if (member_c_start < range.c_end) {
                    redo = plan->channel(r);
                    scope->spawn_future(
                        [plan, r, start = member_c_start,
                         producer = redo->producer()]() mutable {
                            const InflateRange& rg = plan->ranges[r];
                            return parallel_worker(
                                plan->fd, plan->file_size, start, rg.c_end,
                                plan->ckpt_size, plan->members, false,
                                plan->extend(r), plan->ship_bytes,
                                plan->buffers, std::move(producer));
                        });
                    co_await consume(*redo);
                    redo.reset();
                }
            } else {
                // A range after one that stopped at a bad member starts at its
                // own first member.
                member_c_start = range.c_start;
                co_await consume(*chan);
            }
            if (r + plan->ring.size() < plan->ranges.size())
                plan->open(r + plan->ring.size());
            plan->permits.release(1);
        }
    } catch (...) {
        plan->abandon(redo.get());
        error = std::current_exception();
    }
    if (error) {
        // Visitors finish their own tasks, which may hold their state.
        for (auto& v : visitors) {
            try {
                co_await v.get().flush();
            } catch (const std::exception& e) {
                // The first error is the one rethrown below.
                DFTRACER_UTILS_LOG_WARN(
                    "Indexer: a visitor flush failed after an earlier "
                    "error: %s",
                    e.what());
            } catch (...) {
                DFTRACER_UTILS_LOG_WARN(
                    "Indexer: a visitor flush failed after an earlier "
                    "error with a non-standard exception");
            }
        }
        std::rethrow_exception(error);
    }

    if (has_visitors) {
        for (auto& v : visitors) co_await v.get().flush();
    }
    total_uc_size = global_uc;
    co_return true;
}

static dftracer::utils::coro::CoroTask<bool> process_chunks_parallel(
    int fd, std::uint64_t slice_c_end, std::uint64_t file_size,
    std::uint64_t ckpt_size, std::size_t ways,
    dstd::Vector<GzipMember> scanned_members, std::uint64_t member_idx_base,
    bool strip_slice_leading_partial, std::uint64_t& total_lines,
    std::uint64_t& total_uc_size, dstd::Vector<GzipMemberRecord>& members,
    dstd::Vector<dstd::Pair<std::uint64_t, dstd::String>>& restart_windows,
    const CheckpointIndexer::VisitorList& visitors, bool& truncated,
    std::uint64_t& recovered_bytes, bool& has_open_line) {
    // Fanning out past the thread count only adds channel and scheduling
    // overhead; on a single-threaded drive (coro::RunLoop, the no-executor
    // .get() path) it collapses to one worker.
    constexpr std::size_t DEFAULT_MAX_WORKERS = 16;
    dstd::Vector<InflateRange> ranges =
        plan_ranges(scanned_members, slice_c_end,
                    std::max<std::uint64_t>(ckpt_size / 4, 1));
    const std::size_t hw =
        std::max<std::size_t>(1, common::available_parallelism());
    const std::size_t worker_count = std::min<std::size_t>(
        {ways > 0 ? ways : DEFAULT_MAX_WORKERS, ranges.size(), hw});
    // Two ranges past the workers' keep them busy while the dispatcher
    // consumes the oldest; each range in the window holds its plaintext.
    InflatePlan plan(std::min<std::size_t>(worker_count + 2, ranges.size()),
                     worker_count);
    plan.fd = fd;
    plan.file_size = file_size;
    plan.ckpt_size = ckpt_size;
    // Without visitors a message carries no bytes, so its channel is
    // unbounded.
    plan.ship_bytes = !visitors.empty();
    plan.strip_first = strip_slice_leading_partial;
    // The last range of a non-last slice extends past `slice_c_end` to
    // capture the line that straddles the slice boundary.
    plan.extend_last = slice_c_end < file_size;
    plan.members = std::move(scanned_members);
    plan.ranges = std::move(ranges);
    for (std::size_t r = 0; r < plan.ring.size(); ++r) plan.open(r);

    bool dispatcher_ok = true;
    InflatePlan* plan_ptr = &plan;
    co_await coro::with_scope(
        [&](coro::Scope& child) -> dftracer::utils::coro::CoroTask<void> {
            for (std::size_t w = 0; w < worker_count; ++w)
                child.spawn([plan_ptr]() { return range_worker(plan_ptr); });
            // In the body, not spawned, so its error leaves the scope after the
            // workers are joined.
            dispatcher_ok = co_await parallel_dispatcher(
                &child, plan_ptr, member_idx_base, total_lines, total_uc_size,
                members, restart_windows, visitors, truncated, recovered_bytes,
                has_open_line);
        });

    co_return dispatcher_ok;
}

static dftracer::utils::coro::CoroTask<bool> process_chunks(
    int fd, const GzipMemberSlice* slice, std::uint64_t ckpt_size,
    std::size_t ways, std::uint64_t& total_lines, std::uint64_t& total_uc_size,
    dstd::Vector<GzipMemberRecord>& members,
    dstd::Vector<dstd::Pair<std::uint64_t, dstd::String>>& restart_windows,
    const CheckpointIndexer::VisitorList& visitors, bool& truncated,
    std::uint64_t& recovered_bytes, bool& has_open_line) {
    struct stat st;
    const ssize_t stat_result = co_await runtime::io::fstat(fd, &st);
    if (stat_result != 0) co_return false;
    const std::uint64_t file_size = static_cast<std::uint64_t>(st.st_size);

    // Pre-scanned slice path: caller supplied the member map and a range.
    // Used by the MPI/distributed indexer to split one file across ranks
    // without re-scanning. Member indices start at `member_begin` so
    // multiple slices of the same file_id produce disjoint entries.
    if (slice != nullptr && slice->members != nullptr &&
        slice->member_end > slice->member_begin) {
        const auto& all = *slice->members;
        const std::size_t mb = slice->member_begin;
        const std::size_t me = slice->member_end;
        if (me > all.size() || mb >= me) co_return false;
        // Slice end: next-member offset if this isn't the last slice of
        // the file, else EOF. Crucial: a non-last slice's workers must
        // not inflate past this boundary into another slice's bytes.
        const std::uint64_t slice_c_end = slice->c_end != 0   ? slice->c_end
                                          : (me < all.size()) ? all[me].c_offset
                                                              : file_size;
        dstd::Vector<GzipMember> sliced(all.begin() + mb, all.begin() + me);
        co_return co_await process_chunks_parallel(
            fd, slice_c_end, slice->c_end != 0 ? slice->c_end : file_size,
            ckpt_size, ways, std::move(sliced),
            /*member_idx_base=*/slice->idx_base + mb,
            /*strip_slice_leading_partial=*/mb > 0, total_lines, total_uc_size,
            members, restart_windows, visitors, truncated, recovered_bytes,
            has_open_line);
    }

    // Discover member boundaries so the inflate pass can fan out. The scan
    // is zero-copy, sequential, and fast relative to inflate. A file whose
    // members cannot be enumerated is processed as one member spanning the
    // whole file, which the same path handles with a single worker.
    dstd::Vector<GzipMember> scanned;
    if (file_size >= 18) {
        co_await enumerate_gzip_member_candidates(fd, file_size, scanned);
    }
    if (scanned.empty()) scanned.push_back(GzipMember{0, file_size});

    co_return co_await process_chunks_parallel(
        fd, file_size, file_size, ckpt_size, ways, std::move(scanned),
        /*member_idx_base=*/0, /*strip_slice_leading_partial=*/false,
        total_lines, total_uc_size, members, restart_windows, visitors,
        truncated, recovered_bytes, has_open_line);
}

}  // namespace

dftracer::utils::coro::CoroTask<dstd::Optional<GzipBuildArtifacts>>
build_gzip_index_artifacts(dstd::CStringView gz_path, std::uint64_t ckpt_size,
                           const CheckpointIndexer::VisitorList& visitors,
                           coro::Scope* scope, const GzipMemberSlice* slice,
                           std::size_t ways) {
    const dstd::OpenOptions open_options;
    auto opening =
        runtime::io::open_file(dstd::PathView(gz_path), open_options);
    auto file = co_await std::move(opening);
    if (!file) {
        co_return dstd::nullopt;
    }
    co_return co_await build_gzip_index_artifacts(file->native_handle(),
                                                  gz_path, ckpt_size, visitors,
                                                  scope, slice, ways);
}

dftracer::utils::coro::CoroTask<dstd::Optional<GzipBuildArtifacts>>
build_gzip_index_artifacts(int fd, dstd::CStringView gz_path,
                           std::uint64_t ckpt_size,
                           const CheckpointIndexer::VisitorList& visitors,
                           coro::Scope* scope, const GzipMemberSlice* slice,
                           std::size_t ways) {
    struct stat fst{};
    const ssize_t fst_result = co_await runtime::io::fstat(fd, &fst);
    const std::uint64_t compressed_bytes =
        fst_result == 0 ? static_cast<std::uint64_t>(fst.st_size) : 0;
    if (!visitors.empty()) {
        const std::size_t estimated = static_cast<std::size_t>(
            compressed_bytes / (ckpt_size > 0 ? ckpt_size : 1));
        for (auto& visitor : visitors) {
            visitor.get().begin(estimated);
            visitor.get().bind_scope(scope);
        }
    }

    std::uint64_t total_lines = 0;
    std::uint64_t total_uc_size = 0;
    dstd::Vector<GzipMemberRecord> members;
    dstd::Vector<dstd::Pair<std::uint64_t, dstd::String>> restart_windows;

    bool truncated = false;
    std::uint64_t recovered_bytes = 0;
    bool has_open_line = false;
    const bool success = co_await process_chunks(
        fd, slice, ckpt_size, ways, total_lines, total_uc_size, members,
        restart_windows, visitors, truncated, recovered_bytes, has_open_line);

    if (!success) {
        co_return dstd::nullopt;
    }

    if (truncated) {
        DFTRACER_UTILS_LOG_WARN(
            "Indexer: %s ends in a truncated gzip member; recovered %llu bytes "
            "up to its last complete line (not checksum-verified).",
            gz_path.c_str(), static_cast<unsigned long long>(recovered_bytes));
    } else if (slice == nullptr && members.empty() && compressed_bytes > 0) {
        unsigned char magic[2] = {0, 0};
        const ssize_t magic_read = co_await runtime::io::pread(fd, magic, 2, 0);
        if (magic_read != 2) magic[0] = 0;
        if (magic[0] == 0x1F && magic[1] == 0x8B) {
            DFTRACER_UTILS_LOG_WARN(
                "Indexer: no gzip member of %s could be decoded; it is "
                "corrupt, so the index will be empty.",
                gz_path.c_str());
        } else {
            throw common::DFTUtilsException::cat(
                common::ErrorCode::InvalidArgument, "Indexer: ", gz_path,
                " is not gzip; an index reads gzip traces only. Gzip it "
                "first");
        }
    }

    GzipBuildArtifacts artifacts;
    artifacts.checkpoint_size = ckpt_size;
    artifacts.total_lines = total_lines;
    artifacts.total_uc_size = total_uc_size;
    artifacts.members = std::move(members);
    artifacts.restart_windows = std::move(restart_windows);
    artifacts.truncated = truncated;
    artifacts.recovered_bytes = recovered_bytes;
    artifacts.has_open_line = has_open_line;
    co_return artifacts;
}

void persist_gzip_index_artifacts(index::FileBuild& build,
                                  const GzipBuildArtifacts& artifacts) {
    for (const auto& member : artifacts.members)
        index::put_gzip_member(build, member);
    for (const auto& [member_idx, window] : artifacts.restart_windows)
        index::put_restart_window(build, member_idx, window);
    const bool has_mid_line_chunk =
        artifacts.has_mid_line_chunk ||
        std::any_of(artifacts.members.begin(), artifacts.members.end(),
                    [](const GzipMemberRecord& member) {
                        return member.opens_mid_line;
                    });
    index::put_file_metadata(
        build, artifacts.checkpoint_size, artifacts.total_lines,
        artifacts.total_uc_size, artifacts.truncated, artifacts.has_open_line,
        artifacts.members.empty() ? 0 : artifacts.members.back().member_idx + 1,
        has_mid_line_chunk);
    build.finish(index::store::IndexExtension::Members, 0);
}

GzipIndexer::GzipIndexer(const dstd::String& gz_path,
                         const dstd::String& idx_path, std::uint64_t ckpt_size,
                         bool force_rebuild)
    : gz_path_(gz_path),
      gz_path_logical_path_(index::store::internal::get_logical_path(gz_path)),
      index_path_(index::normalize_index_root(idx_path)),
      ckpt_size_(ckpt_size),
      force_rebuild_(force_rebuild),
      cached_is_valid_(false),
      cached_max_bytes_(0),
      cached_max_bytes_ready_(false),
      cached_line_count_(0),
      cached_line_count_ready_(false),
      cached_checkpoint_size_(0),
      cached_checkpoint_size_ready_(false) {
    if (gz_path_.empty()) {
        throw index::store::IndexerError(
            index::store::IndexerError::Type::InvalidArgument,
            "gz_path must not be empty");
    }

    if (!common::input_exists(gz_path_)) {
        throw index::store::IndexerError(
            index::store::IndexerError::Type::FileError,
            "gz_path does not exist: " + gz_path_);
    }

    if (ckpt_size_ == 0) {
        throw index::store::IndexerError(
            index::store::IndexerError::Type::InvalidArgument,
            "ckpt_size must be greater than 0");
    }

    open();
}

GzipIndexer::~GzipIndexer() {
    DFTRACER_UTILS_LOG_DEBUG("Destroying GZIP indexer for %s",
                             gz_path_.c_str());
    close();
}

GzipIndexer::GzipIndexer(GzipIndexer&& other) noexcept
    : gz_path_(std::move(other.gz_path_)),
      gz_path_logical_path_(std::move(other.gz_path_logical_path_)),
      index_path_(std::move(other.index_path_)),
      ckpt_size_(other.ckpt_size_),
      force_rebuild_(other.force_rebuild_),
      visitors_(std::move(other.visitors_)),
      cached_is_valid_(other.cached_is_valid_.load()),
      cached_max_bytes_(other.cached_max_bytes_.load()),
      cached_max_bytes_ready_(other.cached_max_bytes_ready_.load()),
      cached_line_count_(other.cached_line_count_.load()),
      cached_line_count_ready_(other.cached_line_count_ready_.load()),
      cached_checkpoint_size_(other.cached_checkpoint_size_.load()),
      cached_checkpoint_size_ready_(other.cached_checkpoint_size_ready_.load()),
      cached_members_(std::move(other.cached_members_)),
      snapshot_(std::move(other.snapshot_)),
      file_ref_(std::move(other.file_ref_)),
      cached_loaded_(other.cached_loaded_.load()) {}

GzipIndexer& GzipIndexer::operator=(GzipIndexer&& other) noexcept {
    if (this != &other) {
        gz_path_ = std::move(other.gz_path_);
        gz_path_logical_path_ = std::move(other.gz_path_logical_path_);
        index_path_ = std::move(other.index_path_);
        ckpt_size_ = other.ckpt_size_;
        force_rebuild_ = other.force_rebuild_;
        visitors_ = std::move(other.visitors_);
        cached_is_valid_.store(other.cached_is_valid_.load());
        cached_max_bytes_.store(other.cached_max_bytes_.load());
        cached_max_bytes_ready_.store(other.cached_max_bytes_ready_.load());
        cached_line_count_.store(other.cached_line_count_.load());
        cached_line_count_ready_.store(other.cached_line_count_ready_.load());
        cached_checkpoint_size_.store(other.cached_checkpoint_size_.load());
        cached_checkpoint_size_ready_.store(
            other.cached_checkpoint_size_ready_.load());
        cached_loaded_.store(other.cached_loaded_.load());
        std::lock_guard<std::mutex> lock(cached_members_mutex_);
        cached_members_ = std::move(other.cached_members_);
        snapshot_ = std::move(other.snapshot_);
        file_ref_ = std::move(other.file_ref_);
    }
    return *this;
}

void GzipIndexer::open() {}

void GzipIndexer::close() {}

dftracer::utils::coro::CoroTask<void> GzipIndexer::build_async() const {
    if (!force_rebuild_ && !need_rebuild()) {
        co_return;
    }

    const std::time_t mtime =
        index::store::internal::get_file_modification_time(gz_path_);
    const auto hash = index::store::internal::calculate_file_hash(gz_path_);
    const std::uint64_t bytes =
        index::store::internal::file_size_bytes(gz_path_);
    const std::uint64_t final_ckpt_size =
        determine_checkpoint_size(ckpt_size_, gz_path_);
    const dstd::String logical = gz_path_logical_path_;

    dstd::Optional<GzipBuildArtifacts> artifacts;
    co_await coro::with_scope(
        [&](coro::Scope& scope) -> dftracer::utils::coro::CoroTask<void> {
            artifacts = co_await build_gzip_index_artifacts(
                gz_path_, final_ckpt_size, visitors_, &scope);
        });
    if (!artifacts) {
        throw index::store::IndexerError(
            index::store::IndexerError::Type::BuildError,
            "Failed to build index for " + gz_path_);
    }

    dstd::Optional<index::Snapshot> snapshot;
    {
        auto opened = index::Index::open(index_path_);
        bool fresh = false;
        if (opened) {
            auto before = opened->snapshot();
            fresh = before && !before->file(logical).has_value();
        }
        auto build =
            opened ? opened->build(
                         logical,
                         index::SourceStat{static_cast<std::uint64_t>(mtime),
                                           bytes, hash})
                   : dstd::Expected<index::FileBuild, dstd::Error>(
                         dstd::Unexpected<dstd::Error>(opened.error()));
        dstd::Expected<void, dstd::Error> committed = {};
        if (build) {
            persist_gzip_index_artifacts(*build, *artifacts);
            // The new members renumber the chunks the old copies describe. A
            // fresh file has none.
            if (!fresh) index::clear_chunk_copies(*build);
            committed = co_await build->commit();
        } else {
            committed = dstd::Unexpected<dstd::Error>(build.error());
        }
        if (!committed) {
            throw index::store::IndexerError(
                index::store::IndexerError::Type::BuildError,
                "Failed to write the index of " + gz_path_ + ": " +
                    dstd::String(committed.error().message()));
        }
        snapshot.emplace(common::unwrap(opened->snapshot()));
    }

    cached_is_valid_ = true;
    cached_checkpoint_size_ = final_ckpt_size;
    cached_checkpoint_size_ready_ = true;
    const auto file = snapshot->file(logical);
    store::FileMetadataResult metadata;
    if (file) {
        if (auto stored =
                common::unwrap(index::file_metadata(*snapshot, *file)))
            metadata = *stored;
    }
    cached_line_count_ = metadata.line_count;
    cached_line_count_ready_ = true;
    cached_max_bytes_ = metadata.max_bytes;
    cached_max_bytes_ready_ = true;
    std::lock_guard<std::mutex> lock(cached_members_mutex_);
    if (file) {
        auto members = common::unwrap(index::members(*snapshot, *file));
        cached_members_.assign(members.begin(), members.end());
    } else {
        cached_members_.clear();
    }
    file_ref_ = file;
    snapshot_ = std::move(snapshot);
    cached_loaded_.store(true, std::memory_order_release);
    co_return;
}

bool GzipIndexer::is_valid() const { return cached_is_valid_; }

bool GzipIndexer::exists() const {
    return common::unwrap(
        dstd::fs::is_directory(dstd::PathView(index::store_dir(index_path_))));
}

bool GzipIndexer::need_rebuild() const {
    if (is_valid()) {
        return false;
    }
    if (!exists()) {
        return true;
    }

    try {
        auto opened = common::unwrap(index::Index::open(index_path_));
        auto snapshot = common::unwrap(opened.snapshot());
        const auto file = snapshot.file(gz_path_logical_path_);
        if (!file) {
            return true;
        }
        index::SourceStat now;
        dftracer::utils::common::InputFileStat st;
        if (common::stat_input(gz_path_, st)) {
            now.mtime = static_cast<std::uint64_t>(st.mtime);
            now.size = st.size;
        }
        if (snapshot.freshness(gz_path_logical_path_, now) !=
            index::Freshness::Current) {
            return true;
        }
        // The stat check misses a same-size rewrite within one mtime second.
        const auto stored_hash = index::file_hash(snapshot, *file);
        if (!stored_hash) {
            return true;
        }
        const auto metadata =
            common::unwrap(index::file_metadata(snapshot, *file));
        if (!metadata || metadata->checkpoint_size == 0) {
            return true;
        }
        return index::store::internal::calculate_file_hash(gz_path_) !=
               *stored_hash;
    } catch (const std::exception& e) {
        // An unreadable index is stale because the rebuild rewrites it.
        DFTRACER_UTILS_LOG_WARN("Indexer: index %s is unreadable (%s); rebuild",
                                index_path_.c_str(), e.what());
        return true;
    }
}

const dstd::String& GzipIndexer::index_path() const { return index_path_; }

const dstd::String& GzipIndexer::archive_path() const { return gz_path_; }

const dstd::String& GzipIndexer::gz_path() const { return gz_path_; }

void GzipIndexer::ensure_loaded() const {
    if (cached_loaded_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(cached_members_mutex_);
    if (cached_loaded_.load(std::memory_order_relaxed)) {
        return;
    }
    auto opened = common::unwrap(index::Index::open(index_path_));
    auto snapshot = common::unwrap(opened.snapshot());
    const auto file = snapshot.file(gz_path_logical_path_);
    if (file) {
        store::FileMetadataResult metadata;
        if (auto stored = common::unwrap(index::file_metadata(snapshot, *file)))
            metadata = *stored;
        cached_line_count_.store(metadata.line_count,
                                 std::memory_order_relaxed);
        cached_line_count_ready_.store(true, std::memory_order_relaxed);
        cached_max_bytes_.store(metadata.max_bytes, std::memory_order_relaxed);
        cached_max_bytes_ready_.store(true, std::memory_order_relaxed);
        cached_checkpoint_size_.store(metadata.checkpoint_size,
                                      std::memory_order_relaxed);
        cached_checkpoint_size_ready_.store(true, std::memory_order_relaxed);
        auto members = common::unwrap(index::members(snapshot, *file));
        cached_members_.assign(members.begin(), members.end());
        for (const auto& m : cached_members_)
            if (m.kind == GzipRecordKind::Head)
                cached_heads_.push_back(m.c_offset);
    }
    file_ref_ = file;
    snapshot_ = std::move(snapshot);
    cached_loaded_.store(true, std::memory_order_release);
}

std::uint64_t GzipIndexer::max_bytes() const {
    ensure_loaded();
    return cached_max_bytes_.load(std::memory_order_relaxed);
}

std::uint64_t GzipIndexer::checkpoint_size() const {
    ensure_loaded();
    return cached_checkpoint_size_.load(std::memory_order_relaxed);
}

std::uint64_t GzipIndexer::line_count() const {
    ensure_loaded();
    return cached_line_count_.load(std::memory_order_relaxed);
}

bool GzipIndexer::find_member(std::size_t target_offset,
                              GzipMemberRecord& member) const {
    ensure_loaded();
    const auto at = std::partition_point(
        cached_members_.begin(), cached_members_.end(),
        [target_offset](const GzipMemberRecord& candidate) {
            return target_offset >= candidate.uc_offset + candidate.uc_size;
        });
    if (at == cached_members_.end()) return false;
    member = *at;
    return true;
}

dstd::Vector<GzipMemberRecord> GzipIndexer::members() const {
    ensure_loaded();
    return cached_members_;
}

dstd::Span<const std::uint64_t> GzipIndexer::piece_heads() const {
    ensure_loaded();
    return cached_heads_;
}

std::uint64_t GzipIndexer::compressed_end() const {
    ensure_loaded();
    return file_ref_ ? file_ref_->stat().size : 0;
}

dstd::String GzipIndexer::restart_window(std::uint64_t member_idx) const {
    ensure_loaded();
    if (!file_ref_) return {};
    auto window = common::unwrap(
        index::restart_window(*snapshot_, *file_ref_, member_idx));
    if (!window) return {};
    return dstd::String(window->data(), window->size());
}

}  // namespace dftracer::utils::index::gzip
