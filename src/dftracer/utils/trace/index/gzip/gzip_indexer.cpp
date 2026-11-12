// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
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
#include <dftracer/utils/fileio/compress/gzip_stream_inflater.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/trace/index/build/index_visitor.h>
#include <dftracer/utils/trace/index/gzip/checkpoint_size.h>
#include <dftracer/utils/trace/index/gzip/gzip_indexer.h>
#include <dftracer/utils/trace/index/gzip/gzip_member_scanner.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/store/error.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/index/tables.h>
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

namespace dftracer::utils::trace::index::gzip {

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
    // The first message of a range that found its own start: member_c_end is
    // the offset of its first header.
    bool range_start = false;
    // The member ended where it could not be inflated further: a record that a
    // reader streams from the header and stops reading in, not one it decodes
    // whole.
    bool damaged = false;
    // With damaged: the deflate data was whole and its trailer did not match.
    bool checksum_failed = false;
};

using ParallelChan = dftracer::utils::coro::Channel<ParallelInflateMsg>;

// The bytes a message holds against the budget of its range.
static std::uint64_t message_charge(const ParallelInflateMsg& msg,
                                    std::uint64_t cap) noexcept {
    return msg.data ? std::min<std::uint64_t>(msg.data_size, cap) : 0;
}

// A worker's end of its range's channel. With a budget, a message that carries
// text waits until the dispatcher has consumed enough earlier text of the
// range, so the plaintext a range holds is bounded by bytes, not by messages.
struct Outbox {
    dftracer::utils::coro::ChannelProducer<ParallelInflateMsg> producer;
    dstd::SharedPtr<dftracer::utils::coro::AsyncSemaphore> budget;
    std::uint64_t cap = 0;

    auto guard() { return producer.guard(); }

    dftracer::utils::coro::CoroTask<bool> send(ParallelInflateMsg msg) {
        const std::uint64_t charge = budget ? message_charge(msg, cap) : 0;
        if (charge > 0) co_await budget->acquire(charge);
        const auto ok = co_await producer.send(std::move(msg));
        co_return static_cast<bool>(ok);
    }
};

using Bytes = dftracer::utils::dstd::RawBuffer<unsigned char>;

static void drop_front(Bytes& b, std::size_t n) noexcept {
    if (n == 0) return;
    std::memmove(b.data(), b.data() + n, b.size() - n);
    b.resize(b.size() - n);
}

// Compressed bytes of the file held by one worker: the file bytes
// [off, off + data.size()), of which the first `pos` are consumed. A member
// that ends inside the buffer leaves the bytes after it for the next member,
// so no input byte is read twice by one worker.
struct ReadBuf {
    Bytes data;
    std::uint64_t off = 0;
    std::size_t pos = 0;

    std::size_t avail() const noexcept { return data.size() - pos; }
    const unsigned char* cur() const noexcept { return data.data() + pos; }
    std::uint64_t end_off() const noexcept { return off + data.size(); }

    // Moves to file offset `c`, keeping the held bytes from there on.
    void seek(std::uint64_t c) {
        if (c >= off && c <= end_off()) {
            pos = static_cast<std::size_t>(c - off);
        } else {
            data.clear();
            off = c;
            pos = 0;
        }
    }

    void compact() {
        if (pos == 0) return;
        std::memmove(data.data(), data.data() + pos, avail());
        data.resize(avail());
        off += pos;
        pos = 0;
    }
};

// Appends up to `want` bytes that follow the held bytes. Returns the bytes
// read, 0 at end of file, -1 on a read error.
static dftracer::utils::coro::CoroTask<ssize_t> read_more(
    int fd, ReadBuf& in, std::uint64_t file_size, std::size_t want) {
    const std::uint64_t at = in.end_off();
    if (at >= file_size) co_return 0;
    want =
        static_cast<std::size_t>(std::min<std::uint64_t>(want, file_size - at));
    in.compact();
    const std::size_t old = in.data.size();
    in.data.resize(old + want);
    const ssize_t n = co_await dftracer::utils::runtime::io::pread(
        fd, in.data.data() + old, want, at);
    if (n <= 0) {
        in.data.resize(old);
        co_return -1;
    }
    in.data.resize(old + static_cast<std::size_t>(n));
    co_return n;
}

// A member that crosses its range end is read past it in steps that double
// from the first to the last size, so the bytes read past its end, which the
// next range's worker reads too, stay few.
constexpr std::size_t PAST_END_FIRST_STEP = std::size_t{8} << 10;
constexpr std::size_t PAST_END_LAST_STEP = std::size_t{64} << 10;

// Decode the gzip member at compressed offset `c_off` into `out`, reading it
// through `in`, which the caller reuses. Neither buffer is zeroed. Returns
// the compressed bytes the member consumed, or nullopt on read/decode failure
// or when `stream` is set: the member inflates past `max_out`, or past the
// output held when `stream_input` bytes of it are buffered, or it is not whole
// in the bytes before `read_limit`, or its data is invalid at the end of the
// file, so the caller streams it from `in` and keeps what precedes the damage.
// Peak memory is one member.
static dftracer::utils::coro::CoroTask<dstd::Optional<std::uint64_t>>
decode_member_at(int fd, std::uint64_t c_off, std::uint64_t file_size,
                 std::uint64_t read_limit,
                 const fileio::compress::GzipMemberDecompressor& dec,
                 Bytes& out, ReadBuf& in, std::size_t& out_size,
                 bool& truncated, std::size_t max_out, std::size_t stream_input,
                 bool& stream) {
    truncated = false;
    stream = false;
    constexpr std::size_t READ_CHUNK = 1u << 20;
    if (out.size() < (1u << 20)) out.resize(1u << 20);
    in.seek(c_off);
    fileio::compress::DecompressResult res{};
    while (true) {
        if (in.avail() > 0) {
            const std::size_t out_cap = std::min(out.size(), max_out);
            const fileio::compress::GzipDecode status = dec.decompress_status(
                in.cur(), in.avail(), out.data(), out_cap, res);
            if (status == fileio::compress::GzipDecode::Ok) break;
            if (status == fileio::compress::GzipDecode::InsufficientSpace) {
                if (out_cap >= max_out || in.avail() >= stream_input) {
                    stream = true;
                    co_return dstd::nullopt;
                }
                out.resize(std::min(out.size() * 2, max_out));
                continue;
            }
            // BadData may just mean the member is not fully buffered yet.
        }
        const std::uint64_t at = in.end_off();
        if (at >= file_size) {
            if (in.avail() == 0) co_return dstd::nullopt;
            dstd::Vector<std::uint8_t> cut;
            auto keep = fileio::compress::decode_truncated_member(
                in.cur(), in.avail(), cut);
            if (!keep) {
                stream = true;
                co_return dstd::nullopt;
            }
            out.resize(cut.size());
            std::memcpy(out.data(), cut.data(), cut.size());
            out_size = *keep;
            truncated = true;
            const std::uint64_t used = in.avail();
            in.pos = in.data.size();
            co_return used;
        }
        if (at >= read_limit) {
            stream = true;
            co_return dstd::nullopt;
        }
        // libdeflate restarts the member on each retry, so at least double
        // the buffered input per retry to keep a large member linear.
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(
                std::max(READ_CHUNK, in.avail()), read_limit - at));
        const ssize_t got = co_await read_more(fd, in, file_size, want);
        if (got <= 0) co_return dstd::nullopt;
    }
    out_size = res.out_bytes;
    in.pos += static_cast<std::size_t>(res.in_bytes);
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
// A member that has this much compressed input buffered and still does not fit
// the output is streamed too, so a worker does not grow its output to the
// bound for each large member.
constexpr std::size_t STREAM_INPUT_BYTES = std::size_t{256} << 10;

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

