#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_STREAM_INFLATER_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_STREAM_INFLATER_H

#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <zlib.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::utilities::fileio::compress {

/// Streaming inflate of one gzip member with the interface of a zlib gzip
/// stream, but the CRC32 is computed by libdeflate instead of zlib's crc32_z.
/// zlib inflates the raw deflate data; this class parses the header (including
/// the FHCRC field), checksums the output and checks the trailer CRC32 and
/// ISIZE. inflate() reports a bad header, CRC or size as Z_DATA_ERROR and
/// returns Z_STREAM_END only once the trailer has been verified.
///
/// A negative `window_bits` opens a plain zlib stream (a raw deflate piece
/// read from a restart point), which carries no CRC.
///
/// Set next_in, avail_in, next_out and avail_out on z() as for zlib. Not
/// thread-safe; one per stream.
class GzipStreamInflater {
   public:
    GzipStreamInflater() = default;
    GzipStreamInflater(const GzipStreamInflater&) = delete;
    GzipStreamInflater& operator=(const GzipStreamInflater&) = delete;
    ~GzipStreamInflater() { end(); }

    /// 15 + 16 (or 31) for a gzip member, a negative value for raw deflate.
    bool init(int window_bits) {
        end();
        zs_ = z_stream{};
        raw_ = window_bits < 0;
        if (inflateInit2(&zs_, raw_ ? window_bits : -MAX_WBITS) != Z_OK) {
            return false;
        }
        open_ = true;
        restart_member();
        return true;
    }

    void end() {
        if (open_) inflateEnd(&zs_);
        open_ = false;
    }

    /// Start the next member of a concatenated stream. The input and output
    /// pointers in z() are kept.
    bool reset() {
        if (inflateReset(&zs_) != Z_OK) return false;
        restart_member();
        return true;
    }

    z_stream& z() { return zs_; }

    /// Compressed bytes consumed so far: header, deflate data and trailer.
    std::uint64_t total_in() const {
        return raw_ ? zs_.total_in : head_bytes_ + zs_.total_in + trail_pos_;
    }

    /// Z_OK, Z_BUF_ERROR (no progress possible), Z_STREAM_END or an error
    /// code, as zlib's inflate().
    int inflate(int flush) {
        if (raw_) return ::inflate(&zs_, flush);
        bool first = true;
        while (true) {
            if (phase_ == Phase::Header) {
                if (zs_.avail_in == 0) return first ? Z_BUF_ERROR : Z_OK;
                while (phase_ == Phase::Header && zs_.avail_in > 0) {
                    const unsigned char byte = *zs_.next_in++;
                    --zs_.avail_in;
                    ++head_bytes_;
                    if (!header_byte(byte)) return Z_DATA_ERROR;
                }
                first = false;
            } else if (phase_ == Phase::Body) {
                Bytef* const out = zs_.next_out;
                const uInt before = zs_.avail_out;
                const int rc = ::inflate(&zs_, flush);
                const std::size_t got = before - zs_.avail_out;
                crc_ = gzip_crc32(crc_, out, got);
                size_ += static_cast<std::uint32_t>(got);
                if (rc != Z_STREAM_END) return rc;
                phase_ = Phase::Trailer;
                first = false;
            } else if (phase_ == Phase::Trailer) {
                while (trail_pos_ < TRAILER_SIZE && zs_.avail_in > 0) {
                    trailer_[trail_pos_++] = *zs_.next_in++;
                    --zs_.avail_in;
                }
                if (trail_pos_ < TRAILER_SIZE) {
                    return first ? Z_BUF_ERROR : Z_OK;
                }
                phase_ = Phase::Done;
                if (read_le32(trailer_) != crc_ ||
                    read_le32(trailer_ + 4) != size_) {
                    return Z_DATA_ERROR;
                }
            } else {
                return Z_STREAM_END;
            }
        }
    }

   private:
    enum class Phase : std::uint8_t { Header, Body, Trailer, Done };
    enum class Field : std::uint8_t {
        Magic1,
        Magic2,
        Method,
        Flags,
        Fixed,
        ExtraLen0,
        ExtraLen1,
        Extra,
        Name,
        Comment,
        HeaderCrc0,
        HeaderCrc1,
    };

