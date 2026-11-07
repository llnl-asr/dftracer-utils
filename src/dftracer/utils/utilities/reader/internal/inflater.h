// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_INFLATER_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_INFLATER_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/raw_buffer.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/gzip_member_record.h>
#include <dftracer/utils/runtime/io/io.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_stream_inflater.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/reader/internal/member_decode_cache.h>
#include <sys/stat.h>

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dftracer::utils::utilities::reader::internal {

/**
 * Random-access gzip reader backed by libdeflate.
 *
 * A gzip member is a self-contained stream, so each is decoded whole with a
 * single libdeflate call and served from memory. Reads walk members forward
 * from the seek point using libdeflate's consumed-byte count for boundaries,
 * so no header scan or streaming state machine is needed. Peak memory is one
 * decoded member (plus its compressed bytes) rather than a small streaming
 * window.
 *
 * When a MemberDecodeCache is attached, each member is fetched through it
 * keyed by (file_token, compressed offset), so concurrent readers of the same
 * member share one decode. Without a cache the member is decoded into a reused
 * local buffer.
 *
 * A HEAD or RESTART piece of a member split at restart points is read with a
 * zlib stream instead, which can start inside a deflate stream; at the end of
 * the stream the reader goes back to whole-member decoding.
 *
 * A member larger than the bound is never held whole but streamed in steps of
 * whole lines. One the index (set_index_hint) does not call big is tried in a
 * buffer that stops at the bound, so the work lost is at most one bound per
 * big member.
 */
class ReaderInflater {
   public:
    ReaderInflater() = default;
    ReaderInflater(const ReaderInflater&) = delete;
    ReaderInflater& operator=(const ReaderInflater&) = delete;
    ~ReaderInflater() { end_stream(); }

    /// Route member decodes through `cache`, keyed by `file_token`. Pass
    /// nullptr to decode locally. Set once before reads.
    void set_cache(MemberDecodeCache* cache, std::uint64_t file_token) {
        cache_ = cache;
        file_token_ = file_token;
    }

    /// Members larger than `bytes` (at least 1 MiB) are streamed. Set once
    /// before reads. The default is the checkpoint size of the indexer.
    void set_member_bound(std::size_t bytes) {
        bound_ = std::max<std::size_t>(bytes, INIT_OUT);
    }

    /// The compressed offsets where the index puts the HEAD of a member too
    /// big to hold whole (empty when it has none). Such a member is streamed
    /// without a libdeflate attempt.
    void set_index_hint(dstd::Vector<std::uint64_t> heads) {
        big_heads_ = std::move(heads);
        std::sort(big_heads_.begin(), big_heads_.end());
        hinted_ = true;
    }

    /// Start reading from `file_offset` (0 = start of file). `expected_out`
    /// (0 = default) pre-sizes the decode buffer to a known uncompressed span.
    coro::CoroTask<bool> initialize(int fd, off_t& offset,
                                    std::uint64_t file_offset = 0,
                                    int /*window_bits*/ = 0,
                                    std::size_t expected_out = 0) {
        reset();
        co_return co_await begin_at(fd, offset, file_offset, expected_out);
    }

    /// Seek to a member or piece for random access. A member header is a
    /// member boundary, so a MEMBER restarts the forward walk at
    /// member.c_offset. A HEAD or RESTART piece starts a zlib stream there;
    /// `window` is the 32 KiB window of a RESTART piece.
    coro::CoroTask<bool> seek_to_member(
        int fd, off_t& offset,
        const dftracer::utils::index::gzip::GzipMemberRecord& member,
        dstd::StringView window, std::size_t expected_out = 0) {
        DFTRACER_UTILS_LOG_DEBUG("Seeking to member %" PRIu64
                                 ": c_offset=%" PRIu64 ", uc_offset=%" PRIu64,
                                 member.member_idx, member.c_offset,
                                 member.uc_offset);
        reset();
        const auto begin_at_ok =
            co_await begin_at(fd, offset, member.c_offset, expected_out);
        if (!begin_at_ok) {
            co_return false;
        }
        using dftracer::utils::index::gzip::GzipRecordKind;
        if (member.kind == GzipRecordKind::Member) co_return true;
        if (member.kind == GzipRecordKind::Head) {
            co_return start_stream(31);
        }
        // Resuming mid-stream with a window and a bit prime follows Mark
        // Adler's zran.c (zlib license).
        if (window.empty() || member.bits > 7) co_return false;
        unsigned char prime = 0;
        if (member.bits > 0) {
            const auto prime_read =
                co_await dftracer::utils::runtime::io::pread(
                    fd, &prime, 1, member.c_offset - 1);
            if (prime_read != 1) {
                co_return false;
            }
        }
        if (!start_stream(-15)) co_return false;
        if (member.bits > 0 &&
            !zs_.prime(member.bits, prime >> (8 - member.bits))) {
            end_stream();
            co_return false;
        }
        if (!zs_.set_dictionary(
                reinterpret_cast<const unsigned char*>(window.data()),
                window.size())) {
            end_stream();
            co_return false;
        }
        co_return true;
    }

    /// Fill up to `len` uncompressed bytes into `buf`. `bytes_out` is the
    /// amount produced; 0 means end of stream.
    coro::CoroTask<bool> read(int fd, off_t& offset, unsigned char* buf,
                              std::size_t len, std::size_t& bytes_out) {
        bytes_out = 0;
        while (bytes_out < len) {
            if (member_pos_ >= member_size_) {
                if (exhausted_) break;
                const auto decode_next_member_ok =
                    co_await decode_next_member(fd, offset);
                if (!decode_next_member_ok) co_return false;
                if (member_size_ == 0) {
                    exhausted_ = true;
                    break;
                }
            }
            const std::size_t avail = member_size_ - member_pos_;
            const std::size_t take = std::min(len - bytes_out, avail);
            std::memcpy(buf + bytes_out, member_data_ + member_pos_, take);
            member_pos_ += take;
            bytes_out += take;
        }
        co_return true;
    }

    /// Discard `bytes_to_skip` uncompressed bytes.
    coro::CoroTask<bool> skip_bytes(int fd, off_t& offset,
                                    std::size_t bytes_to_skip) {
        while (bytes_to_skip > 0) {
            if (member_pos_ >= member_size_) {
                if (exhausted_) break;
                const auto decode_next_member_ok =
                    co_await decode_next_member(fd, offset);
                if (!decode_next_member_ok) co_return false;
                if (member_size_ == 0) {
                    exhausted_ = true;
                    break;
                }
            }
            const std::size_t avail = member_size_ - member_pos_;
            const std::size_t take = std::min(bytes_to_skip, avail);
            member_pos_ += take;
            bytes_to_skip -= take;
        }
        co_return bytes_to_skip == 0;
    }

    void reset() {
        end_stream();
        tail_.clear();
        comp_.clear();
        member_shared_.reset();
        member_data_ = nullptr;
        member_size_ = 0;
        member_pos_ = 0;
        comp_off_ = 0;
        next_read_ = 0;
        file_size_ = 0;
        exhausted_ = false;
    }

    bool is_at_end() const { return exhausted_ && member_pos_ >= member_size_; }

   private:
    static constexpr std::size_t READ_CHUNK = 1u << 20;
    static constexpr std::size_t INIT_OUT = 1u << 20;
    static constexpr std::size_t BOUND_DEFAULT = std::size_t{32} << 20;

    coro::CoroTask<bool> begin_at(int fd, off_t& offset,
                                  std::uint64_t file_offset,
                                  std::size_t expected_out = 0) {
        struct stat st;
        if (::fstat(fd, &st) != 0) co_return false;
        file_size_ = static_cast<std::uint64_t>(st.st_size);
        comp_off_ = file_offset;
        next_read_ = file_offset;
        offset = static_cast<off_t>(file_offset);
        // Pre-size to the known span so the first member does not grow the
        // buffer by doubling.
        if (!cache_) {
            const std::size_t want =
                std::min(std::max<std::size_t>(INIT_OUT, expected_out), bound_);
            if (member_owned_.size() < want) member_owned_.resize(want);
        }
        co_return dec_.valid();
    }

    bool start_stream(int window_bits) {
        if (!zs_.init(window_bits)) return false;
        stream_open_ = true;
        stream_base_ = comp_off_;
        stream_raw_ = window_bits < 0;
        return true;
    }

    void end_stream() {
        zs_.end();
        stream_open_ = false;
    }

    /// Inflate the next run of whole lines of the open zlib stream into
    /// member_owned_. Bytes after the last newline are held back until more
    /// output or the end of the stream arrives, so a cut file yields only
    /// complete lines.
    coro::CoroTask<bool> decode_next_stream(int fd, off_t& offset) {
        member_size_ = 0;
        member_pos_ = 0;
        member_data_ = nullptr;
        member_shared_.reset();

        std::size_t filled = tail_.size();
        if (member_owned_.size() < filled + INIT_OUT) {
            member_owned_.resize(filled + INIT_OUT);
        }
        std::memcpy(member_owned_.data(), tail_.data(), filled);
        tail_.clear();

        bool ended = false;
        bool cut = false;
        while (true) {
            if (zs_.avail_in() == 0 && next_read_ < file_size_) {
                if (comp_.size() < READ_CHUNK) comp_.resize(READ_CHUNK);
                const std::size_t want =
                    static_cast<std::size_t>(std::min<std::uint64_t>(
                        READ_CHUNK, file_size_ - next_read_));
                const ssize_t n = co_await dftracer::utils::runtime::io::pread(
                    fd, comp_.data(), want, next_read_);
                if (n <= 0) co_return false;
                zs_.set_input(comp_.data(), static_cast<std::size_t>(n));
                next_read_ += static_cast<std::uint64_t>(n);
            }
            if (member_owned_.size() - filled < INIT_OUT) {
                member_owned_.resize(filled + INIT_OUT);
            }
            const std::size_t space = member_owned_.size() - filled;
            zs_.set_output(member_owned_.data() + filled, space);
            const utilities::fileio::compress::InflateStatus rc = zs_.inflate();
            filled += space - zs_.avail_out();
            if (rc == utilities::fileio::compress::InflateStatus::StreamEnd) {
                ended = true;
                break;
            }
            if (rc != utilities::fileio::compress::InflateStatus::Ok &&
                rc != utilities::fileio::compress::InflateStatus::BufError) {
                co_return false;
            }
            if (zs_.avail_in() == 0 && next_read_ >= file_size_ &&
                zs_.avail_out() != 0) {
                cut = true;
                break;
            }
            if (filled >= INIT_OUT) {
                std::size_t keep = filled;
                while (keep > 0 && member_owned_[keep - 1] != '\n') --keep;
                if (keep > 0) {
                    tail_.assign(member_owned_.begin() + keep,
                                 member_owned_.begin() + filled);
                    filled = keep;
                    break;
                }
            }
        }

        if (ended) {
            comp_off_ = stream_base_ + zs_.total_in() + (stream_raw_ ? 8 : 0);
        } else if (cut) {
            while (filled > 0 && member_owned_[filled - 1] != '\n') --filled;
            comp_off_ = file_size_;
        }
        if (ended || cut) {
            end_stream();
            comp_.clear();
            next_read_ = comp_off_;
        }
        offset = static_cast<off_t>(comp_off_);
        member_data_ = member_owned_.data();
        member_size_ = filled;
        if (filled == 0 && ended && comp_off_ < file_size_) {
            co_return co_await decode_next_member(fd, offset);
        }
        co_return true;
    }

    coro::CoroTask<bool> decode_next_member(int fd, off_t& offset) {
        if (stream_open_) co_return co_await decode_next_stream(fd, offset);
        if (hinted_ && std::binary_search(big_heads_.begin(), big_heads_.end(),
                                          comp_off_)) {
            if (!begin_member_stream()) co_return false;
            co_return co_await decode_next_stream(fd, offset);
        }
        if (cache_) co_return co_await decode_next_member_cached(fd, offset);
        co_return co_await decode_next_member_local(fd, offset);
    }

    /// Stream the member at comp_off_ from its header, over the input already
    /// read for it.
    bool begin_member_stream() {
        // Cached decodes advance comp_off_ and leave no input behind, so the
        // stream reads from the member's offset. A local decode leaves in comp_
        // the input that starts at comp_off_, which the stream consumes first.
        if (cache_) {
            comp_.clear();
            next_read_ = comp_off_;
        }
        if (!start_stream(31)) return false;
        if (!comp_.empty()) {
            zs_.set_input(comp_.data(), comp_.size());
        }
        return true;
    }

    /// Fetch the member at comp_off_ through the cache (one decode shared
    /// across concurrent readers) and point the read window at it.
    coro::CoroTask<bool> decode_next_member_cached(int fd, off_t& offset) {
        member_size_ = 0;
        member_pos_ = 0;
        member_data_ = nullptr;
        member_shared_.reset();
        if (comp_off_ >= file_size_) co_return true;  // clean EOF

        const std::uint64_t off = comp_off_;
        const std::uint64_t fsize = file_size_;
        const std::size_t bound = bound_;
        MemberDecodeCache::Producer produce =
            [fd, off, fsize,
             bound]() -> coro::CoroTask<MemberDecodeCache::Bytes> {
            bool too_big = false;
            auto decoded =
                co_await decode_member_at(fd, off, fsize, bound, too_big);
            if (too_big) co_return nullptr;
            if (!decoded) {
                throw common::DFTUtilsException(common::ErrorCode::Compression,
                                                "gzip member decode failed");
            }
            co_return dstd::make_shared<const DecodedMember>(
                std::move(*decoded));
        };

        try {
            member_shared_ = co_await cache_->get_or_decode(file_token_, off,
                                                            std::move(produce));
        } catch (...) {
            co_return false;
        }
        if (!member_shared_) {
            if (!begin_member_stream()) co_return false;
            co_return co_await decode_next_stream(fd, offset);
        }

        member_data_ = member_shared_->data.data();
        member_size_ = member_shared_->data.size();
        comp_off_ += member_shared_->compressed_size;
        offset = static_cast<off_t>(comp_off_);
        co_return true;
    }

    /// Decode the member at comp_off_ into the reused local buffer, or leave
    /// member_size_ == 0 at a clean end of file.
    coro::CoroTask<bool> decode_next_member_local(int fd, off_t& offset) {
        member_size_ = 0;
        member_pos_ = 0;
        member_data_ = nullptr;
        if (comp_off_ >= file_size_) co_return true;  // clean EOF

        utilities::fileio::compress::DecompressResult res{};
        while (true) {
            if (!comp_.empty()) {
                const utilities::fileio::compress::GzipDecode status =
                    dec_.decompress_status(comp_.data(), comp_.size(),
                                           member_owned_.data(),
                                           member_owned_.size(), res);
                if (status == utilities::fileio::compress::GzipDecode::Ok)
                    break;
                if (status == utilities::fileio::compress::GzipDecode::
                                  InsufficientSpace) {
                    if (member_owned_.size() >= bound_) {
                        if (!begin_member_stream()) co_return false;
                        co_return co_await decode_next_stream(fd, offset);
                    }
                    member_owned_.resize(
                        std::min(member_owned_.size() * 2, bound_));
                    continue;
                }
                // BadData can just mean the member is not fully buffered yet;
                // pull more input before treating it as corrupt.
            }
            if (next_read_ >= file_size_) {
                // The last member was cut short: keep its complete lines.
                dstd::Vector<std::uint8_t> kept;
                auto keep =
                    utilities::fileio::compress::decode_truncated_member(
                        comp_.data(), comp_.size(), kept);
                if (!keep) co_return false;
                member_owned_.resize(*keep);
                std::memcpy(member_owned_.data(), kept.data(), *keep);
                member_data_ = member_owned_.data();
                member_size_ = *keep;
                comp_off_ = file_size_;
                comp_.clear();
                offset = static_cast<off_t>(comp_off_);
                co_return true;
            }
            // Each retry decodes the member from its start, so the buffered
            // input at least doubles per retry to keep a large member linear.
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(std::max(READ_CHUNK, comp_.size()),
                                        file_size_ - next_read_));
            const std::size_t old = comp_.size();
            comp_.resize(old + want);
            const ssize_t n = co_await dftracer::utils::runtime::io::pread(
                fd, comp_.data() + old, want, next_read_);
            if (n <= 0) co_return false;
            comp_.resize(old + static_cast<std::size_t>(n));
            next_read_ += static_cast<std::uint64_t>(n);
        }

        member_data_ = member_owned_.data();
        member_size_ = res.out_bytes;
        comp_off_ += res.in_bytes;
        const std::size_t rest = comp_.size() - res.in_bytes;
        std::memmove(comp_.data(), comp_.data() + res.in_bytes, rest);
        comp_.resize(rest);
        offset = static_cast<off_t>(comp_off_);
        co_return true;
    }

    /// Decode the single gzip member starting at `off`, returning its bytes
    /// and the compressed size it consumed. nullopt on read/decode failure.
    /// Self-contained (own decompressor and buffers) so it is safe to run as a
    /// cache producer independent of any reader's state.
    static coro::CoroTask<dstd::Optional<DecodedMember>> decode_member_at(
        int fd, std::uint64_t off, std::uint64_t file_size, std::size_t max_out,
        bool& too_big) {
        too_big = false;
        utilities::fileio::compress::GzipMemberDecompressor dec;
        if (!dec.valid()) co_return dstd::nullopt;
        dstd::Vector<unsigned char> comp;
        dstd::Vector<std::uint8_t> out(INIT_OUT);
        std::uint64_t next_read = off;
        utilities::fileio::compress::DecompressResult res{};

        while (true) {
            if (!comp.empty()) {
                const utilities::fileio::compress::GzipDecode status =
                    dec.decompress_status(comp.data(), comp.size(), out.data(),
                                          out.size(), res);
                if (status == utilities::fileio::compress::GzipDecode::Ok)
                    break;
                if (status == utilities::fileio::compress::GzipDecode::
                                  InsufficientSpace) {
                    if (out.size() >= max_out) {
                        too_big = true;
                        co_return dstd::nullopt;
                    }
                    out.resize(std::min(out.size() * 2, max_out));
                    continue;
                }
            }
            if (next_read >= file_size) {
                // The last member was cut short: keep its complete lines.
                auto keep =
                    utilities::fileio::compress::decode_truncated_member(
                        comp.data(), comp.size(), out);
                if (!keep) co_return dstd::nullopt;
                out.resize(*keep);
                co_return DecodedMember{std::move(out), comp.size()};
            }
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

        out.resize(res.out_bytes);
        co_return DecodedMember{std::move(out), res.in_bytes};
    }

    utilities::fileio::compress::GzipMemberDecompressor dec_;
    dstd::RawBuffer<unsigned char> comp_;
    dstd::RawBuffer<unsigned char> member_owned_;
    MemberDecodeCache::Bytes member_shared_;
    const unsigned char* member_data_ = nullptr;
    std::size_t member_size_ = 0;
    std::size_t member_pos_ = 0;
    std::uint64_t comp_off_ = 0;
    std::uint64_t next_read_ = 0;
    std::uint64_t file_size_ = 0;
    bool exhausted_ = false;

    utilities::fileio::compress::GzipStreamInflater zs_;
    dstd::Vector<unsigned char> tail_;
    std::uint64_t stream_base_ = 0;
    bool stream_open_ = false;
    bool stream_raw_ = false;

    MemberDecodeCache* cache_ = nullptr;
    std::uint64_t file_token_ = 0;

    std::size_t bound_ = BOUND_DEFAULT;
    dstd::Vector<std::uint64_t> big_heads_;
    bool hinted_ = false;
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_INFLATER_H