// The same for a worker that has no candidate list: past a short run of zero
// bytes that reaches a valid member header or the end of the file. The bytes
// come from `in` when it holds them, and a short read stands in when it does
// not.
static dftracer::utils::coro::CoroTask<std::uint64_t> next_member_start(
    int fd, ReadBuf& in, std::uint64_t file_size, std::uint64_t end) {
    if (end >= file_size) co_return end;
    in.seek(end);
    unsigned char peek[MAX_ZERO_GAP + 10];
    const unsigned char* p = in.cur();
    std::size_t n = in.avail();
    if (n < sizeof peek && in.end_off() < file_size) {
        if (n > 0 && p[0] != 0) co_return end;
        n = static_cast<std::size_t>(
            std::min<std::uint64_t>(sizeof peek, file_size - end));
        const ssize_t got =
            co_await dftracer::utils::runtime::io::pread(fd, peek, n, end);
        if (got != static_cast<ssize_t>(n)) co_return end;
        p = peek;
    }
    std::size_t zeros = 0;
    while (zeros < n && zeros <= MAX_ZERO_GAP && p[zeros] == 0) ++zeros;
    if (zeros == 0 || zeros > MAX_ZERO_GAP) co_return end;
    if (end + zeros == file_size) co_return end + zeros;
    if (n >= zeros + 10 && gzip_header_looks_valid(p + zeros))
        co_return end + zeros;
    co_return end;
}

static dftracer::utils::coro::CoroTask<std::uint64_t> following_member_start(
    int fd, ReadBuf& in, std::uint64_t file_size,
    const dstd::Vector<GzipMember>* candidates, std::uint64_t range_c_end,
    std::uint64_t end) {
    if (candidates != nullptr)
        co_return co_await next_member_start(fd, *candidates, end, range_c_end);
    co_return co_await next_member_start(fd, in, file_size, end);
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
// is gone or the member was cut with nothing to keep. `corrupt` is set when the
// member could not be inflated to its end: the text it gave is kept as
// records, the last of which a reader serves before the damage is reported, and
// nothing is sent when it gave nothing. The input starts with the
// bytes `in` holds at `c_off`, and `in` holds what follows the member on
// return.
static dftracer::utils::coro::CoroTask<bool> stream_member(
    int fd, std::uint64_t c_off, std::uint64_t file_size,
    std::uint64_t read_limit, std::uint64_t ckpt_size,
    const dstd::Vector<GzipMember>* candidates, std::uint64_t range_c_end,
    bool strip_leading_partial, bool ship_bytes, InflateBuffers& buffers,
    Outbox& producer, ReadBuf& in, std::uint64_t& in_bytes, bool& stop,
    bool& corrupt) {
    constexpr std::size_t READ_CHUNK = 1u << 20;
    constexpr std::size_t OUT_STEP = 1u << 20;
    const std::size_t split_at = static_cast<std::size_t>(
        std::max<std::uint64_t>(ckpt_size, RESTART_WINDOW));

    fileio::compress::GzipStreamInflater gz;
    if (!gz.init(31)) co_return false;

    in.seek(c_off);
    InflateBuffer buf = buffers.take();
    buf->clear();
    // Growing a piece by doubling would leave a buffer twice the piece.
    buf->reserve(SHIP_BYTES + OUT_STEP);
    std::uint64_t next_read = in.end_off();
    std::size_t past_step = PAST_END_FIRST_STEP;
    if (in.avail() > 0) gz.set_input(in.cur(), in.avail());
    bool strip = strip_leading_partial;
    bool cut = false;
    bool broken = false;
    // Text of this member was sent that no record holds yet.
    bool uncommitted = false;
    bool has_record = false;
    std::uint64_t since_newline = 0;
    std::uint64_t since_restart = 0;
    // The last RESTART_WINDOW bytes of output already shipped, for the window
    // of a restart point whose piece holds less than that.
    dstd::String shipped_tail;
    stop = false;
    corrupt = false;

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
            std::uint64_t want =
                std::min<std::uint64_t>(READ_CHUNK, file_size - next_read);
            if (next_read < read_limit) {
                want = std::min(want, read_limit - next_read);
            } else {
                want = std::min<std::uint64_t>(want, past_step);
                past_step = std::min(past_step * 2, PAST_END_LAST_STEP);
            }
            in.data.clear();
            in.off = next_read;
            in.pos = 0;
            const ssize_t n = co_await read_more(
                fd, in, file_size, static_cast<std::size_t>(want));
            if (n <= 0) co_return false;
            gz.set_input(in.cur(), in.avail());
            next_read += static_cast<std::uint64_t>(n);
        }

        Bytes& piece = *buf;
        const std::size_t old = piece.size();
        piece.resize(old + OUT_STEP);
        gz.set_output(piece.data() + old, OUT_STEP);
        const fileio::compress::InflateStatus rc =
            gz.inflate(fileio::compress::InflateFlush::Block);
        piece.resize(old + (OUT_STEP - gz.avail_out()));
        since_restart += piece.size() - old;

        if (rc == fileio::compress::InflateStatus::StreamEnd) break;
        if (rc != fileio::compress::InflateStatus::Ok &&
            rc != fileio::compress::InflateStatus::BufError) {
            broken = true;
            break;
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
            uncommitted = false;
            has_record = true;
        } else if (piece.size() >= SHIP_BYTES) {
            ParallelInflateMsg msg;
            emit(msg);
            const auto send_ok = co_await producer.send(std::move(msg));
            if (!send_ok) {
                stop = true;
                co_return true;
            }
            uncommitted = true;
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
    } else if (broken) {
        if (piece.empty() && !uncommitted && !has_record) {
            buffers.give(std::move(buf));
            corrupt = true;
            co_return true;
        }
        msg.truncated = true;
        msg.damaged = true;
        msg.checksum_failed = gz.trailer_mismatch();
        in_bytes = gz.total_in();
        corrupt = true;
    } else {
        in_bytes = gz.total_in();
        in.pos = in.data.size() - gz.avail_in();
    }
    emit(msg);
    buffers.give(std::move(buf));
    msg.member_ended = true;
    // A damaged member's text ends where the damage is, with or without a
    // newline, and the next record starts a line of its own.
    msg.has_open_line = since_newline > 0 && !broken;
    msg.member_c_end = c_off + in_bytes;
    if (!corrupt) {
        msg.member_c_end = co_await following_member_start(
            fd, in, file_size, candidates, range_c_end, c_off + in_bytes);
    }
    const auto send_ok = co_await producer.send(std::move(msg));
    if (!send_ok) stop = true;
    co_return true;
}

