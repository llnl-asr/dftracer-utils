#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/async_semaphore.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/build/index_visitor.h>
#include <dftracer/utils/index/gzip/checkpoint_size.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/gzip/gzip_member_scanner.h>
#include <dftracer/utils/index/store/error.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::index::gzip {

using index::store::IndexDatabase;

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
    std::unique_ptr<std::vector<unsigned char>> data;
    // Per-worker monotonic sequence. Load-bearing: moodycamel (our channel
    // backend) does not guarantee strict FIFO without producer tokens, so
    // the dispatcher reorders by this before handing chunks to visitors.
    std::uint64_t seq = 0;
    std::uint64_t lines = 0;
    // Absolute compressed offset just past the member this chunk ended.
    bool member_ended = false;
    std::uint64_t member_c_end = 0;
    // Uncompressed bytes of this chunk; `data` holds them only when a visitor
    // reads them.
    std::uint64_t data_len = 0;
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
    std::string window;
};

using ParallelChan = dftracer::utils::coro::Channel<ParallelInflateMsg>;

namespace compress = dftracer::utils::utilities::fileio::compress;

// Decode the gzip member starting at compressed offset `c_off` into `out`
// (grown as needed). Returns the number of compressed bytes the member
// consumed, or nullopt on read/decode failure. Peak memory is one member.
static dftracer::utils::coro::CoroTask<std::optional<std::uint64_t>>
decode_member_at(int fd, std::uint64_t c_off, std::uint64_t file_size,
                 const compress::GzipMemberDecompressor& dec,
                 std::vector<unsigned char>& out, std::size_t& out_len,
                 bool& truncated) {
    truncated = false;
    constexpr std::size_t READ_CHUNK = 1u << 20;
    if (out.size() < (1u << 20)) out.resize(1u << 20);
    std::vector<unsigned char> comp;
    std::uint64_t next_read = c_off;
    compress::DecompressResult res{};
    while (true) {
        if (!comp.empty()) {
            const compress::GzipDecode status = dec.decompress_status(
                comp.data(), comp.size(), out.data(), out.size(), res);
            if (status == compress::GzipDecode::Ok) break;
            if (status == compress::GzipDecode::InsufficientSpace) {
                out.resize(out.size() * 2);
                continue;
            }
            // BadData may just mean the member is not fully buffered yet.
        }
        if (next_read >= file_size) {
            if (comp.empty()) co_return std::nullopt;
            auto keep = compress::decode_truncated_member(comp.data(),
                                                          comp.size(), out);
            if (!keep) co_return std::nullopt;
            out_len = *keep;
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
        const ssize_t n = co_await dftracer::utils::io::pread(
            fd, comp.data() + old, want, static_cast<off_t>(next_read));
        if (n <= 0) co_return std::nullopt;
        comp.resize(old + static_cast<std::size_t>(n));
        next_read += static_cast<std::uint64_t>(n);
    }
    out_len = res.out_bytes;
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

constexpr std::size_t RESTART_WINDOW = 32768;

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
    int fd, const std::vector<GzipMember>& candidates, std::uint64_t end,
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
    const ssize_t n = co_await dftracer::utils::io::pread(
        fd, gap, len, static_cast<off_t>(end));
    if (n != static_cast<ssize_t>(len)) co_return end;
    for (std::size_t i = 0; i < len; ++i)
        if (gap[i] != 0) co_return end;
    co_return to;
}

using InflateBuffer = std::unique_ptr<std::vector<unsigned char>>;

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
        return std::make_unique<std::vector<unsigned char>>();
    }
    void give(InflateBuffer b) {
        if (!b) return;
        std::lock_guard<std::mutex> lock(mu_);
        if (free_.size() < keep_) free_.push_back(std::move(b));
    }

   private:
    std::mutex mu_;
    std::vector<InflateBuffer> free_;
    std::size_t keep_;
};

