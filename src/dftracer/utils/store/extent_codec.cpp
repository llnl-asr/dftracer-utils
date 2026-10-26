#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/store/extent_codec.h>
#include <zstd.h>

#include <cstring>
#include <limits>

namespace dftracer::utils::store {

namespace {

/// Level 1: extents are written on the appending thread, so speed comes first.
constexpr int EXTENT_ZSTD_LEVEL = 1;

dstd::Unexpected<dstd::Error> parse_error() {
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Parse));
}

dstd::Unexpected<dstd::Error> invalid_error() {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument));
}

struct CompressContext {
    ZSTD_CCtx* ctx = ZSTD_createCCtx();
    ~CompressContext() { ZSTD_freeCCtx(ctx); }
};

struct DecompressContext {
    ZSTD_DCtx* ctx = ZSTD_createDCtx();
    ~DecompressContext() { ZSTD_freeDCtx(ctx); }
};

std::size_t align_up(std::size_t value, std::size_t align) {
    return (value + align - 1) / align * align;
}

}  // namespace

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_extent(
    ByteView raw, std::size_t align) {
    if (raw.empty() || raw.size() > EXTENT_MAX_RAW_BYTES || align == 0) {
        return invalid_error();
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(raw.data());
    dstd::Vector<std::uint8_t> out;
    out.resize(EXTENT_HEADER_BYTES + ZSTD_compressBound(raw.size()));
    thread_local CompressContext context;
    std::size_t payload = 0;
    ExtentCodec codec = ExtentCodec::None;
    if (context.ctx != nullptr) {
        const std::size_t compressed =
            ZSTD_compressCCtx(context.ctx, out.data() + EXTENT_HEADER_BYTES,
                              out.size() - EXTENT_HEADER_BYTES, data,
                              raw.size(), EXTENT_ZSTD_LEVEL);
        if (ZSTD_isError(compressed) == 0 &&
            compressed <= raw.size() - raw.size() / 8) {
            payload = compressed;
            codec = ExtentCodec::Zstd;
        }
    }
    if (codec == ExtentCodec::None) {
        std::memcpy(out.data() + EXTENT_HEADER_BYTES, data, raw.size());
        payload = raw.size();
    }
    out.resize(EXTENT_HEADER_BYTES + payload);
    out.resize(align_up(out.size(), align));
    std::memset(out.data(), 0, EXTENT_HEADER_BYTES);
    out[0] = static_cast<std::uint8_t>(codec);
    write_u32_le(out.data() + 4, static_cast<std::uint32_t>(raw.size()));
    write_u32_le(out.data() + 8, static_cast<std::uint32_t>(payload));
    return out;
}

dstd::Expected<void, dstd::Error> decode_extent(
    dstd::Vector<std::uint8_t>& stored) {
    if (stored.size() < EXTENT_HEADER_BYTES) return parse_error();
    const std::uint8_t* header = stored.data();
    const std::uint32_t raw = read_u32_le(header + 4);
    const std::uint32_t payload = read_u32_le(header + 8);
    if (header[1] != 0 || header[2] != 0 || header[3] != 0 ||
        read_u32_le(header + 12) != 0 || raw == 0 ||
        raw > EXTENT_MAX_RAW_BYTES ||
        payload > stored.size() - EXTENT_HEADER_BYTES) {
        return parse_error();
    }
    switch (static_cast<ExtentCodec>(header[0])) {
        case ExtentCodec::None: {
            if (payload != raw) return parse_error();
            std::memmove(stored.data(), stored.data() + EXTENT_HEADER_BYTES,
                         raw);
            stored.resize(raw);
            return {};
        }
        case ExtentCodec::Zstd: {
            dstd::Vector<std::uint8_t> out;
            out.resize(raw);
            thread_local DecompressContext context;
            if (context.ctx == nullptr) return parse_error();
            const std::size_t got = ZSTD_decompressDCtx(
                context.ctx, out.data(), raw,
                stored.data() + EXTENT_HEADER_BYTES, payload);
            if (ZSTD_isError(got) != 0 || got != raw) return parse_error();
            stored = std::move(out);
            return {};
        }
    }
    return parse_error();
}

}  // namespace dftracer::utils::store
