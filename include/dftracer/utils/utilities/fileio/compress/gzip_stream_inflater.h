#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_STREAM_INFLATER_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_STREAM_INFLATER_H

#include <cstddef>
#include <cstdint>
#include <memory>

namespace dftracer::utils::utilities::fileio::compress {

enum class InflateFlush : std::uint8_t {
    None,
    Block,  ///< stop at each deflate block boundary
};

enum class InflateStatus : std::uint8_t {
    Ok,
    BufError,   ///< no progress possible
    StreamEnd,  ///< the member ended and its trailer was verified
    DataError,  ///< bad header, data, CRC or size
    Error,      ///< any other failure
};

/// Streaming inflate of one gzip member, with the CRC32 computed by libdeflate
/// instead of zlib. zlib-ng inflates the raw deflate data; this class parses
/// the header (including the FHCRC field), checksums the output and checks the
/// trailer CRC32 and ISIZE. inflate() reports a bad header, CRC or size as
/// DataError and returns StreamEnd only once the trailer has been verified.
///
/// A negative `window_bits` opens a raw deflate stream (a piece read from a
/// restart point), which carries no CRC.
///
/// Set the input and output with set_input() and set_output() as for zlib.
/// Not thread-safe; one per stream.
class GzipStreamInflater {
   public:
    GzipStreamInflater();
    GzipStreamInflater(const GzipStreamInflater&) = delete;
    GzipStreamInflater& operator=(const GzipStreamInflater&) = delete;
    ~GzipStreamInflater();

    /// 15 + 16 (or 31) for a gzip member, a negative value for raw deflate.
    bool init(int window_bits);

    void end();

    /// Start the next member of a concatenated stream. The input and output
    /// pointers are kept.
    bool reset();

    /// At most 2^32 - 1 bytes.
    void set_input(const unsigned char* data, std::size_t size);
    void set_output(unsigned char* data, std::size_t size);
    std::size_t avail_in() const;
    std::size_t avail_out() const;

    /// Compressed bytes consumed so far: header, deflate data and trailer.
    std::uint64_t total_in() const;

    /// zlib's data_type of the stream: bit 7 is set at a block boundary, bit 6
    /// at the last block, and the low three bits count unused bits of the last
    /// input byte.
    int data_type() const;

    InflateStatus inflate(InflateFlush flush = InflateFlush::None);

    /// Insert `bits` bits of `value` before the next input, for a raw stream
    /// that starts inside a byte.
    bool prime(int bits, int value);

    /// The 32 KiB window of a raw stream that starts at a restart point.
    bool set_dictionary(const unsigned char* data, std::size_t size);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// The streams opened in this process in a mode where zlib-ng verifies the
/// gzip or zlib checksum itself. The decode paths inflate raw deflate and
/// check the CRC32 with libdeflate, so they never raise it.
std::uint64_t zlib_checksum_streams_opened();

}  // namespace dftracer::utils::utilities::fileio::compress

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_STREAM_INFLATER_H
