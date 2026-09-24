#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_LIBDEFLATE_GZIP_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_LIBDEFLATE_GZIP_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

/// Opaque handles keep <libdeflate.h> out of this header.
struct libdeflate_decompressor;
struct libdeflate_compressor;

namespace dftracer::utils::utilities::fileio::compress {

// One-shot (whole-buffer) gzip codec over libdeflate. libdeflate has no
// streaming API; a single gzip member is the unit of work.

struct DecompressResult {
    std::size_t out_bytes;
    std::size_t in_bytes;  ///< input consumed (this member's compressed length)
};

/// BadData is returned both for corrupt input and for a member not yet fully
/// buffered, so streaming callers should read more and retry before giving up.
enum class GzipDecode : std::uint8_t {
    Ok,
    BadData,
    ShortOutput,
    InsufficientSpace,
};

/// Reusable, not thread-safe (one per thread), move-only.
class GzipMemberDecompressor {
   public:
    GzipMemberDecompressor();
    ~GzipMemberDecompressor();
    GzipMemberDecompressor(const GzipMemberDecompressor&) = delete;
    GzipMemberDecompressor& operator=(const GzipMemberDecompressor&) = delete;
    GzipMemberDecompressor(GzipMemberDecompressor&& other) noexcept;
    GzipMemberDecompressor& operator=(GzipMemberDecompressor&& other) noexcept;

    /// out_cap must be >= the member's uncompressed size. nullopt on failure.
    std::optional<DecompressResult> decompress(const void* comp,
                                               std::size_t comp_len, void* out,
                                               std::size_t out_cap) const;

    /// Reports the exact status so streaming callers can tell InsufficientSpace
    /// from BadData. `result` is filled on Ok.
    GzipDecode decompress_status(const void* comp, std::size_t comp_len,
                                 void* out, std::size_t out_cap,
                                 DecompressResult& result) const;

    std::optional<std::vector<std::uint8_t>> decompress_member(
        const void* comp, std::size_t comp_len,
        std::size_t uncompressed_size) const;

    bool valid() const { return d_ != nullptr; }

   private:
    libdeflate_decompressor* d_;
};

/// Decode a gzip member that ends at end of file before its trailer (a trace
/// cut while being written), which the libdeflate decoder cannot do. `comp`
/// holds every byte from the member start to end of file; `out` is resized as
/// needed. Returns the number of leading bytes of `out` to keep: all output
/// when the member turns out complete, else the output up to and including its
/// last '\n' (0 when no line is complete). nullopt when the deflate data is
/// invalid rather than short, so corruption is never recovered. The kept bytes
/// are not checksum-verified.
std::optional<std::size_t> decode_truncated_member(
    const void* comp, std::size_t comp_len, std::vector<std::uint8_t>& out);

/// Compresses a whole buffer into one gzip member. Reusable, not thread-safe,
/// move-only.
class GzipMemberCompressor {
   public:
    explicit GzipMemberCompressor(int level = 6);
    ~GzipMemberCompressor();
    GzipMemberCompressor(const GzipMemberCompressor&) = delete;
    GzipMemberCompressor& operator=(const GzipMemberCompressor&) = delete;
    GzipMemberCompressor(GzipMemberCompressor&& other) noexcept;
    GzipMemberCompressor& operator=(GzipMemberCompressor&& other) noexcept;

    /// Worst-case compressed size, for sizing `out`.
    std::size_t bound(std::size_t len) const;

    /// out_cap must be >= bound(len). nullopt if it did not fit.
    std::optional<std::size_t> compress(const void* data, std::size_t len,
                                        void* out, std::size_t out_cap) const;

    /// Compress one gzip member into `out`, resized to the exact member length.
    /// `out`'s capacity is reused across calls, so a caller that keeps one
    /// scratch buffer pays no per-member heap allocation once it is warm.
    /// Returns false on failure (leaving `out` in an unspecified state). Prefer
    /// this over compress_member() on hot paths and over the manual
    /// bound()+resize()+ compress() dance everywhere.
    bool compress_member_into(std::vector<std::uint8_t>& out, const void* data,
                              std::size_t len) const;

    /// One-shot convenience: allocates a fresh buffer per call. Prefer
    /// compress_member_into() when compressing repeatedly.
    std::optional<std::vector<std::uint8_t>> compress_member(
        const void* data, std::size_t len) const;

    bool valid() const { return c_ != nullptr; }

   private:
    libdeflate_compressor* c_;
};

}  // namespace dftracer::utils::utilities::fileio::compress

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_LIBDEFLATE_GZIP_H
