#include <dftracer/utils/utilities/fileio/compress/gzip_stream_inflater.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/fileio/compress/zng_stream.h>

#include <atomic>
#include <mutex>

namespace dftracer::utils::utilities::fileio::compress {

namespace {

std::atomic<std::uint64_t> CHECKSUM_STREAMS{0};

// zlib-ng fills its CPU-feature function table on first use without a lock.
// Run one deflate and one inflate once, before any concurrent use.
void warm_up() {
    static std::once_flag once;
    std::call_once(once, [] {
        unsigned char src[64];
        for (std::size_t i = 0; i < sizeof(src); ++i) {
            src[i] = static_cast<unsigned char>(i);
        }
        unsigned char comp[128];
        unsigned char back[64];

        zng_stream def{};
        if (zng_deflateInit2(&def, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8,
                             Z_DEFAULT_STRATEGY) != Z_OK) {
            return;
        }
        def.next_in = src;
        def.avail_in = sizeof(src);
        def.next_out = comp;
        def.avail_out = sizeof(comp);
        zng_deflate(&def, Z_FINISH);
        const auto comp_len =
            static_cast<std::uint32_t>(sizeof(comp) - def.avail_out);
        zng_deflateEnd(&def);

        zng_stream inf{};
        if (zng_inflateInit2(&inf, 31) == Z_OK) {
            inf.next_in = comp;
            inf.avail_in = comp_len;
            inf.next_out = back;
            inf.avail_out = sizeof(back);
            zng_inflate(&inf, Z_FINISH);
            zng_inflateEnd(&inf);
        }
    });
}

InflateStatus to_status(int rc) {
    switch (rc) {
        case Z_OK:
            return InflateStatus::Ok;
        case Z_BUF_ERROR:
            return InflateStatus::BufError;
        case Z_STREAM_END:
            return InflateStatus::StreamEnd;
        case Z_DATA_ERROR:
            return InflateStatus::DataError;
        default:
            return InflateStatus::Error;
    }
}

}  // namespace

std::int32_t open_inflate_stream(zng_stream& zs, int window_bits) {
    warm_up();
    if (window_bits > 0) {
        CHECKSUM_STREAMS.fetch_add(1, std::memory_order_relaxed);
    }
    return zng_inflateInit2(&zs, window_bits);
}

std::uint64_t zlib_checksum_streams_opened() {
    return CHECKSUM_STREAMS.load(std::memory_order_relaxed);
}

struct GzipStreamInflater::Impl {
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
        phase = raw ? Phase::Body : Phase::Header;
        field = Field::Magic1;
        flags = 0;
        left = 0;
        stored_hcrc = 0;
        head_crc = 0;
        crc = 0;
        size = 0;
        head_bytes = 0;
        trail_pos = 0;
    }

    void to_extra() {
        if (flags & FLAG_EXTRA) {
            field = Field::ExtraLen0;
        } else {
            to_name();
        }
    }
    void to_name() {
        if (flags & FLAG_NAME) {
            field = Field::Name;
        } else {
            to_comment();
        }
    }
    void to_comment() {
        if (flags & FLAG_COMMENT) {
            field = Field::Comment;
        } else {
            to_header_crc();
        }
    }
    void to_header_crc() {
        if (flags & FLAG_HCRC) {
            field = Field::HeaderCrc0;
        } else {
            phase = Phase::Body;
        }
    }

    /// Consume one header byte. False when the header is not a valid gzip
    /// header.
    bool header_byte(unsigned char byte) {
        if (field != Field::HeaderCrc0 && field != Field::HeaderCrc1) {
            head_crc = gzip_crc32(head_crc, &byte, 1);
        }
        switch (field) {
            case Field::Magic1:
                field = Field::Magic2;
                return byte == 0x1f;
            case Field::Magic2:
                field = Field::Method;
                return byte == 0x8b;
            case Field::Method:
                field = Field::Flags;
                return byte == 8;
            case Field::Flags:
                flags = byte;
                left = FIXED_BYTES;
                field = Field::Fixed;
                return (byte & FLAG_RESERVED) == 0;
            case Field::Fixed:
                if (--left == 0) to_extra();
                return true;
            case Field::ExtraLen0:
                left = byte;
                field = Field::ExtraLen1;
                return true;
            case Field::ExtraLen1:
                left |= static_cast<unsigned>(byte) << 8;
                if (left == 0) {
                    to_name();
                } else {
                    field = Field::Extra;
                }
                return true;
            case Field::Extra:
                if (--left == 0) to_name();
                return true;
            case Field::Name:
                if (byte == 0) to_comment();
                return true;
            case Field::Comment:
                if (byte == 0) to_header_crc();
                return true;
            case Field::HeaderCrc0:
                stored_hcrc = byte;
                field = Field::HeaderCrc1;
                return true;
            case Field::HeaderCrc1:
                stored_hcrc |= static_cast<unsigned>(byte) << 8;
                phase = Phase::Body;
                return stored_hcrc == (head_crc & 0xffffu);
        }
        return false;
    }

