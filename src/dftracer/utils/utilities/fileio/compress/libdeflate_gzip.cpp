#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <libdeflate.h>
#include <zlib.h>

#include <algorithm>
#include <climits>
#include <cstddef>

namespace dftracer::utils::utilities::fileio::compress {

namespace {

// libdeflate selects its crc32, adler32 and inflate kernels (and reads the CPU
// features) on first use by writing unsynchronized globals. Resolve them once,
// under a function-local static, so concurrent codecs only read them.
void resolve_dispatch() {
    static const bool RESOLVED = [] {
        static const unsigned char EMPTY_BLOCK[] = {0x03, 0x00};
        unsigned char byte = 0;
        libdeflate_crc32(0, &byte, 0);
        libdeflate_adler32(1, &byte, 0);
        if (libdeflate_decompressor* d = libdeflate_alloc_decompressor()) {
            std::size_t n = 0;
            libdeflate_deflate_decompress(d, EMPTY_BLOCK, sizeof(EMPTY_BLOCK),
                                          &byte, 0, &n);
            libdeflate_free_decompressor(d);
        }
        return true;
    }();
    (void)RESOLVED;
}

}  // namespace

GzipMemberDecompressor::GzipMemberDecompressor()
    : d_((resolve_dispatch(), libdeflate_alloc_decompressor())) {}

GzipMemberDecompressor::~GzipMemberDecompressor() {
    if (d_) libdeflate_free_decompressor(d_);
}

GzipMemberDecompressor::GzipMemberDecompressor(
    GzipMemberDecompressor&& other) noexcept
    : d_(other.d_) {
    other.d_ = nullptr;
}

GzipMemberDecompressor& GzipMemberDecompressor::operator=(
    GzipMemberDecompressor&& other) noexcept {
    if (this != &other) {
        if (d_) libdeflate_free_decompressor(d_);
        d_ = other.d_;
        other.d_ = nullptr;
    }
    return *this;
}

GzipDecode GzipMemberDecompressor::decompress_status(
    const void* comp, std::size_t comp_len, void* out, std::size_t out_cap,
    DecompressResult& result) const {
    if (!d_) return GzipDecode::BadData;
    std::size_t actual_in = 0;
    std::size_t actual_out = 0;
    const libdeflate_result r = libdeflate_gzip_decompress_ex(
        d_, comp, comp_len, out, out_cap, &actual_in, &actual_out);
    switch (r) {
        case LIBDEFLATE_SUCCESS:
            result = DecompressResult{actual_out, actual_in};
            return GzipDecode::Ok;
        case LIBDEFLATE_SHORT_OUTPUT:
            return GzipDecode::ShortOutput;
        case LIBDEFLATE_INSUFFICIENT_SPACE:
            return GzipDecode::InsufficientSpace;
        default:
            return GzipDecode::BadData;
    }
}

std::optional<DecompressResult> GzipMemberDecompressor::decompress(
    const void* comp, std::size_t comp_len, void* out,
    std::size_t out_cap) const {
    DecompressResult result{};
    if (decompress_status(comp, comp_len, out, out_cap, result) ==
        GzipDecode::Ok) {
        return result;
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>>
GzipMemberDecompressor::decompress_member(const void* comp,
                                          std::size_t comp_len,
                                          std::size_t uncompressed_size) const {
    std::vector<std::uint8_t> out(uncompressed_size);
    auto res = decompress(comp, comp_len, out.data(), out.size());
    if (!res) return std::nullopt;
    out.resize(res->out_bytes);
    return out;
}

GzipMemberCompressor::GzipMemberCompressor(int level)
    : c_((resolve_dispatch(), libdeflate_alloc_compressor(level))) {}

GzipMemberCompressor::~GzipMemberCompressor() {
    if (c_) libdeflate_free_compressor(c_);
}

GzipMemberCompressor::GzipMemberCompressor(
    GzipMemberCompressor&& other) noexcept
    : c_(other.c_) {
    other.c_ = nullptr;
}

GzipMemberCompressor& GzipMemberCompressor::operator=(
    GzipMemberCompressor&& other) noexcept {
    if (this != &other) {
        if (c_) libdeflate_free_compressor(c_);
        c_ = other.c_;
        other.c_ = nullptr;
    }
    return *this;
}

std::size_t GzipMemberCompressor::bound(std::size_t len) const {
    return libdeflate_gzip_compress_bound(c_, len);
}

std::optional<std::size_t> GzipMemberCompressor::compress(
    const void* data, std::size_t len, void* out, std::size_t out_cap) const {
    if (!c_) return std::nullopt;
    const std::size_t n = libdeflate_gzip_compress(c_, data, len, out, out_cap);
    if (n == 0) return std::nullopt;  // did not fit
    return n;
}

bool GzipMemberCompressor::compress_member_into(std::vector<std::uint8_t>& out,
                                                const void* data,
                                                std::size_t len) const {
    if (!c_) return false;
    out.resize(bound(len));
    auto n = compress(data, len, out.data(), out.size());
    if (!n) return false;
    out.resize(*n);
    return true;
}

std::optional<std::vector<std::uint8_t>> GzipMemberCompressor::compress_member(
    const void* data, std::size_t len) const {
    std::vector<std::uint8_t> out;
    if (!compress_member_into(out, data, len)) return std::nullopt;
    return out;
}

std::optional<std::size_t> decode_truncated_member(
    const void* comp, std::size_t comp_len, std::vector<std::uint8_t>& out) {
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return std::nullopt;
    const auto* in = static_cast<const Bytef*>(comp);
    std::size_t in_left = comp_len;
    std::size_t produced = 0;
    if (out.size() < (1u << 20)) out.resize(1u << 20);
    bool complete = false;
    bool ok = true;
    while (true) {
        if (zs.avail_in == 0 && in_left > 0) {
            const std::size_t take = std::min<std::size_t>(in_left, UINT_MAX);
            zs.next_in = const_cast<Bytef*>(in);
            zs.avail_in = static_cast<uInt>(take);
            in += take;
            in_left -= take;
        }
        if (produced == out.size()) out.resize(out.size() * 2);
        const std::size_t room =
            std::min<std::size_t>(out.size() - produced, UINT_MAX);
        zs.next_out = out.data() + produced;
        zs.avail_out = static_cast<uInt>(room);
        const int rc = inflate(&zs, Z_NO_FLUSH);
        produced += room - zs.avail_out;
        if (rc == Z_STREAM_END) {
            complete = true;
            break;
        }
        if (rc == Z_OK) continue;
        if (rc == Z_BUF_ERROR) {
            if (zs.avail_out == 0) continue;
            if (zs.avail_in == 0 && in_left == 0) break;  // ran out: cut member
        }
        ok = false;  // Z_DATA_ERROR, Z_NEED_DICT, Z_MEM_ERROR
        break;
    }
    inflateEnd(&zs);
    if (!ok) return std::nullopt;
    if (complete) return produced;
    std::size_t keep = produced;
    while (keep > 0 && out[keep - 1] != '\n') --keep;
    return keep;
}

}  // namespace dftracer::utils::utilities::fileio::compress