// Stream the gzip member at `c_off` with zlib, emitting a message per piece:
// a restart point is taken at the first block boundary once the piece holds
// `ckpt_size` output bytes. A member that stays under that is one message.
// `in_bytes` is the compressed size consumed. `stop` is set when the consumer
// is gone or the member was cut with nothing to keep.
static dftracer::utils::coro::CoroTask<bool> stream_member(
    int fd, std::uint64_t c_off, std::uint64_t file_size,
    std::uint64_t ckpt_size, const std::vector<GzipMember>& candidates,
    std::uint64_t range_c_end, bool strip_leading_partial, bool ship_bytes,
    InflateBuffers& buffers,
    dftracer::utils::coro::ChannelProducer<ParallelInflateMsg>& producer,
    std::uint64_t& seq, std::uint64_t& in_bytes, bool& stop) {
    constexpr std::size_t READ_CHUNK = 1u << 20;
    constexpr std::size_t OUT_STEP = 1u << 20;
    const std::size_t split_at = static_cast<std::size_t>(
        std::max<std::uint64_t>(ckpt_size, RESTART_WINDOW));

    z_stream zs{};
    if (inflateInit2(&zs, 31) != Z_OK) co_return false;
    struct End {
        z_stream* z;
        ~End() { inflateEnd(z); }
    } end_guard{&zs};

    std::vector<unsigned char> in(READ_CHUNK);
    InflateBuffer buf = buffers.take();
    buf->clear();
    // Growing a piece by doubling would leave a 64 MB buffer for a 32 MB one.
    buf->reserve(split_at + OUT_STEP);
    std::uint64_t next_read = c_off;
    bool strip = strip_leading_partial;
    bool cut = false;
    std::uint64_t since_newline = 0;
    stop = false;

    auto emit = [&](ParallelInflateMsg& msg) {
        std::vector<unsigned char>& data = *buf;
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
        msg.seq = seq++;
        msg.lines = count_newlines(data.data() + start, data.size() - start);
        std::size_t last = data.size();
        while (last > start && data[last - 1] != '\n') --last;
        since_newline = last > start ? data.size() - last
                                     : since_newline + (data.size() - start);
        msg.data_len = data.size() - start;
        if (ship_bytes) {
            data.erase(data.begin(),
                       data.begin() + static_cast<std::ptrdiff_t>(start));
            msg.data = std::move(buf);
            buf = buffers.take();
            buf->reserve(split_at + OUT_STEP);
        }
        buf->clear();
    };

    while (true) {
        if (zs.avail_in == 0 && next_read < file_size) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(READ_CHUNK, file_size - next_read));
            const ssize_t n = co_await dftracer::utils::io::pread(
                fd, in.data(), want, static_cast<off_t>(next_read));
            if (n <= 0) co_return false;
            zs.next_in = in.data();
            zs.avail_in = static_cast<uInt>(n);
            next_read += static_cast<std::uint64_t>(n);
        }

        std::vector<unsigned char>& piece = *buf;
        const std::size_t old = piece.size();
        piece.resize(old + OUT_STEP);
        zs.next_out = piece.data() + old;
        zs.avail_out = static_cast<uInt>(OUT_STEP);
        const int rc = inflate(&zs, Z_BLOCK);
        piece.resize(old + (OUT_STEP - zs.avail_out));

        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK && rc != Z_BUF_ERROR) co_return false;
        if (zs.avail_in == 0 && next_read >= file_size && zs.avail_out != 0) {
            cut = true;
            break;
        }

        if ((zs.data_type & 128) && !(zs.data_type & 64) &&
            piece.size() >= split_at) {
            ParallelInflateMsg msg;
            msg.restart = true;
            msg.bits = static_cast<std::uint8_t>(zs.data_type & 7);
            msg.member_c_end = c_off + zs.total_in;
            msg.window.assign(reinterpret_cast<const char*>(piece.data()) +
                                  piece.size() - RESTART_WINDOW,
                              RESTART_WINDOW);
            emit(msg);
            if (!(co_await producer.send(std::move(msg)))) {
                stop = true;
                co_return true;
            }
        }
    }

    ParallelInflateMsg msg;
    std::vector<unsigned char>& piece = *buf;
    if (cut) {
        std::size_t keep = piece.size();
        while (keep > 0 && piece[keep - 1] != '\n') --keep;
        if (keep == 0) {
            msg.seq = seq++;
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
        in_bytes = zs.total_in;
    }
    emit(msg);
    buffers.give(std::move(buf));
    msg.member_ended = true;
    msg.member_c_end = co_await next_member_start(
        fd, candidates, c_off + in_bytes, range_c_end);
    if (!(co_await producer.send(std::move(msg)))) stop = true;
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
    const std::vector<GzipMember>& candidates, bool strip_leading_partial,
    bool extend_to_newline_past_end, bool ship_bytes, InflateBuffers& buffers,
    dftracer::utils::coro::ChannelProducer<ParallelInflateMsg> producer) {
    auto guard = producer.guard();

    compress::GzipMemberDecompressor dec;
    if (!dec.valid()) co_return false;

    InflateBuffer buf = buffers.take();
    std::uint64_t c_off = range_c_start;
    std::uint64_t seq = 0;
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
        if (span >= ckpt_size / 8) {
            std::uint64_t consumed = 0;
            bool stop = false;
            if (!co_await stream_member(
                    fd, c_off, file_size, ckpt_size, candidates, range_c_end,
                    strip_leading_partial && first, ship_bytes, buffers,
                    producer, seq, consumed, stop)) {
                co_return false;
            }
            if (stop) co_return true;
            first = false;
            c_off = co_await next_member_start(fd, candidates, c_off + consumed,
                                               range_c_end);
            continue;
        }
        std::size_t out_len = 0;
        bool truncated = false;
        std::vector<unsigned char>& out = *buf;
        // A buffer comes back as long as its last message; one shorter than
        // the member, by its ISIZE, would make the decode start over. ISIZE is
        // the four bytes before the next header, so the last member, which a
        // cut can end anywhere, has none to trust.
        if (next != candidates.begin() && next != candidates.end() &&
            std::prev(next)->c_offset == c_off) {
            const GzipMember& m = *std::prev(next);
            if (m.isize <= m.c_size * MAX_DEFLATE_RATIO &&
                out.size() <= m.isize)
                out.resize(std::size_t{m.isize} + 1);
        }
        auto in_bytes = co_await decode_member_at(fd, c_off, file_size, dec,
                                                  out, out_len, truncated);
        if (!in_bytes) co_return false;
        if (truncated && out_len == 0) {
            ParallelInflateMsg msg;
            msg.seq = seq++;
            msg.truncated = true;
            co_await producer.send(std::move(msg));
            co_return true;
        }

        std::size_t emit_start = 0;
        // The first member of a mid-file slice opens mid-line (the tail of a
        // line that began in the previous slice); drop up to its first \n so
        // the dispatcher sees a clean start.
        if (strip_leading_partial && first) {
            emit_start = out_len;
            for (std::size_t i = 0; i < out_len; ++i) {
                if (out[i] == '\n') {
                    emit_start = i + 1;
                    break;
                }
            }
        }
        first = false;

        const std::size_t emit_len = out_len - emit_start;
        ParallelInflateMsg msg;
        msg.seq = seq++;
        msg.lines = count_newlines(out.data() + emit_start, emit_len);
        msg.data_len = emit_len;
        if (ship_bytes) {
            out.resize(out_len);
            out.erase(out.begin(),
                      out.begin() + static_cast<std::ptrdiff_t>(emit_start));
            msg.data = std::move(buf);
            buf = buffers.take();
        }
        msg.member_ended = true;
        msg.member_c_end = co_await next_member_start(
            fd, candidates, c_off + *in_bytes, range_c_end);
        msg.truncated = truncated;
        c_off = msg.member_c_end;
        if (!(co_await producer.send(std::move(msg)))) co_return true;
    }

    // Non-last slice: capture the line straddling this slice's end by decoding
    // the next slice's first member and emitting up to and including its first
    // \n. No member record for it (member_ended stays false).
    std::vector<unsigned char>& out = *buf;
    if (extend_to_newline_past_end && range_c_end < file_size) {
        std::size_t out_len = 0;
        bool truncated = false;
        auto in_bytes = co_await decode_member_at(fd, range_c_end, file_size,
                                                  dec, out, out_len, truncated);
        if (!in_bytes) co_return false;
        std::size_t take = out_len;
        for (std::size_t i = 0; i < out_len; ++i) {
            if (out[i] == '\n') {
                take = i + 1;
                break;
            }
        }
        ParallelInflateMsg msg;
        msg.seq = seq++;
        msg.lines = count_newlines(out.data(), take);
        msg.data_len = take;
        if (ship_bytes)
            msg.data = std::make_unique<std::vector<unsigned char>>(
                out.data(), out.data() + take);
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
static std::vector<InflateRange> plan_ranges(
    const std::vector<GzipMember>& members, std::uint64_t slice_c_end,
    std::uint64_t target) {
    std::vector<InflateRange> ranges;
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

struct RangeSlot {
    std::shared_ptr<ParallelChan> chan;
    std::optional<dftracer::utils::coro::ChannelProducer<ParallelInflateMsg>>
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
    std::vector<GzipMember> members;
    std::vector<InflateRange> ranges;
    dftracer::utils::coro::CoroSemaphore permits;
    std::vector<RangeSlot> ring;
    InflateBuffers buffers;
    std::atomic<std::size_t> next{0};

    std::shared_ptr<ParallelChan> channel(std::size_t r) const {
        return dftracer::utils::coro::make_channel<ParallelInflateMsg>(
            ship_bytes ? ranges[r].messages + 1 : 0);
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
    CoroScope* scope, InflatePlan* plan, std::uint64_t member_idx_base,
    std::uint64_t& total_lines, std::uint64_t& total_uc_size,
    std::vector<GzipMemberRecord>& members,
    std::vector<std::pair<std::uint64_t, std::string>>& restart_windows,
    const CheckpointIndexer::VisitorList& visitors, bool& truncated,
    std::uint64_t& recovered_bytes) {
    const bool has_visitors = !visitors.empty();
    std::uint64_t global_uc = 0;

    std::uint64_t member_idx = member_idx_base;
    std::uint64_t member_c_start = plan->ranges.front().c_start;
    std::uint64_t member_uc_start = 0;
    std::uint64_t member_first_line = total_lines + 1;
    GzipRecordKind piece_kind = GzipRecordKind::MEMBER;
    std::uint8_t piece_bits = 0;
    std::string piece_window;

    std::uint64_t total_chunks_received = 0;

    auto process_msg =
        [&](ParallelInflateMsg& msg) -> dftracer::utils::coro::CoroTask<void> {
        const std::size_t data_len = static_cast<std::size_t>(msg.data_len);
        ++total_chunks_received;
        if (msg.truncated) {
            truncated = true;
            recovered_bytes += data_len;
        }
        if (msg.drop_tail > 0) {
            std::uint64_t left = msg.drop_tail;
            while (left > 0 && !members.empty()) {
                auto& last = members.back();
                const std::uint64_t cut = std::min(left, last.uc_size);
                last.uc_size -= cut;
                left -= cut;
                if (last.uc_size == 0) {
                    if (last.kind == GzipRecordKind::RESTART)
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

        if (has_visitors && data_len > 0) {
            const char* data = reinterpret_cast<const char*>(msg.data->data());
            for (auto& v : visitors) {
                co_await v.get().on_chunk(data, data_len, member_idx);
            }
        }

        global_uc += data_len;
        total_lines += msg.lines;

        if (msg.member_ended || msg.restart) {
            GzipRecordKind kind = piece_kind;
            if (msg.restart && kind == GzipRecordKind::MEMBER) {
                kind = GzipRecordKind::HEAD;
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
            });
            if (kind == GzipRecordKind::RESTART) {
                restart_windows.emplace_back(member_idx,
                                             std::move(piece_window));
            }
            if (msg.restart) {
                piece_kind = GzipRecordKind::RESTART;
                piece_bits = msg.bits;
                piece_window = std::move(msg.window);
            } else {
                piece_kind = GzipRecordKind::MEMBER;
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

    // moodycamel::ConcurrentQueue (backing coro::Channel) does not guarantee
    // FIFO without producer tokens, so messages are put back in `seq` order.
    auto consume =
        [&](ParallelChan& chan) -> dftracer::utils::coro::CoroTask<void> {
        std::uint64_t expected_seq = 0;
        std::map<std::uint64_t, ParallelInflateMsg> pending;
        while (auto msg_opt = co_await chan.receive()) {
            pending.emplace(msg_opt->seq, std::move(*msg_opt));
            auto it = pending.begin();
            while (it != pending.end() && it->first == expected_seq) {
                co_await process_msg(it->second);
                plan->buffers.give(std::move(it->second.data));
                co_await drain_visitors();
                it = pending.erase(it);
                ++expected_seq;
            }
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

    std::shared_ptr<ParallelChan> redo;
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
                    scope->spawn(
                        [plan, r, start = member_c_start,
                         producer = redo->producer()](CoroScope&) mutable {
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
            } catch (...) {
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
    CoroScope* scope, int fd, std::uint64_t slice_c_end,
    std::uint64_t file_size, std::uint64_t ckpt_size, std::size_t ways,
    std::vector<GzipMember> scanned_members, std::uint64_t member_idx_base,
    bool strip_slice_leading_partial, std::uint64_t& total_lines,
    std::uint64_t& total_uc_size, std::vector<GzipMemberRecord>& members,
    std::vector<std::pair<std::uint64_t, std::string>>& restart_windows,
    const CheckpointIndexer::VisitorList& visitors, bool& truncated,
    std::uint64_t& recovered_bytes) {
    // Fanning out past the thread count only adds channel and scheduling
    // overhead; on a single-threaded drive (RunLoop, the no-executor .get()
    // path) it collapses to one worker.
    constexpr std::size_t DEFAULT_MAX_WORKERS = 16;
    std::vector<InflateRange> ranges =
        plan_ranges(scanned_members, slice_c_end,
                    std::max<std::uint64_t>(ckpt_size / 4, 1));
    const std::size_t hw = std::max<std::size_t>(1, available_parallelism());
    const std::size_t num_workers = std::min<std::size_t>(
        {ways > 0 ? ways : DEFAULT_MAX_WORKERS, ranges.size(), hw});
    // Two ranges past the workers' keep them busy while the dispatcher
    // consumes the oldest; each range in the window holds its plaintext.
    InflatePlan plan(std::min<std::size_t>(num_workers + 2, ranges.size()),
                     num_workers);
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
    co_await scope->scope(
        [&](CoroScope& child) -> dftracer::utils::coro::CoroTask<void> {
            for (std::size_t w = 0; w < num_workers; ++w)
                child.spawn(
                    [plan_ptr](CoroScope&) { return range_worker(plan_ptr); });
            // In the body, not spawned, so its error leaves the scope after the
            // workers are joined.
            dispatcher_ok = co_await parallel_dispatcher(
                &child, plan_ptr, member_idx_base, total_lines, total_uc_size,
                members, restart_windows, visitors, truncated, recovered_bytes);
        });

    co_return dispatcher_ok;
}

static dftracer::utils::coro::CoroTask<bool> process_chunks(
    CoroScope* scope, int fd, const GzipMemberSlice* slice,
    std::uint64_t ckpt_size, std::size_t ways, std::uint64_t& total_lines,
    std::uint64_t& total_uc_size, std::vector<GzipMemberRecord>& members,
    std::vector<std::pair<std::uint64_t, std::string>>& restart_windows,
    const CheckpointIndexer::VisitorList& visitors, bool& truncated,
    std::uint64_t& recovered_bytes) {
    struct stat st;
    if (::fstat(fd, &st) != 0) co_return false;
    const std::uint64_t file_size = static_cast<std::uint64_t>(st.st_size);

    // Pre-scanned slice path: caller supplied the member map and a range.
    // Used by the MPI/distributed indexer to split one file across ranks
    // without re-scanning. Member indices start at `member_begin` so
    // multiple slices of the same file_id produce disjoint SST entries.
    if (slice != nullptr && slice->members != nullptr &&
        slice->member_end > slice->member_begin) {
        const auto& all = *slice->members;
        const std::size_t mb = slice->member_begin;
        const std::size_t me = slice->member_end;
        if (me > all.size() || mb >= me) co_return false;
        // Slice end: next-member offset if this isn't the last slice of
        // the file, else EOF. Crucial: a non-last slice's workers must
        // not inflate past this boundary into another slice's bytes.
        const std::uint64_t slice_c_end =
            (me < all.size()) ? all[me].c_offset : file_size;
        std::vector<GzipMember> sliced(all.begin() + mb, all.begin() + me);
        co_return co_await process_chunks_parallel(
            scope, fd, slice_c_end, file_size, ckpt_size, ways,
            std::move(sliced),
            /*member_idx_base=*/mb, /*strip_slice_leading_partial=*/mb > 0,
            total_lines, total_uc_size, members, restart_windows, visitors,
            truncated, recovered_bytes);
    }

    // Discover member boundaries so the inflate pass can fan out. The scan
    // is zero-copy, sequential, and fast relative to inflate. A file whose
    // members cannot be enumerated is processed as one member spanning the
    // whole file, which the same path handles with a single worker.
    std::vector<GzipMember> scanned;
    if (file_size >= 18) {
        co_await enumerate_gzip_member_candidates(fd, file_size, scanned);
    }
    if (scanned.empty()) scanned.push_back(GzipMember{0, file_size});

    co_return co_await process_chunks_parallel(
        scope, fd, file_size, file_size, ckpt_size, ways, std::move(scanned),
        /*member_idx_base=*/0, /*strip_slice_leading_partial=*/false,
        total_lines, total_uc_size, members, restart_windows, visitors,
        truncated, recovered_bytes);
}

}  // namespace

dftracer::utils::coro::CoroTask<std::optional<GzipBuildArtifacts>>
build_gzip_index_artifacts(const std::string& gz_path, std::uint64_t ckpt_size,
                           const CheckpointIndexer::VisitorList& visitors,
                           CoroScope* scope, const GzipMemberSlice* slice,
                           std::size_t ways) {
    int fd = ::open(gz_path.c_str(), O_RDONLY);
    if (fd < 0) {
        co_return std::nullopt;
    }

    if (!visitors.empty()) {
        const std::uint64_t compressed_bytes =
            index::store::internal::file_size_bytes(gz_path);
        const std::size_t estimated = static_cast<std::size_t>(
            compressed_bytes / (ckpt_size > 0 ? ckpt_size : 1));
        for (auto& visitor : visitors) {
            visitor.get().begin(estimated);
            visitor.get().bind_scope(scope);
        }
    }

    std::uint64_t total_lines = 0;
    std::uint64_t total_uc_size = 0;
    std::vector<GzipMemberRecord> members;
    std::vector<std::pair<std::uint64_t, std::string>> restart_windows;

    bool truncated = false;
    std::uint64_t recovered_bytes = 0;
    const bool success = co_await process_chunks(
        scope, fd, slice, ckpt_size, ways, total_lines, total_uc_size, members,
        restart_windows, visitors, truncated, recovered_bytes);
    ::close(fd);

    if (!success) {
        co_return std::nullopt;
    }

    if (truncated) {
        DFTRACER_UTILS_LOG_WARN(
            "Indexer: %s ends in a truncated gzip member; recovered %llu bytes "
            "up to its last complete line (not checksum-verified).",
            gz_path.c_str(), static_cast<unsigned long long>(recovered_bytes));
    } else if (slice == nullptr && members.empty() &&
               index::store::internal::file_size_bytes(gz_path) > 0) {
        unsigned char magic[2] = {0, 0};
        if (FILE* f = std::fopen(gz_path.c_str(), "rb")) {
            if (std::fread(magic, 1, 2, f) != 2) magic[0] = 0;
            std::fclose(f);
        }
        if (magic[0] == 0x1F && magic[1] == 0x8B) {
            DFTRACER_UTILS_LOG_WARN(
                "Indexer: no gzip member of %s could be decoded; it is "
                "corrupt, so the index will be empty.",
                gz_path.c_str());
        } else {
            throw DFTUtilsException::cat(
                ErrorCode::INVALID_ARGUMENT, "Indexer: ", gz_path,
                " is not gzip; an index reads gzip traces only. Gzip it, or "
                "let dftracer_view, dftracer_run or dftracer_index convert it "
                "(they write a gzip copy under split/)");
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
    co_return artifacts;
}

void persist_gzip_index_artifacts(index::store::IndexWrite& w, int file_id,
                                  const GzipBuildArtifacts& artifacts) {
    namespace records = index::store::records;
    records::clear_file(w, index::store::IndexExtension::MEMBERS, file_id);
    for (const auto& member : artifacts.members)
        records::put_gzip_member(w, file_id, member);
    for (const auto& [member_idx, window] : artifacts.restart_windows)
        records::put_restart_window(w, file_id, member_idx, window);
    records::put_file_metadata(w, file_id, artifacts.checkpoint_size,
                               artifacts.total_lines, artifacts.total_uc_size,
                               artifacts.truncated);
    records::put_manifest(w, file_id, index::store::IndexExtension::MEMBERS, 0);
}

GzipIndexer::GzipIndexer(const std::string& gz_path_,
                         const std::string& idx_path_, std::uint64_t ckpt_size_,
                         bool force_rebuild_)
    : gz_path(gz_path_),
      gz_path_logical_path(index::store::internal::get_logical_path(gz_path_)),
      index_path(index::store::internal::normalize_index_root(idx_path_)),
      ckpt_size(ckpt_size_),
      force_rebuild(force_rebuild_),
      cached_is_valid(false),
      cached_file_id(-1),
      cached_max_bytes(0),
      cached_max_bytes_ready(false),
      cached_num_lines(0),
      cached_num_lines_ready(false),
      cached_checkpoint_size(0),
      cached_checkpoint_size_ready(false) {
    if (gz_path.empty()) {
        throw index::store::IndexerError(
            index::store::IndexerError::Type::INVALID_ARGUMENT,
            "gz_path must not be empty");
    }

    if (!fs::exists(gz_path)) {
        throw index::store::IndexerError(
            index::store::IndexerError::Type::FILE_ERROR,
            "gz_path does not exist: " + gz_path);
    }

    if (ckpt_size == 0) {
        throw index::store::IndexerError(
            index::store::IndexerError::Type::INVALID_ARGUMENT,
            "ckpt_size must be greater than 0");
    }

    open();
}

GzipIndexer::~GzipIndexer() {
    DFTRACER_UTILS_LOG_DEBUG("Destroying GZIP indexer for %s", gz_path.c_str());
    close();
}

GzipIndexer::GzipIndexer(GzipIndexer&& other) noexcept
    : gz_path(std::move(other.gz_path)),
      gz_path_logical_path(std::move(other.gz_path_logical_path)),
      index_path(std::move(other.index_path)),
      ckpt_size(other.ckpt_size),
      force_rebuild(other.force_rebuild),
      visitors_(std::move(other.visitors_)),
      cached_is_valid(other.cached_is_valid.load()),
      cached_file_id(other.cached_file_id.load()),
      cached_max_bytes(other.cached_max_bytes.load()),
      cached_max_bytes_ready(other.cached_max_bytes_ready.load()),
      cached_num_lines(other.cached_num_lines.load()),
      cached_num_lines_ready(other.cached_num_lines_ready.load()),
      cached_checkpoint_size(other.cached_checkpoint_size.load()),
      cached_checkpoint_size_ready(other.cached_checkpoint_size_ready.load()),
      cached_members(std::move(other.cached_members)),
      cached_loaded(other.cached_loaded.load()) {}

GzipIndexer& GzipIndexer::operator=(GzipIndexer&& other) noexcept {
    if (this != &other) {
        gz_path = std::move(other.gz_path);
        gz_path_logical_path = std::move(other.gz_path_logical_path);
        index_path = std::move(other.index_path);
        ckpt_size = other.ckpt_size;
        force_rebuild = other.force_rebuild;
        visitors_ = std::move(other.visitors_);
        cached_is_valid.store(other.cached_is_valid.load());
        cached_file_id.store(other.cached_file_id.load());
        cached_max_bytes.store(other.cached_max_bytes.load());
        cached_max_bytes_ready.store(other.cached_max_bytes_ready.load());
        cached_num_lines.store(other.cached_num_lines.load());
        cached_num_lines_ready.store(other.cached_num_lines_ready.load());
        cached_checkpoint_size.store(other.cached_checkpoint_size.load());
        cached_checkpoint_size_ready.store(
            other.cached_checkpoint_size_ready.load());
        cached_loaded.store(other.cached_loaded.load());
        std::lock_guard<std::mutex> lock(cached_members_mutex);
        cached_members = std::move(other.cached_members);
    }
    return *this;
}

void GzipIndexer::open() {}

void GzipIndexer::close() {}

dftracer::utils::coro::CoroTask<void> GzipIndexer::build_async() const {
    if (!force_rebuild && !need_rebuild()) {
        co_return;
    }

    index::store::IndexDatabase db(index_path);
    const std::time_t mtime =
        index::store::internal::get_file_modification_time(gz_path);
    const auto hash = index::store::internal::calculate_file_hash(gz_path);
    const std::uint64_t bytes =
        index::store::internal::file_size_bytes(gz_path);
    const std::uint64_t final_ckpt_size =
        determine_checkpoint_size(ckpt_size, gz_path);
    const std::string logical = gz_path_logical_path;

    std::optional<GzipBuildArtifacts> artifacts;
    co_await run_coro_scope(
        [&](CoroScope& scope) -> dftracer::utils::coro::CoroTask<void> {
            artifacts = co_await build_gzip_index_artifacts(
                gz_path, final_ckpt_size, visitors_, &scope);
        });
    if (!artifacts) {
        throw index::store::IndexerError(
            index::store::IndexerError::Type::BUILD_ERROR,
            "Failed to build index for " + gz_path);
    }

    int file_id = -1;
    {
        auto w = db.begin_write();
        file_id = w->file_id_for(logical);
        index::store::records::put_file_record(
            *w, logical,
            {static_cast<std::uint32_t>(file_id),
             static_cast<std::uint64_t>(mtime), hash, bytes});
        persist_gzip_index_artifacts(*w, file_id, *artifacts);
        w->commit();
    }

    cached_is_valid = true;
    cached_file_id = file_id;
    cached_checkpoint_size = final_ckpt_size;
    cached_checkpoint_size_ready = true;
    cached_num_lines = db.get_num_lines(file_id);
    cached_num_lines_ready = true;
    cached_max_bytes = db.get_max_bytes(file_id);
    cached_max_bytes_ready = true;
    std::lock_guard<std::mutex> lock(cached_members_mutex);
    cached_members = db.query_gzip_members(file_id);
    cached_loaded.store(true, std::memory_order_release);
    co_return;
}

bool GzipIndexer::is_valid() const { return cached_is_valid; }

bool GzipIndexer::exists() const {
    return fs::exists(index_path) && fs::is_directory(index_path);
}

bool GzipIndexer::need_rebuild() const {
    if (is_valid()) {
        return false;
    }
    if (!exists()) {
        return true;
    }

    try {
        index::store::IndexDatabase db(index_path,
                                       index::store::IndexOpenMode::ReadOnly);
        const auto stored_hash = db.get_file_hash(gz_path_logical_path);
        const int file_id = db.get_file_info_id(gz_path_logical_path);
        if (!stored_hash || file_id < 0) {
            return true;
        }

        const auto current_hash =
            index::store::internal::calculate_file_hash(gz_path);
        const auto current_ckpt_size = db.get_checkpoint_size(file_id);
        return current_hash != *stored_hash || current_ckpt_size == 0;
    } catch (...) {
        return true;
    }
}

const std::string& GzipIndexer::get_index_path() const { return index_path; }

const std::string& GzipIndexer::get_archive_path() const { return gz_path; }

const std::string& GzipIndexer::get_gz_path() const { return gz_path; }

void GzipIndexer::ensure_loaded() const {
    if (cached_loaded.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(cached_members_mutex);
    if (cached_loaded.load(std::memory_order_relaxed)) {
        return;
    }
    index::store::IndexDatabase db(index_path,
                                   index::store::IndexOpenMode::ReadOnly);
    const int file_id = db.get_file_info_id(gz_path_logical_path);
    cached_file_id.store(file_id, std::memory_order_relaxed);
    if (file_id != -1) {
        cached_num_lines.store(db.get_num_lines(file_id),
                               std::memory_order_relaxed);
        cached_num_lines_ready.store(true, std::memory_order_relaxed);
        cached_max_bytes.store(db.get_max_bytes(file_id),
                               std::memory_order_relaxed);
        cached_max_bytes_ready.store(true, std::memory_order_relaxed);
        cached_checkpoint_size.store(db.get_checkpoint_size(file_id),
                                     std::memory_order_relaxed);
        cached_checkpoint_size_ready.store(true, std::memory_order_relaxed);
        cached_members = db.query_gzip_members(file_id);
    }
    cached_loaded.store(true, std::memory_order_release);
}

std::uint64_t GzipIndexer::get_max_bytes() const {
    ensure_loaded();
    return cached_max_bytes.load(std::memory_order_relaxed);
}

std::uint64_t GzipIndexer::get_checkpoint_size() const {
    ensure_loaded();
    return cached_checkpoint_size.load(std::memory_order_relaxed);
}

std::uint64_t GzipIndexer::get_num_lines() const {
    ensure_loaded();
    return cached_num_lines.load(std::memory_order_relaxed);
}

int GzipIndexer::get_file_id() const {
    ensure_loaded();
    return cached_file_id.load(std::memory_order_relaxed);
}

int GzipIndexer::find_file_id(const std::string& path) const {
    index::store::IndexDatabase db(index_path,
                                   index::store::IndexOpenMode::ReadOnly);
    return db.get_file_info_id(index::store::internal::get_logical_path(path));
}

bool GzipIndexer::find_member(std::size_t target_offset,
                              GzipMemberRecord& member) const {
    ensure_loaded();
    std::lock_guard<std::mutex> lock(cached_members_mutex);
    for (const auto& candidate : cached_members) {
        if (target_offset < candidate.uc_offset + candidate.uc_size) {
            member = candidate;
            return true;
        }
    }
    return false;
}

std::vector<GzipMemberRecord> GzipIndexer::get_members() const {
    ensure_loaded();
    std::lock_guard<std::mutex> lock(cached_members_mutex);
    return cached_members;
}

std::string GzipIndexer::restart_window(std::uint64_t member_idx) const {
    ensure_loaded();
    const int file_id = cached_file_id.load(std::memory_order_relaxed);
    if (file_id == -1) return {};
    index::store::IndexDatabase db(index_path,
                                   index::store::IndexOpenMode::ReadOnly);
    return db.query_restart_window(file_id, member_idx).value_or(std::string{});
}

}  // namespace dftracer::utils::index::gzip