    InflateStatus inflate(InflateFlush flush) {
        const std::int32_t mode =
            flush == InflateFlush::Block ? Z_BLOCK : Z_NO_FLUSH;
        if (raw) return to_status(zng_inflate(&zs, mode));
        bool first = true;
        while (true) {
            if (phase == Phase::Header) {
                if (zs.avail_in == 0) {
                    return first ? InflateStatus::BufError : InflateStatus::Ok;
                }
                while (phase == Phase::Header && zs.avail_in > 0) {
                    const unsigned char byte = *zs.next_in++;
                    --zs.avail_in;
                    ++head_bytes;
                    if (!header_byte(byte)) return InflateStatus::DataError;
                }
                first = false;
            } else if (phase == Phase::Body) {
                std::uint8_t* const out = zs.next_out;
                const std::uint32_t before = zs.avail_out;
                const int rc = zng_inflate(&zs, mode);
                const std::size_t got = before - zs.avail_out;
                crc = gzip_crc32(crc, out, got);
                size += static_cast<std::uint32_t>(got);
                if (rc != Z_STREAM_END) return to_status(rc);
                phase = Phase::Trailer;
                first = false;
            } else if (phase == Phase::Trailer) {
                while (trail_pos < TRAILER_SIZE && zs.avail_in > 0) {
                    trailer[trail_pos++] = *zs.next_in++;
                    --zs.avail_in;
                }
                if (trail_pos < TRAILER_SIZE) {
                    return first ? InflateStatus::BufError : InflateStatus::Ok;
                }
                phase = Phase::Done;
                if (read_le32(trailer) != crc ||
                    read_le32(trailer + 4) != size) {
                    return InflateStatus::DataError;
                }
            } else {
                return InflateStatus::StreamEnd;
            }
        }
    }

    zng_stream zs{};
    std::uint64_t head_bytes = 0;
    std::uint32_t crc = 0;
    std::uint32_t size = 0;
    std::uint32_t head_crc = 0;
    unsigned left = 0;
    unsigned stored_hcrc = 0;
    unsigned trail_pos = 0;
    unsigned char trailer[TRAILER_SIZE] = {};
    unsigned char flags = 0;
    Phase phase = Phase::Header;
    Field field = Field::Magic1;
    bool raw = false;
    bool open = false;
};

GzipStreamInflater::GzipStreamInflater() = default;

GzipStreamInflater::~GzipStreamInflater() { end(); }

bool GzipStreamInflater::init(int window_bits) {
    end();
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->zs = zng_stream{};
    impl_->raw = window_bits < 0;
    if (open_inflate_stream(impl_->zs, impl_->raw ? window_bits : -MAX_WBITS) !=
        Z_OK) {
        return false;
    }
    impl_->open = true;
    impl_->restart_member();
    return true;
}

void GzipStreamInflater::end() {
    if (impl_ && impl_->open) zng_inflateEnd(&impl_->zs);
    if (impl_) impl_->open = false;
}

bool GzipStreamInflater::reset() {
    if (!impl_ || zng_inflateReset(&impl_->zs) != Z_OK) return false;
    impl_->restart_member();
    return true;
}

void GzipStreamInflater::set_input(const unsigned char* data,
                                   std::size_t size) {
    impl_->zs.next_in = data;
    impl_->zs.avail_in = static_cast<std::uint32_t>(size);
}

void GzipStreamInflater::set_output(unsigned char* data, std::size_t size) {
    impl_->zs.next_out = data;
    impl_->zs.avail_out = static_cast<std::uint32_t>(size);
}

std::size_t GzipStreamInflater::avail_in() const {
    return impl_ ? impl_->zs.avail_in : 0;
}

std::size_t GzipStreamInflater::avail_out() const {
    return impl_ ? impl_->zs.avail_out : 0;
}

std::uint64_t GzipStreamInflater::total_in() const {
    if (!impl_) return 0;
    const Impl& s = *impl_;
    return s.raw ? s.zs.total_in : s.head_bytes + s.zs.total_in + s.trail_pos;
}

int GzipStreamInflater::data_type() const {
    return impl_ ? impl_->zs.data_type : 0;
}

InflateStatus GzipStreamInflater::inflate(InflateFlush flush) {
    return impl_->inflate(flush);
}

bool GzipStreamInflater::prime(int bits, int value) {
    return zng_inflatePrime(&impl_->zs, bits, value) == Z_OK;
}

bool GzipStreamInflater::set_dictionary(const unsigned char* data,
                                        std::size_t size) {
    return zng_inflateSetDictionary(&impl_->zs, data,
                                    static_cast<std::uint32_t>(size)) == Z_OK;
}

}  // namespace dftracer::utils::utilities::fileio::compress