    static constexpr unsigned TRAILER_SIZE = 8;
    static constexpr unsigned char FLAG_HCRC = 2;
    static constexpr unsigned char FLAG_EXTRA = 4;
    static constexpr unsigned char FLAG_NAME = 8;
    static constexpr unsigned char FLAG_COMMENT = 16;
    static constexpr unsigned char FLAG_RESERVED = 0xe0;
    static constexpr unsigned FIXED_BYTES = 6;

    static std::uint32_t read_le32(const unsigned char* p) {
        return static_cast<std::uint32_t>(p[0]) |
               static_cast<std::uint32_t>(p[1]) << 8 |
               static_cast<std::uint32_t>(p[2]) << 16 |
               static_cast<std::uint32_t>(p[3]) << 24;
    }

    void restart_member() {
        phase_ = raw_ ? Phase::Body : Phase::Header;
        field_ = Field::Magic1;
        flags_ = 0;
        left_ = 0;
        stored_hcrc_ = 0;
        head_crc_ = 0;
        crc_ = 0;
        size_ = 0;
        head_bytes_ = 0;
        trail_pos_ = 0;
    }

    void to_extra() {
        if (flags_ & FLAG_EXTRA) {
            field_ = Field::ExtraLen0;
        } else {
            to_name();
        }
    }
    void to_name() {
        if (flags_ & FLAG_NAME) {
            field_ = Field::Name;
        } else {
            to_comment();
        }
    }
    void to_comment() {
        if (flags_ & FLAG_COMMENT) {
            field_ = Field::Comment;
        } else {
            to_header_crc();
        }
    }
    void to_header_crc() {
        if (flags_ & FLAG_HCRC) {
            field_ = Field::HeaderCrc0;
        } else {
            phase_ = Phase::Body;
        }
    }

    /// Consume one header byte. False when the header is not a valid gzip
    /// header.
    bool header_byte(unsigned char byte) {
        if (field_ != Field::HeaderCrc0 && field_ != Field::HeaderCrc1) {
            head_crc_ = gzip_crc32(head_crc_, &byte, 1);
        }
        switch (field_) {
            case Field::Magic1:
                field_ = Field::Magic2;
                return byte == 0x1f;
            case Field::Magic2:
                field_ = Field::Method;
                return byte == 0x8b;
            case Field::Method:
                field_ = Field::Flags;
                return byte == 8;
            case Field::Flags:
                flags_ = byte;
                left_ = FIXED_BYTES;
                field_ = Field::Fixed;
                return (byte & FLAG_RESERVED) == 0;
            case Field::Fixed:
                if (--left_ == 0) to_extra();
                return true;
            case Field::ExtraLen0:
                left_ = byte;
                field_ = Field::ExtraLen1;
                return true;
            case Field::ExtraLen1:
                left_ |= static_cast<unsigned>(byte) << 8;
                if (left_ == 0) {
                    to_name();
                } else {
                    field_ = Field::Extra;
                }
                return true;
            case Field::Extra:
                if (--left_ == 0) to_name();
                return true;
            case Field::Name:
                if (byte == 0) to_comment();
                return true;
            case Field::Comment:
                if (byte == 0) to_header_crc();
                return true;
            case Field::HeaderCrc0:
                stored_hcrc_ = byte;
                field_ = Field::HeaderCrc1;
                return true;
            case Field::HeaderCrc1:
                stored_hcrc_ |= static_cast<unsigned>(byte) << 8;
                phase_ = Phase::Body;
                return stored_hcrc_ == (head_crc_ & 0xffffu);
        }
        return false;
    }

    z_stream zs_{};
    std::uint64_t head_bytes_ = 0;
    std::uint32_t crc_ = 0;
    std::uint32_t size_ = 0;
    std::uint32_t head_crc_ = 0;
    unsigned left_ = 0;
    unsigned stored_hcrc_ = 0;
    unsigned trail_pos_ = 0;
    unsigned char trailer_[TRAILER_SIZE] = {};
    unsigned char flags_ = 0;
    Phase phase_ = Phase::Header;
    Field field_ = Field::Magic1;
    bool raw_ = false;
    bool open_ = false;
};

}  // namespace dftracer::utils::utilities::fileio::compress

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_STREAM_INFLATER_H