// Looks for the first gzip header that starts in [from, limit) and leaves `in`
// positioned at it with the bytes read, so the decode that follows does not
// read them again. `found` is false when there is none. False when a read
// fails.
static dftracer::utils::coro::CoroTask<bool> find_range_start(
    int fd, std::uint64_t file_size, std::uint64_t from, std::uint64_t limit,
    ReadBuf& in, std::uint64_t& start, bool& found) {
    constexpr std::size_t SEARCH_WINDOW = std::size_t{1} << 20;
    constexpr std::size_t HEADER_BYTES = 10;
    found = false;
    in.data.clear();
    in.off = from;
    in.pos = 0;
    const std::uint64_t target_end = std::min(file_size, limit + 9);
    std::uint64_t at = from;
    while (at < limit && at + HEADER_BYTES <= file_size) {
        in.seek(at);
        if (in.avail() < HEADER_BYTES) {
            const std::uint64_t have = in.end_off();
            if (have >= target_end) break;
            const ssize_t n = co_await read_more(
                fd, in, file_size,
                static_cast<std::size_t>(
                    std::min<std::uint64_t>(SEARCH_WINDOW, target_end - have)));
            if (n <= 0) co_return false;
            continue;
        }
        const unsigned char* base = in.cur();
        const std::uint64_t last =
            std::min<std::uint64_t>(limit, at + in.avail() - 9);
        const unsigned char* p = base;
        const unsigned char* stop = base + (last - at);
        while (p < stop) {
            p = static_cast<const unsigned char*>(
                std::memchr(p, 0x1F, static_cast<std::size_t>(stop - p)));
            if (!p) break;
            if (gzip_header_looks_valid(p)) {
                start = at + static_cast<std::uint64_t>(p - base);
                found = true;
                in.seek(start);
                co_return true;
            }
            ++p;
        }
        at = last;
    }
    co_return true;
}

