#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_INFLATER_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_INFLATER_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/index/gzip/gzip_member_record.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/reader/internal/member_decode_cache.h>
#include <sys/stat.h>
#include <zlib.h>

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::reader::internal {

namespace compress = dftracer::utils::utilities::fileio::compress;

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
        const std::string& window, std::size_t expected_out = 0) {
        DFTRACER_UTILS_LOG_DEBUG("Seeking to member %" PRIu64
                                 ": c_offset=%" PRIu64 ", uc_offset=%" PRIu64,
                                 member.member_idx, member.c_offset,
                                 member.uc_offset);
        reset();
        if (!co_await begin_at(fd, offset, member.c_offset, expected_out)) {
            co_return false;
        }
        using dftracer::utils::index::gzip::GzipRecordKind;
        if (member.kind == GzipRecordKind::MEMBER) co_return true;
        if (member.kind == GzipRecordKind::HEAD) {
            co_return start_stream(31);
        }
        if (window.empty() || member.bits > 7) co_return false;
        unsigned char prime = 0;
        if (member.bits > 0) {
            if (co_await dftracer::utils::io::pread(
                    fd, &prime, 1, static_cast<off_t>(member.c_offset - 1)) !=
                1) {
                co_return false;
            }
        }
        if (!start_stream(-15)) co_return false;
        if (member.bits > 0 &&
            inflatePrime(&zs_, member.bits, prime >> (8 - member.bits)) !=
                Z_OK) {
            end_stream();
            co_return false;
        }
        if (inflateSetDictionary(&zs_,
                                 reinterpret_cast<const Bytef*>(window.data()),
                                 static_cast<uInt>(window.size())) != Z_OK) {
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
            if (member_pos_ >= member_len_) {
                if (exhausted_) break;
                if (!co_await decode_next_member(fd, offset)) co_return false;
                if (member_len_ == 0) {
                    exhausted_ = true;
                    break;
                }
            }
            const std::size_t avail = member_len_ - member_pos_;
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
            if (member_pos_ >= member_len_) {
                if (exhausted_) break;
                if (!co_await decode_next_member(fd, offset)) co_return false;
                if (member_len_ == 0) {
                    exhausted_ = true;
                    break;
                }
            }
            const std::size_t avail = member_len_ - member_pos_;
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
        member_len_ = 0;
        member_pos_ = 0;
        comp_off_ = 0;
        next_read_ = 0;
        file_size_ = 0;
        exhausted_ = false;
    }

    bool is_at_end() const { return exhausted_ && member_pos_ >= member_len_; }

   private:
    static constexpr std::size_t READ_CHUNK = 1u << 20;
    static constexpr std::size_t INIT_OUT = 1u << 20;

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
                std::max<std::size_t>(INIT_OUT, expected_out);
            if (member_owned_.size() < want) member_owned_.resize(want);
        }
        co_return dec_.valid();
    }

    bool start_stream(int window_bits) {
        zs_ = z_stream{};
        if (inflateInit2(&zs_, window_bits) != Z_OK) return false;
        stream_open_ = true;
        stream_base_ = comp_off_;
        stream_raw_ = window_bits < 0;
        return true;
    }

    void end_stream() {
        if (stream_open_) inflateEnd(&zs_);
        stream_open_ = false;
    }

    /// Inflate the next run of whole lines of the open zlib stream into
    /// member_owned_. Bytes after the last newline are held back until more
    /// output or the end of the stream arrives, so a cut file yields only
    /// complete lines.
    coro::CoroTask<bool> decode_next_stream(int fd, off_t& offset) {
        member_len_ = 0;
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
            if (zs_.avail_in == 0 && next_read_ < file_size_) {
                if (comp_.size() < READ_CHUNK) comp_.resize(READ_CHUNK);
                const std::size_t want =
                    static_cast<std::size_t>(std::min<std::uint64_t>(
                        READ_CHUNK, file_size_ - next_read_));
                const ssize_t n = co_await dftracer::utils::io::pread(
                    fd, comp_.data(), want, static_cast<off_t>(next_read_));
                if (n <= 0) co_return false;
                zs_.next_in = comp_.data();
                zs_.avail_in = static_cast<uInt>(n);
                next_read_ += static_cast<std::uint64_t>(n);
            }
            if (member_owned_.size() - filled < INIT_OUT) {
                member_owned_.resize(filled + INIT_OUT);
            }
            const std::size_t space = member_owned_.size() - filled;
            zs_.next_out = member_owned_.data() + filled;
            zs_.avail_out = static_cast<uInt>(space);
            const int rc = inflate(&zs_, Z_NO_FLUSH);
            filled += space - zs_.avail_out;
            if (rc == Z_STREAM_END) {
                ended = true;
                break;
            }
            if (rc != Z_OK && rc != Z_BUF_ERROR) co_return false;
            if (zs_.avail_in == 0 && next_read_ >= file_size_ &&
                zs_.avail_out != 0) {
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
            comp_off_ = stream_base_ + zs_.total_in + (stream_raw_ ? 8 : 0);
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
        member_len_ = filled;
        if (filled == 0 && ended && comp_off_ < file_size_) {
            co_return co_await decode_next_member(fd, offset);
        }
        co_return true;
    }

    coro::CoroTask<bool> decode_next_member(int fd, off_t& offset) {
        if (stream_open_) co_return co_await decode_next_stream(fd, offset);
        if (cache_) co_return co_await decode_next_member_cached(fd, offset);
        co_return co_await decode_next_member_local(fd, offset);
    }

    /// Fetch the member at comp_off_ through the cache (one decode shared
    /// across concurrent readers) and point the read window at it.
    coro::CoroTask<bool> decode_next_member_cached(int fd, off_t& offset) {
        member_len_ = 0;
        member_pos_ = 0;
        member_data_ = nullptr;
        member_shared_.reset();
        if (comp_off_ >= file_size_) co_return true;  // clean EOF

        const std::uint64_t off = comp_off_;
        const std::uint64_t fsize = file_size_;
        MemberDecodeCache::Producer produce =
            [fd, off, fsize]() -> coro::CoroTask<MemberDecodeCache::Bytes> {
            auto decoded = co_await decode_member_at(fd, off, fsize);
            if (!decoded) {
                throw DFTUtilsException(ErrorCode::COMPRESSION,
                                        "gzip member decode failed");
            }
            co_return std::make_shared<const DecodedMember>(
                std::move(*decoded));
        };

        try {
            member_shared_ =
                co_await cache_->get_or_decode(file_token_, off, produce);
        } catch (...) {
            co_return false;
        }
        if (!member_shared_) co_return false;

        member_data_ = member_shared_->data.data();
        member_len_ = member_shared_->data.size();
        comp_off_ += member_shared_->compressed_size;
        offset = static_cast<off_t>(comp_off_);
        co_return true;
    }

    /// Decode the member at comp_off_ into the reused local buffer, or leave
    /// member_len_ == 0 at a clean end of file.
    coro::CoroTask<bool> decode_next_member_local(int fd, off_t& offset) {
        member_len_ = 0;
        member_pos_ = 0;
        member_data_ = nullptr;
        if (comp_off_ >= file_size_) co_return true;  // clean EOF

        compress::DecompressResult res{};
        while (true) {
            if (!comp_.empty()) {
                const compress::GzipDecode status = dec_.decompress_status(
                    comp_.data(), comp_.size(), member_owned_.data(),
                    member_owned_.size(), res);
                if (status == compress::GzipDecode::Ok) break;
                if (status == compress::GzipDecode::InsufficientSpace) {
                    member_owned_.resize(member_owned_.size() * 2);
                    continue;
                }
                // BadData can just mean the member is not fully buffered yet;
                // pull more input before treating it as corrupt.
            }
            if (next_read_ >= file_size_) {
                // The last member was cut short: keep its complete lines.
                auto keep = compress::decode_truncated_member(
                    comp_.data(), comp_.size(), member_owned_);
                if (!keep) co_return false;
                member_data_ = member_owned_.data();
                member_len_ = *keep;
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
            const ssize_t n = co_await dftracer::utils::io::pread(
                fd, comp_.data() + old, want, static_cast<off_t>(next_read_));
            if (n <= 0) co_return false;
            comp_.resize(old + static_cast<std::size_t>(n));
            next_read_ += static_cast<std::uint64_t>(n);
        }

        member_data_ = member_owned_.data();
        member_len_ = res.out_bytes;
        comp_off_ += res.in_bytes;
        comp_.erase(comp_.begin(),
                    comp_.begin() + static_cast<std::ptrdiff_t>(res.in_bytes));
        offset = static_cast<off_t>(comp_off_);
        co_return true;
    }

    /// Decode the single gzip member starting at `off`, returning its bytes
    /// and the compressed size it consumed. nullopt on read/decode failure.
    /// Self-contained (own decompressor and buffers) so it is safe to run as a
    /// cache producer independent of any reader's state.
    static coro::CoroTask<std::optional<DecodedMember>> decode_member_at(
        int fd, std::uint64_t off, std::uint64_t file_size) {
        compress::GzipMemberDecompressor dec;
        if (!dec.valid()) co_return std::nullopt;
        std::vector<unsigned char> comp;
        std::vector<std::uint8_t> out(INIT_OUT);
        std::uint64_t next_read = off;
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
            }
            if (next_read >= file_size) {
                // The last member was cut short: keep its complete lines.
                auto keep = compress::decode_truncated_member(comp.data(),
                                                              comp.size(), out);
                if (!keep) co_return std::nullopt;
                out.resize(*keep);
                co_return DecodedMember{std::move(out), comp.size()};
            }
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

        out.resize(res.out_bytes);
        co_return DecodedMember{std::move(out), res.in_bytes};
    }

    compress::GzipMemberDecompressor dec_;
    std::vector<unsigned char> comp_;
    std::vector<unsigned char> member_owned_;
    MemberDecodeCache::Bytes member_shared_;
    const unsigned char* member_data_ = nullptr;
    std::size_t member_len_ = 0;
    std::size_t member_pos_ = 0;
    std::uint64_t comp_off_ = 0;
    std::uint64_t next_read_ = 0;
    std::uint64_t file_size_ = 0;
    bool exhausted_ = false;

    z_stream zs_{};
    std::vector<unsigned char> tail_;
    std::uint64_t stream_base_ = 0;
    bool stream_open_ = false;
    bool stream_raw_ = false;

    MemberDecodeCache* cache_ = nullptr;
    std::uint64_t file_token_ = 0;
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_INFLATER_H
