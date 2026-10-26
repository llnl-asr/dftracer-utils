#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/store/page_codec.h>
#include <zstd.h>

#include <cstring>
#include <limits>

namespace dftracer::utils::store {

namespace {

/// Level 1: pages are written on the appending thread, so speed comes first.
constexpr int PAGE_ZSTD_LEVEL = 1;

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

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_page(
    ByteView raw, std::size_t align) {
    if (raw.empty() || raw.size() > PAGE_MAX_RAW_BYTES || align == 0) {
        return invalid_error();
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(raw.data());
    dstd::Vector<std::uint8_t> out;
    out.resize(PAGE_HEADER_BYTES + ZSTD_compressBound(raw.size()));
    thread_local CompressContext context;
    std::size_t payload = 0;
    PageCodec codec = PageCodec::None;
    if (context.ctx != nullptr) {
        const std::size_t compressed = ZSTD_compressCCtx(
            context.ctx, out.data() + PAGE_HEADER_BYTES,
            out.size() - PAGE_HEADER_BYTES, data, raw.size(), PAGE_ZSTD_LEVEL);
        if (ZSTD_isError(compressed) == 0 &&
            compressed <= raw.size() - raw.size() / 8) {
            payload = compressed;
            codec = PageCodec::Zstd;
        }
    }
    if (codec == PageCodec::None) {
        std::memcpy(out.data() + PAGE_HEADER_BYTES, data, raw.size());
        payload = raw.size();
    }
    out.resize(PAGE_HEADER_BYTES + payload);
    out.resize(align_up(out.size(), align));
    std::memset(out.data(), 0, PAGE_HEADER_BYTES);
    out[0] = static_cast<std::uint8_t>(codec);
    write_u32_le(out.data() + 4, static_cast<std::uint32_t>(raw.size()));
    write_u32_le(out.data() + 8, static_cast<std::uint32_t>(payload));
    return out;
}

dstd::Expected<void, dstd::Error> decode_page(
    dstd::Vector<std::uint8_t>& stored) {
    if (stored.size() < PAGE_HEADER_BYTES) return parse_error();
    const std::uint8_t* header = stored.data();
    const std::uint32_t raw = read_u32_le(header + 4);
    const std::uint32_t payload = read_u32_le(header + 8);
    if (header[1] != 0 || header[2] != 0 || header[3] != 0 ||
        read_u32_le(header + 12) != 0 || raw == 0 || raw > PAGE_MAX_RAW_BYTES ||
        payload > stored.size() - PAGE_HEADER_BYTES) {
        return parse_error();
    }
    switch (static_cast<PageCodec>(header[0])) {
        case PageCodec::None: {
            if (payload != raw) return parse_error();
            std::memmove(stored.data(), stored.data() + PAGE_HEADER_BYTES, raw);
            stored.resize(raw);
            return {};
        }
        case PageCodec::Zstd: {
            dstd::Vector<std::uint8_t> out;
            out.resize(raw);
            thread_local DecompressContext context;
            if (context.ctx == nullptr) return parse_error();
            const std::size_t got =
                ZSTD_decompressDCtx(context.ctx, out.data(), raw,
                                    stored.data() + PAGE_HEADER_BYTES, payload);
            if (ZSTD_isError(got) != 0 || got != raw) return parse_error();
            stored = std::move(out);
            return {};
        }
    }
    return parse_error();
}

}  // namespace dftracer::utils::store