// Each worker owns a member-aligned compressed range and decodes its members
// one at a time with libdeflate, emitting one message per member. strip and
// extend handle lines that straddle a slice boundary in the distributed
// (member_begin > 0) case; single-process workers use neither and rely on the
// dispatcher accumulator to reassemble lines across worker boundaries.
//
// With `candidates` the range starts at a known header. Without them
// (`candidates` null) the range starts at `range_c_start`, and with
// `search_to` set the worker first looks for the first header in
// [range_c_start, search_to) and reports where it is in a range_start message;
// when there is none it ends the range with no message.
static dftracer::utils::coro::CoroTask<bool> parallel_worker(
    int fd, std::uint64_t file_size, std::uint64_t range_c_start,
    std::uint64_t range_c_end, std::uint64_t search_to, std::uint64_t ckpt_size,
    const dstd::Vector<GzipMember>* candidates, bool strip_leading_partial,
    bool extend_to_newline_past_end, bool ship_bytes, InflateBuffers& buffers,
    std::atomic<std::uint64_t>* member_hint, Outbox producer) {
    auto guard = producer.guard();

    fileio::compress::GzipMemberDecompressor dec;
    if (!dec.valid()) co_return false;

    InflateBuffer buf = buffers.take();
    ReadBuf in;
    std::uint64_t c_off = range_c_start;
    bool first = true;
    // Past a range end only a member that began before it is read to its end.
    const std::uint64_t read_limit =
        candidates != nullptr ? file_size : range_c_end;

    if (search_to > 0) {
        std::uint64_t start = 0;
        bool found = false;
        // The first range may look past its end, but only after its own bytes.
        const std::uint64_t own_to = std::min(search_to, range_c_end);
        bool searched = co_await find_range_start(fd, file_size, range_c_start,
                                                  own_to, in, start, found);
        if (searched && !found && search_to > own_to)
            searched = co_await find_range_start(fd, file_size, own_to,
                                                 search_to, in, start, found);
        if (!searched) co_return false;
        // The file start is the start of the first range, whatever is there.
        if (!found && range_c_start == 0) {
            start = 0;
            found = true;
            in.seek(0);
        }
        if (!found) co_return true;
        ParallelInflateMsg msg;
        msg.range_start = true;
        msg.member_c_end = start;
        const bool send_ok = co_await producer.send(std::move(msg));
        if (!send_ok) co_return true;
        c_off = start;
    }

    auto note_member = [&](std::uint64_t compressed) {
        if (member_hint == nullptr) return;
        std::uint64_t seen = member_hint->load(std::memory_order_relaxed);
        while (seen < compressed &&
               !member_hint->compare_exchange_weak(seen, compressed,
                                                   std::memory_order_relaxed)) {
        }
    };

    // After a member that cannot be read, looks for the next header in the
    // range and tells the dispatcher where it is, since the bytes between are
    // in no record. False when a read fails. `c_off` is at the range end when
    // there is no header left.
    auto resync =
        [&](std::uint64_t bad) -> dftracer::utils::coro::CoroTask<bool> {
        std::uint64_t start = 0;
        bool found = false;
        const bool searched = co_await find_range_start(
            fd, file_size, bad + 1, range_c_end, in, start, found);
        if (!searched) co_return false;
        if (!found) {
            DFTRACER_UTILS_LOG_WARN(
                "Indexer: {} bytes from offset {} are not a gzip member and "
                "no member follows in this range; they are not indexed.",
                static_cast<unsigned long long>(range_c_end - bad),
                static_cast<unsigned long long>(bad));
            c_off = range_c_end;
            co_return true;
        }
        DFTRACER_UTILS_LOG_WARN(
            "Indexer: skipped {} bytes at offset {} that no readable gzip "
            "member covers.",
            static_cast<unsigned long long>(start - bad),
            static_cast<unsigned long long>(bad));
        ParallelInflateMsg msg;
        msg.range_start = true;
        msg.member_c_end = start;
        const bool sent = co_await producer.send(std::move(msg));
        c_off = sent ? start : range_c_end;
        co_return true;
    };

    while (c_off < range_c_end) {
        const GzipMember* known = nullptr;
        std::uint64_t span = 0;
        if (candidates != nullptr) {
            const auto next =
                std::upper_bound(candidates->begin(), candidates->end(), c_off,
                                 [](std::uint64_t v, const GzipMember& m) {
                                     return v < m.c_offset;
                                 });
            span = (next != candidates->end()
                        ? std::min(next->c_offset, range_c_end)
                        : range_c_end) -
                   c_off;
            known = next != candidates->begin() && next != candidates->end() &&
                            std::prev(next)->c_offset == c_off
                        ? &*std::prev(next)
                        : nullptr;
        }
        // A member larger than a piece is streamed. The header scan gives its
        // size (ISIZE) when the trailer can be trusted. Otherwise the decode
        // below stops at the bound and the member is streamed from its start.
        const std::size_t bound =
            std::min(static_cast<std::size_t>(
                         std::max<std::uint64_t>(ckpt_size, RESTART_WINDOW)),
                     WHOLE_MEMBER_BYTES);
        const bool size_known =
            known != nullptr &&
            known->isize <= known->c_size * MAX_DEFLATE_RATIO;
        bool stream_it =
            candidates != nullptr &&
            (span >= ckpt_size / 8 || (size_known && known->isize > bound));
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
            bool needs_stream = false;
            in_bytes = co_await decode_member_at(
                fd, c_off, file_size, read_limit, dec, out, in, out_size,
                truncated, bound, STREAM_INPUT_BYTES, needs_stream);
            if (needs_stream) {
                stream_it = true;
            } else if (!in_bytes) {
                if (candidates != nullptr) co_return false;
                const bool resynced = co_await resync(c_off);
                if (!resynced) co_return false;
                if (c_off >= range_c_end) break;
                continue;
            }
        }
        if (stream_it) {
            std::uint64_t consumed = 0;
            bool stop = false;
            bool corrupt = false;
            const auto stream_member_ok = co_await stream_member(
                fd, c_off, file_size, read_limit, ckpt_size, candidates,
                range_c_end, strip_leading_partial && first, ship_bytes,
                buffers, producer, in, consumed, stop, corrupt);
            if (!stream_member_ok) {
                if (candidates != nullptr) co_return false;
                const bool resynced = co_await resync(c_off);
                if (!resynced) co_return false;
                if (c_off >= range_c_end) break;
                continue;
            }
            if (stop) co_return true;
            first = false;
            if (corrupt) {
                if (candidates != nullptr) co_return false;
                const bool resynced = co_await resync(c_off);
                if (!resynced) co_return false;
                if (c_off >= range_c_end) break;
                continue;
            }
            note_member(consumed);
            c_off = co_await following_member_start(
                fd, in, file_size, candidates, range_c_end, c_off + consumed);
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
        note_member(*in_bytes);
        msg.member_c_end = co_await following_member_start(
            fd, in, file_size, candidates, range_c_end, c_off + *in_bytes);
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
        bool needs_stream = false;
        auto in_bytes = co_await decode_member_at(
            fd, range_c_end, file_size, file_size, dec, out, in, out_size,
            truncated, std::numeric_limits<std::size_t>::max(),
            std::numeric_limits<std::size_t>::max(), needs_stream);
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

// Ranges of `range_bytes` compressed bytes cut without looking at the file;
// each worker finds the first header at or after its range start.
static dstd::Vector<InflateRange> cut_ranges(std::uint64_t file_size,
                                             std::uint64_t range_bytes) {
    dstd::Vector<InflateRange> ranges;
    std::uint64_t at = 0;
    do {
        const std::uint64_t end = std::min(at + range_bytes, file_size);
        ranges.push_back({at, end, 0});
        at = end;
    } while (at < file_size);
    return ranges;
}

// A range holds a few members' worth of compressed bytes. A later range
// looks for its first header in its own bytes; what it passes over is the end
// of a member that the range before reads too. While no member is known it
// looks a quarter of a range in, and when the members are longer than a range
// only a short way (a member then hides the headers of the ranges inside it,
// and the dispatcher inflates the rest of those ranges after the member).
constexpr std::uint64_t RANGE_CKPT_DIVISOR = 2;
constexpr std::uint64_t MIN_RANGE_BYTES = std::uint64_t{64} << 10;
constexpr std::uint64_t PROBE_RANGE_DIVISOR = 64;
constexpr std::uint64_t MIN_PROBE_BYTES = std::uint64_t{4} << 10;
constexpr std::uint64_t MIN_BYTE_CAP = std::uint64_t{4} << 20;

static std::uint64_t lazy_range_bytes(std::uint64_t ckpt_size) {
    const auto set = common::Env::get<std::uint64_t>(
        common::constants::indexer::INDEX_RANGE_BYTES_ENV);
    if (set && *set > 0) return std::max(*set, MIN_RANGE_BYTES);
    return std::max(ckpt_size / RANGE_CKPT_DIVISOR,
                    common::constants::indexer::INDEX_RANGE_MIN_BYTES);
}

// A message without bytes is small, so a worker may run far ahead of the
// dispatcher when nothing reads the text.
constexpr std::size_t UNSHIPPED_MESSAGES = 1024;

struct RangeSlot {
    dstd::SharedPtr<ParallelChan> chan;
    dstd::SharedPtr<dftracer::utils::coro::AsyncSemaphore> budget;
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
    // The headers the ranges start at; empty when the ranges are cut by
    // compressed size and each worker finds its first header (lazy).
    dstd::Vector<GzipMember> members;
    bool lazy = false;
    std::uint64_t range_bytes = 0;
    // The text of one range that the dispatcher has not consumed, at most (a
    // message above it goes alone), as the ranges cut by ISIZE hold.
    std::uint64_t byte_cap = 0;
    // Compressed size of the longest member a worker finished, for how far a
    // later range looks for its first header.
    std::atomic<std::uint64_t> member_hint{0};
    dstd::Vector<InflateRange> ranges;
    dftracer::utils::coro::AsyncSemaphore permits;
    dstd::Vector<RangeSlot> ring;
    InflateBuffers buffers;
    std::atomic<std::size_t> next{0};

    const dstd::Vector<GzipMember>* candidates() const {
        return lazy ? nullptr : &members;
    }
    // How far into range `r` its worker looks for a first header: all the way
    // for the first range, which must start at the first one. The bytes a
    // later range passes over before it finds one are the end of a member that
    // the range before reads too, so a range looks only a short way when the
    // members are longer than a range (it would pass over a whole range of
    // one), and a quarter of a range until a member is known.
    std::uint64_t search_to(std::size_t r) const {
        if (!lazy) return 0;
        if (r == 0) return file_size;
        const std::uint64_t hint = member_hint.load(std::memory_order_relaxed);
        std::uint64_t reach = range_bytes / 4;
        if (hint > 0)
            reach = hint <= range_bytes
                        ? range_bytes
                        : std::max(range_bytes / PROBE_RANGE_DIVISOR,
                                   MIN_PROBE_BYTES);
        return std::min(ranges[r].c_start + reach, ranges[r].c_end);
    }
    dstd::SharedPtr<dftracer::utils::coro::AsyncSemaphore> make_budget() const {
        if (!lazy || !ship_bytes) return nullptr;
        return dstd::make_shared<dftracer::utils::coro::AsyncSemaphore>(
            byte_cap);
    }
    dstd::SharedPtr<ParallelChan> channel(std::size_t r) const {
        return dftracer::utils::coro::make_channel<ParallelInflateMsg>(
            ship_bytes && !lazy ? ranges[r].messages + 1 : UNSHIPPED_MESSAGES);
    }
    void open(std::size_t r) {
        RangeSlot& slot = ring[r % ring.size()];
        slot.chan = channel(r);
        slot.budget = make_budget();
        slot.producer.emplace(slot.chan->producer());
    }
    bool extend(std::size_t r) const {
        return extend_last && r + 1 == ranges.size();
    }
    // Ends every worker when the dispatcher stops early: a send to a closed
    // channel fails, and a worker woken by the permits finds no range left.
    void abandon(ParallelChan* redo,
                 dftracer::utils::coro::AsyncSemaphore* redo_budget) {
        next.store(ranges.size());
        for (RangeSlot& slot : ring) {
            if (slot.chan) slot.chan->close();
            if (slot.budget) slot.budget->release(byte_cap);
        }
        if (redo) redo->close();
        if (redo_budget) redo_budget->release(byte_cap);
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
        Outbox outbox{std::move(*slot.producer), slot.budget, plan->byte_cap};
        slot.producer.reset();
        const InflateRange& range = plan->ranges[r];
        co_await parallel_worker(
            plan->fd, plan->file_size, range.c_start, range.c_end,
            plan->search_to(r), plan->ckpt_size, plan->candidates(),
            plan->strip_first && r == 0, plan->extend(r), plan->ship_bytes,
            plan->buffers, &plan->member_hint, std::move(outbox));
    }
}

static dftracer::utils::coro::CoroTask<dstd::Result<bool>> parallel_dispatcher(
    coro::Scope* scope, InflatePlan* plan, std::uint64_t member_idx_base,
    std::uint64_t& total_lines, std::uint64_t& total_uc_size,
    dstd::Vector<GzipMemberRecord>& members,
    dstd::Vector<dstd::Pair<std::uint64_t, dstd::String>>& restart_windows,
    const CheckpointIndexer::VisitorList& visitors, bool& truncated,
    bool& checksum_failed, std::uint64_t& recovered_bytes,
    bool& has_open_line) {
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
    std::uint64_t member_text_start = 0;

    std::uint64_t total_chunks_received = 0;

    auto process_msg = [&](ParallelInflateMsg& msg)
        -> dftracer::utils::coro::CoroTask<dstd::Result<void>> {
        if (msg.range_start) {
            // Bytes that are in no member lie before this header.
            member_c_start = msg.member_c_end;
            piece_kind = GzipRecordKind::Member;
            piece_bits = 0;
            piece_window.clear();
            member_text_start = global_uc;
            co_return dstd::Result<void>{};
        }
        const std::size_t data_size = static_cast<std::size_t>(msg.data_size);
        ++total_chunks_received;
        if (msg.drop_tail > 0) {
            // The partial line is first in the text no record holds yet.
            std::uint64_t left = msg.drop_tail;
            const std::uint64_t pending =
                std::min(left, global_uc - member_uc_start);
            global_uc -= pending;
            left -= pending;
            const std::uint64_t from_records = left;
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
            global_uc -= from_records - left;
            member_uc_start = std::min(member_uc_start, global_uc);
        }

        if (has_visitors && data_size > 0) {
            const char* data = reinterpret_cast<const char*>(msg.data->data());
            for (auto& v : visitors) {
                auto seen =
                    co_await v.get().on_chunk(data, data_size, member_idx);
                if (!seen) co_return seen;
            }
        }

        if (has_visitors && msg.damaged) {
            for (auto& v : visitors) {
                auto cut = co_await v.get().on_line_cut(member_idx);
                if (!cut) co_return cut;
            }
        }

        global_uc += data_size;
        total_lines += msg.lines;
        if (msg.truncated) {
            truncated = true;
            if (msg.checksum_failed) checksum_failed = true;
            recovered_bytes += global_uc > member_text_start
                                   ? global_uc - member_text_start
                                   : 0;
        }

        if (msg.member_ended) has_open_line = msg.has_open_line;
        if (msg.member_ended || msg.restart) {
            GzipRecordKind kind = piece_kind;
            if ((msg.restart || msg.damaged) &&
                kind == GzipRecordKind::Member) {
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
            if (msg.member_ended) member_text_start = global_uc;
            member_first_line = total_lines + 1;

            if (has_visitors) {
                for (auto& v : visitors) {
                    auto seen = co_await v.get().on_checkpoint(member_idx);
                    if (!seen) co_return seen;
                }
            }
            ++member_idx;
        }

        co_return dstd::Result<void>{};
    };

    auto drain_visitors =
        [&]() -> dftracer::utils::coro::CoroTask<dstd::Result<void>> {
        for (auto& v : visitors) {
            if (v.get().wants_drain()) {
                auto drained = co_await v.get().drain_pending();
                if (!drained) co_return drained;
            }
        }
        co_return dstd::Result<void>{};
    };

    using Budget = dftracer::utils::coro::AsyncSemaphore;
    auto consume = [&](ParallelChan& chan, Budget* budget)
        -> dftracer::utils::coro::CoroTask<dstd::Result<void>> {
        for (;;) {
            auto msg_opt = co_await chan.receive();
            if (!msg_opt) break;
            const std::uint64_t charge =
                budget ? message_charge(*msg_opt, plan->byte_cap) : 0;
            auto processed = co_await process_msg(*msg_opt);
            if (!processed) co_return processed;
            plan->buffers.give(std::move(msg_opt->data));
            if (charge > 0) budget->release(charge);
            auto drained = co_await drain_visitors();
            if (!drained) co_return drained;
        }
        co_return dstd::Result<void>{};
    };
    auto discard =
        [&](ParallelChan& chan,
            Budget* budget) -> dftracer::utils::coro::CoroTask<void> {
        while (true) {
            auto msg_opt = co_await chan.receive();
            if (!msg_opt) co_return;
            const std::uint64_t charge =
                budget ? message_charge(*msg_opt, plan->byte_cap) : 0;
            plan->buffers.give(std::move(msg_opt->data));
            if (charge > 0) budget->release(charge);
        }
    };

    dstd::SharedPtr<ParallelChan> redo;
    dstd::SharedPtr<Budget> redo_budget;
    // Inflates the rest of range `r` from `start`: the end of the member
    // before when the range's own start was not that, or, with `search`, the
    // first header of the range when nothing before reached it.
    auto redo_range = [&](std::size_t r, std::uint64_t start, bool search)
        -> dftracer::utils::coro::CoroTask<dstd::Result<void>> {
        redo = plan->channel(r);
        redo_budget = plan->make_budget();
        scope->spawn_future([plan, r, start, search,
                             outbox = Outbox{redo->producer(), redo_budget,
                                             plan->byte_cap}]() mutable {
            const InflateRange& rg = plan->ranges[r];
            return parallel_worker(plan->fd, plan->file_size, start, rg.c_end,
                                   search ? rg.c_end : 0, plan->ckpt_size,
                                   plan->candidates(), false, plan->extend(r),
                                   plan->ship_bytes, plan->buffers,
                                   &plan->member_hint, std::move(outbox));
        });
        auto consumed = co_await consume(*redo, redo_budget.get());
        if (!consumed) co_return consumed;
        redo.reset();
        redo_budget.reset();
        co_return dstd::Result<void>{};
    };
    auto run_ranges =
        [&]() -> dftracer::utils::coro::CoroTask<dstd::Result<void>> {
        for (std::size_t r = 0; r < plan->ranges.size(); ++r) {
            const InflateRange& range = plan->ranges[r];
            RangeSlot& slot = plan->ring[r % plan->ring.size()];
            auto chan = slot.chan;
            auto budget = slot.budget;
            if (plan->lazy) {
                // The worker names where its first header is. The range is
                // used when that is where the member before ended, or when
                // nothing before reached it. Otherwise the header is false or
                // the range has none, and the rest of the range is inflated
                // from the end of that member. When the member before ended
                // short of the range (a member that could not be read), and
                // this range found no header, the range is searched in full.
                auto first = co_await chan->receive();
                const bool found = first.has_value() && first->range_start;
                const std::uint64_t start = found ? first->member_c_end : 0;
                if (found && (r == 0 || member_c_start < range.c_start ||
                              member_c_start == start)) {
                    member_c_start = start;
                    auto consumed = co_await consume(*chan, budget.get());
                    if (!consumed) co_return consumed;
                } else {
                    co_await discard(*chan, budget.get());
                    if (r > 0 && member_c_start >= range.c_start &&
                        member_c_start < range.c_end) {
                        auto redone =
                            co_await redo_range(r, member_c_start, false);
                        if (!redone) co_return redone;
                    } else if (r > 0 && member_c_start < range.c_start) {
                        auto redone =
                            co_await redo_range(r, range.c_start, true);
                        if (!redone) co_return redone;
                    }
                }
            } else if (member_c_start > range.c_start) {
                // The member before runs past this range's start, so the start
                // is a false candidate: inflate the rest of the range from the
                // end of that member instead.
                co_await discard(*chan, nullptr);
                if (member_c_start < range.c_end) {
                    auto redone = co_await redo_range(r, member_c_start, false);
                    if (!redone) co_return redone;
                }
            } else {
                // A range after one that stopped at a bad member starts at its
                // own first member.
                member_c_start = range.c_start;
                auto consumed = co_await consume(*chan, nullptr);
                if (!consumed) co_return consumed;
            }
            if (r + plan->ring.size() < plan->ranges.size())
                plan->open(r + plan->ring.size());
            plan->permits.release(1);
        }
        co_return dstd::Result<void>{};
    };
    auto ran = co_await run_ranges();
    if (!ran) {
        plan->abandon(redo.get(), redo_budget.get());
        // Visitors finish their own tasks, which may hold their state.
        for (auto& v : visitors) {
            auto flushed = co_await v.get().flush();
            // The first error is the one returned below.
            if (!flushed)
                DFTRACER_UTILS_LOG_WARN(
                    "Indexer: a visitor flush failed after an earlier error: "
                    "{}",
                    flushed.error().format());
        }
        co_return ran.error();
    }

    if (has_visitors) {
        for (auto& v : visitors) {
            auto flushed = co_await v.get().flush();
            if (!flushed) co_return flushed.error();
        }
    }
    total_uc_size = global_uc;
    co_return true;
}

static dftracer::utils::coro::CoroTask<dstd::Result<bool>>
process_chunks_parallel(
    int fd, std::uint64_t slice_c_end, std::uint64_t file_size,
    std::uint64_t ckpt_size, std::size_t ways,
    dstd::Vector<GzipMember> scanned_members, dstd::Vector<InflateRange> ranges,
    std::uint64_t member_idx_base, bool strip_slice_leading_partial,
    std::uint64_t& total_lines, std::uint64_t& total_uc_size,
    dstd::Vector<GzipMemberRecord>& members,
    dstd::Vector<dstd::Pair<std::uint64_t, dstd::String>>& restart_windows,
    const CheckpointIndexer::VisitorList& visitors, bool& truncated,
    bool& checksum_failed, std::uint64_t& recovered_bytes,
    bool& has_open_line) {
    // Fanning out past the thread count only adds channel and scheduling
    // overhead; on a single-threaded drive (coro::RunLoop, the no-executor
    // .get() path) it collapses to one worker.
    constexpr std::size_t DEFAULT_MAX_WORKERS = 16;
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
    plan.lazy = plan.members.empty();
    plan.range_bytes = lazy_range_bytes(ckpt_size);
    plan.byte_cap = std::max(ckpt_size / 4, MIN_BYTE_CAP);
    plan.ranges = std::move(ranges);
    for (std::size_t r = 0; r < plan.ring.size(); ++r) plan.open(r);

    dstd::Result<bool> dispatched = true;
    InflatePlan* plan_ptr = &plan;
    co_await coro::with_scope(
        [&](coro::Scope& child) -> dftracer::utils::coro::CoroTask<void> {
            for (std::size_t w = 0; w < worker_count; ++w)
                child.spawn([plan_ptr]() { return range_worker(plan_ptr); });
            // In the body, not spawned, so its error leaves the scope after the
            // workers are joined.
            dispatched = co_await parallel_dispatcher(
                &child, plan_ptr, member_idx_base, total_lines, total_uc_size,
                members, restart_windows, visitors, truncated, checksum_failed,
                recovered_bytes, has_open_line);
        });

    co_return dispatched;
}

static dftracer::utils::coro::CoroTask<dstd::Result<bool>> process_chunks(
    int fd, const GzipMemberSlice* slice, std::uint64_t ckpt_size,
    std::size_t ways, std::uint64_t& total_lines, std::uint64_t& total_uc_size,
    dstd::Vector<GzipMemberRecord>& members,
    dstd::Vector<dstd::Pair<std::uint64_t, dstd::String>>& restart_windows,
    const CheckpointIndexer::VisitorList& visitors, bool& truncated,
    bool& checksum_failed, std::uint64_t& recovered_bytes,
    bool& has_open_line) {
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
        dstd::Vector<InflateRange> ranges = plan_ranges(
            sliced, slice_c_end, std::max<std::uint64_t>(ckpt_size / 4, 1));
        co_return co_await process_chunks_parallel(
            fd, slice_c_end, slice->c_end != 0 ? slice->c_end : file_size,
            ckpt_size, ways, std::move(sliced), std::move(ranges),
            /*member_idx_base=*/slice->idx_base + mb,
            /*strip_slice_leading_partial=*/mb > 0, total_lines, total_uc_size,
            members, restart_windows, visitors, truncated, checksum_failed,
            recovered_bytes, has_open_line);
    }

    // The inflate pass fans out over ranges of compressed bytes, and each
    // worker finds its first member header in the bytes it reads to inflate.
    dstd::Vector<GzipMember> no_candidates;
    dstd::Vector<InflateRange> ranges =
        cut_ranges(file_size, lazy_range_bytes(ckpt_size));
    co_return co_await process_chunks_parallel(
        fd, file_size, file_size, ckpt_size, ways, std::move(no_candidates),
        std::move(ranges),
        /*member_idx_base=*/0, /*strip_slice_leading_partial=*/false,
        total_lines, total_uc_size, members, restart_windows, visitors,
        truncated, checksum_failed, recovered_bytes, has_open_line);
}

}  // namespace

dftracer::utils::coro::CoroTask<dstd::Result<GzipBuildArtifacts>>
build_gzip_index_artifacts(dstd::CStringView gz_path, std::uint64_t ckpt_size,
                           const CheckpointIndexer::VisitorList& visitors,
                           coro::Scope* scope, const GzipMemberSlice* slice,
                           std::size_t ways) {
    const dstd::OpenOptions open_options;
    auto opening =
        runtime::io::open_file(dstd::PathView(gz_path), open_options);
    auto file = co_await std::move(opening);
    if (!file) {
        co_return trace::index::store::indexer_error(
            trace::index::store::IndexerErrorKind::BuildError,
            dstd::fmt<"Failed to build index for {}">(gz_path));
    }
    co_return co_await build_gzip_index_artifacts(file->native_handle(),
                                                  gz_path, ckpt_size, visitors,
                                                  scope, slice, ways);
}

dftracer::utils::coro::CoroTask<dstd::Result<GzipBuildArtifacts>>
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
    bool checksum_failed = false;
    std::uint64_t recovered_bytes = 0;
    bool has_open_line = false;
    const auto processed = co_await process_chunks(
        fd, slice, ckpt_size, ways, total_lines, total_uc_size, members,
        restart_windows, visitors, truncated, checksum_failed, recovered_bytes,
        has_open_line);
    if (!processed.has_value()) co_return processed.error();

    if (!*processed) {
        co_return trace::index::store::indexer_error(
            trace::index::store::IndexerErrorKind::BuildError,
            dstd::fmt<"Failed to build index for {}">(gz_path),
            common::ErrorCode::Compression);
    }

    if (checksum_failed) {
        DFTRACER_UTILS_LOG_WARN(
            "Indexer: {} has a gzip member whose checksum does not match; its "
            "text, {} bytes in all, is indexed but may be corrupt.",
            gz_path, static_cast<unsigned long long>(recovered_bytes));
    } else if (truncated) {
        DFTRACER_UTILS_LOG_WARN(
            "Indexer: {} has a truncated or damaged gzip member; recovered {} "
            "bytes up to the cut or the damage (not checksum-verified).",
            gz_path, static_cast<unsigned long long>(recovered_bytes));
    } else if (slice == nullptr && members.empty() && compressed_bytes > 0) {
        unsigned char magic[2] = {0, 0};
        const ssize_t magic_read = co_await runtime::io::pread(fd, magic, 2, 0);
        if (magic_read != 2) magic[0] = 0;
        if (magic[0] == 0x1F && magic[1] == 0x8B) {
            DFTRACER_UTILS_LOG_WARN(
                "Indexer: no gzip member of {} could be decoded; it is "
                "corrupt, so the index will be empty.",
                gz_path);
        } else {
            co_return common::make_error<
                "Indexer: {} is not gzip; an index reads gzip traces only. "
                "Gzip it first">(common::ErrorCode::InvalidArgument, gz_path);
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

void persist_gzip_index_artifacts(trace::index::FileBuild& build,
                                  const GzipBuildArtifacts& artifacts) {
    for (const auto& member : artifacts.members)
        trace::index::put_gzip_member(build, member);
    for (const auto& [member_idx, window] : artifacts.restart_windows)
        trace::index::put_restart_window(build, member_idx, window);
    const bool has_mid_line_chunk =
        artifacts.has_mid_line_chunk ||
        std::any_of(artifacts.members.begin(), artifacts.members.end(),
                    [](const GzipMemberRecord& member) {
                        return member.opens_mid_line;
                    });
    trace::index::put_file_metadata(
        build, artifacts.checkpoint_size, artifacts.total_lines,
        artifacts.total_uc_size, artifacts.truncated, artifacts.has_open_line,
        artifacts.members.empty() ? 0 : artifacts.members.back().member_idx + 1,
        has_mid_line_chunk);
    build.finish(trace::index::store::IndexExtension::Members, 0);
}

dstd::Result<void> GzipIndexer::check_arguments(const dstd::String& gz_path,
                                                std::uint64_t ckpt_size) {
    if (gz_path.empty()) {
        return trace::index::store::indexer_error(
            trace::index::store::IndexerErrorKind::InvalidArgument,
            "gz_path must not be empty");
    }
    if (!common::input_exists(gz_path)) {
        return trace::index::store::indexer_error(
            trace::index::store::IndexerErrorKind::FileError,
            dstd::fmt<"gz_path does not exist: {}">(gz_path));
    }
    if (ckpt_size == 0) {
        return trace::index::store::indexer_error(
            trace::index::store::IndexerErrorKind::InvalidArgument,
            "ckpt_size must be greater than 0");
    }
    return {};
}

GzipIndexer::GzipIndexer(const dstd::String& gz_path,
                         const dstd::String& idx_path, std::uint64_t ckpt_size,
                         bool force_rebuild)
    : gz_path_(gz_path),
      gz_path_logical_path_(fileio::get_logical_path(gz_path)),
      index_path_(trace::index::normalize_index_root(idx_path)),
      ckpt_size_(ckpt_size),
      force_rebuild_(force_rebuild),
      cached_is_valid_(false),
      cached_max_bytes_(0),
      cached_max_bytes_ready_(false),
      cached_line_count_(0),
      cached_line_count_ready_(false),
      cached_checkpoint_size_(0),
      cached_checkpoint_size_ready_(false) {
    if (!check_arguments(gz_path_, ckpt_size_))
        dstd::fatal("GzipIndexer: the arguments were not checked");
    open();
}

GzipIndexer::~GzipIndexer() {
    DFTRACER_UTILS_LOG_DEBUG("Destroying GZIP indexer for {}", gz_path_);
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
      cached_heads_(std::move(other.cached_heads_)),
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
        cached_heads_ = std::move(other.cached_heads_);
        snapshot_ = std::move(other.snapshot_);
        file_ref_ = std::move(other.file_ref_);
    }
    return *this;
}

void GzipIndexer::open() {}

void GzipIndexer::close() {}

dftracer::utils::coro::CoroTask<dstd::Result<void>>
GzipIndexer::try_build_async() const {
    if (!force_rebuild_ && !need_rebuild()) {
        co_return dstd::Result<void>{};
    }

    const std::time_t mtime = fileio::get_file_modification_time(gz_path_);
    const auto hash = fileio::calculate_file_hash(gz_path_);
    const std::uint64_t bytes = fileio::file_size_bytes(gz_path_);
    const std::uint64_t final_ckpt_size =
        determine_checkpoint_size(ckpt_size_, gz_path_);
    const dstd::String logical = gz_path_logical_path_;

    dstd::Result<GzipBuildArtifacts> artifacts = GzipBuildArtifacts{};
    co_await coro::with_scope(
        [&](coro::Scope& scope) -> dftracer::utils::coro::CoroTask<void> {
            artifacts = co_await build_gzip_index_artifacts(
                gz_path_, final_ckpt_size, visitors_, &scope);
        });
    if (!artifacts) co_return artifacts.error();

    dstd::Optional<trace::index::Snapshot> snapshot;
    {
        auto opened = trace::index::Index::open(index_path_);
        bool fresh = false;
        if (opened) {
            auto before = opened->snapshot();
            fresh = before && !before->file(logical).has_value();
        }
        auto build =
            opened ? opened->build(
                         logical,
                         trace::index::SourceStat{
                             static_cast<std::uint64_t>(mtime), bytes, hash})
                   : dstd::Expected<trace::index::FileBuild, dstd::Error>(
                         dstd::Unexpected<dstd::Error>(opened.error()));
        dstd::Expected<void, dstd::Error> committed = {};
        if (build) {
            persist_gzip_index_artifacts(*build, *artifacts);
            // The new members renumber the chunks the old copies describe. A
            // fresh file has none.
            if (!fresh) trace::index::clear_chunk_copies(*build);
            committed = co_await build->commit();
        } else {
            committed = dstd::Unexpected<dstd::Error>(build.error());
        }
        if (!committed) {
            co_return trace::index::store::indexer_error(
                trace::index::store::IndexerErrorKind::BuildError,
                dstd::fmt<"Failed to write the index of {}: {}">(
                    gz_path_, committed.error().message()));
        }
        auto taken = opened->snapshot();
        if (!taken) co_return taken.error();
        snapshot.emplace(std::move(*taken));
    }

    cached_is_valid_ = true;
    cached_checkpoint_size_ = final_ckpt_size;
    cached_checkpoint_size_ready_ = true;
    const auto file = snapshot->file(logical);
    store::FileMetadataResult metadata;
    if (file) {
        auto stored = trace::index::file_metadata(*snapshot, *file);
        if (!stored) co_return stored.error();
        if (*stored) metadata = **stored;
    }
    cached_line_count_ = metadata.line_count;
    cached_line_count_ready_ = true;
    cached_max_bytes_ = metadata.max_bytes;
    cached_max_bytes_ready_ = true;
    std::lock_guard<std::mutex> lock(cached_members_mutex_);
    if (file) {
        auto members = trace::index::members(*snapshot, *file);
        if (!members) co_return members.error();
        cached_members_.assign(members->begin(), members->end());
    } else {
        cached_members_.clear();
    }
    cache_heads();
    file_ref_ = file;
    snapshot_ = std::move(snapshot);
    cached_loaded_.store(true, std::memory_order_release);
    co_return dstd::Result<void>{};
}

void GzipIndexer::cache_heads() const {
    cached_heads_.clear();
    for (const auto& m : cached_members_)
        if (m.kind == GzipRecordKind::Head) cached_heads_.push_back(m.c_offset);
}

bool GzipIndexer::is_valid() const { return cached_is_valid_; }

bool GzipIndexer::exists() const {
    const auto found = dstd::fs::is_directory(
        dstd::PathView(trace::index::store_dir(index_path_)));
    return found.has_value() && *found;
}

bool GzipIndexer::need_rebuild() const {
    if (is_valid()) {
        return false;
    }
    if (!exists()) {
        return true;
    }

    const auto stale = [&]() -> dstd::Result<bool> {
        auto opened = trace::index::Index::open(index_path_);
        if (!opened) return opened.error();
        auto snapshot = opened->snapshot();
        if (!snapshot) return snapshot.error();
        const auto file = snapshot->file(gz_path_logical_path_);
        if (!file) {
            return true;
        }
        trace::index::SourceStat now;
        dftracer::utils::common::InputFileStat st;
        if (common::stat_input(gz_path_, st)) {
            now.mtime = static_cast<std::uint64_t>(st.mtime);
            now.size = st.size;
        }
        if (snapshot->freshness(gz_path_logical_path_, now) !=
            trace::index::Freshness::Current) {
            return true;
        }
        // The stat check misses a same-size rewrite within one mtime second.
        const auto stored_hash = trace::index::file_hash(*snapshot, *file);
        if (!stored_hash) {
            return true;
        }
        const auto metadata = trace::index::file_metadata(*snapshot, *file);
        if (!metadata) return metadata.error();
        if (!*metadata || (*metadata)->checkpoint_size == 0) {
            return true;
        }
        return fileio::calculate_file_hash(gz_path_) != *stored_hash;
    }();
    if (!stale.has_value()) {
        // An unreadable index is stale because the rebuild rewrites it.
        DFTRACER_UTILS_LOG_WARN("Indexer: index {} is unreadable ({}); rebuild",
                                index_path_, stale.error().format());
        return true;
    }
    return *stale;
}

const dstd::String& GzipIndexer::index_path() const { return index_path_; }

const dstd::String& GzipIndexer::archive_path() const { return gz_path_; }

const dstd::String& GzipIndexer::gz_path() const { return gz_path_; }

dstd::Result<void> GzipIndexer::load() const {
    if (cached_loaded_.load(std::memory_order_acquire)) {
        return {};
    }
    std::lock_guard<std::mutex> lock(cached_members_mutex_);
    if (cached_loaded_.load(std::memory_order_relaxed)) {
        return {};
    }
    auto opened = trace::index::Index::open(index_path_);
    if (!opened) return opened.error();
    auto snapshot = opened->snapshot();
    if (!snapshot) return snapshot.error();
    const auto file = snapshot->file(gz_path_logical_path_);
    if (file) {
        store::FileMetadataResult metadata;
        auto stored = trace::index::file_metadata(*snapshot, *file);
        if (!stored) return stored.error();
        if (*stored) metadata = **stored;
        auto members = trace::index::members(*snapshot, *file);
        if (!members) return members.error();
        cached_line_count_.store(metadata.line_count,
                                 std::memory_order_relaxed);
        cached_line_count_ready_.store(true, std::memory_order_relaxed);
        cached_max_bytes_.store(metadata.max_bytes, std::memory_order_relaxed);
        cached_max_bytes_ready_.store(true, std::memory_order_relaxed);
        cached_checkpoint_size_.store(metadata.checkpoint_size,
                                      std::memory_order_relaxed);
        cached_checkpoint_size_ready_.store(true, std::memory_order_relaxed);
        cached_members_.assign(members->begin(), members->end());
        cache_heads();
    }
    file_ref_ = file;
    snapshot_ = std::move(*snapshot);
    cached_loaded_.store(true, std::memory_order_release);
    return {};
}

void GzipIndexer::ensure_loaded() const {
    const auto loaded = load();
    if (!loaded)
        dstd::fatal(dstd::fmt<"Indexer: the index {} was read without a "
                              "successful load: {}">(index_path_,
                                                     loaded.error().format())
                        .c_str());
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

bool GzipIndexer::has_damage() const {
    ensure_loaded();
    if (!file_ref_) return false;
    const auto stored = trace::index::file_metadata(*snapshot_, *file_ref_);
    return stored && *stored && (*stored)->truncated;
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
    auto window =
        trace::index::restart_window(*snapshot_, *file_ref_, member_idx);
    if (!window) {
        DFTRACER_UTILS_LOG_ERROR("Indexer: cannot read a restart window: {}",
                                 window.error().format());
        return {};
    }
    if (!*window) return {};
    return dstd::String((*window)->data(), (*window)->size());
}

}  // namespace dftracer::utils::trace::index::gzip
