#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_ZNG_STREAM_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_ZNG_STREAM_H

#include <zlib-ng.h>

#include <cstdint>

namespace dftracer::utils::utilities::fileio::compress {

/// Opens `zs` for inflate with zng_inflateInit2. A positive `window_bits` (zlib
/// or gzip format) makes zlib-ng verify the checksum itself, and the open is
/// counted in zlib_checksum_streams_opened(); a raw stream is not. The first
/// call also runs zlib-ng's one-time CPU-feature table init, so it is done
/// before any concurrent use.
std::int32_t open_inflate_stream(zng_stream& zs, int window_bits);

}  // namespace dftracer::utils::utilities::fileio::compress

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_ZNG_STREAM_H
