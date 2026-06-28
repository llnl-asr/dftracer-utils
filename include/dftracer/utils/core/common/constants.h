#ifndef DFTRACER_UTILS_CORE_COMMON_CONSTANTS_H
#define DFTRACER_UTILS_CORE_COMMON_CONSTANTS_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

namespace dftracer::utils::constants {
namespace indexer {
static constexpr std::size_t ZLIB_WINDOW_SIZE = 32768;
static constexpr int ZLIB_GZIP_WINDOW_BITS = 31;    // 15 + 16 for gzip format
static constexpr int ZLIB_FORMAT_WINDOW_BITS = 15;  // zlib format
static constexpr int ZLIB_RAW_WINDOW_BITS = -15;    // raw deflate, no header
// GZIP member magic bytes.
static constexpr unsigned char GZIP_MAGIC_BYTE_0 = 0x1f;
static constexpr unsigned char GZIP_MAGIC_BYTE_1 = 0x8b;
#if defined(__APPLE__)
static constexpr std::size_t INFLATE_BUFFER_SIZE =
    131072;  // 128KB (macOS 512KB thread stack)
#else
static constexpr std::size_t INFLATE_BUFFER_SIZE = 262144;  // 256KB
#endif
static constexpr std::uint64_t DEFAULT_CHECKPOINT_SIZE =
    32 * 1024 * 1024;  // 32MB
inline const char* EXTENSION = ".dftindex";
}  // namespace indexer

namespace reader {
static constexpr std::size_t DEFAULT_BUFFER_SIZE = 65536;  // 64KB
static constexpr std::size_t SKIP_BUFFER_SIZE = 131072;    // 128KB
static constexpr std::size_t FILE_IO_BUFFER_SIZE =
    262144;                                                // 256KB for file I/O
}  // namespace reader

namespace rocksdb {
// ZSTD dictionary training tuning shared by every DB-open path.
static constexpr int ZSTD_COMPRESSION_LEVEL = 9;
static constexpr std::size_t ZSTD_MAX_DICT_BYTES = 262144;        // 256KB
static constexpr std::size_t ZSTD_MAX_TRAIN_BYTES = 1024 * 1024;  // 1MB
}  // namespace rocksdb
}  // namespace dftracer::utils::constants

#else  // C

#include <stddef.h>
#include <stdint.h>

#define DFTRACER_UTILS_ZLIB_WINDOW_SIZE 32768
#define DFTRACER_UTILS_ZLIB_GZIP_WINDOW_BITS 31
#define DFTRACER_UTILS_DEFAULT_CHECKPOINT_SIZE (32 * 1024 * 1024)
#define DFTRACER_UTILS_DEFAULT_BUFFER_SIZE 65536
#define DFTRACER_UTILS_SKIP_BUFFER_SIZE 131072
#define DFTRACER_UTILS_FILE_IO_BUFFER_SIZE 262144
#define DFTRACER_UTILS_INDEX_EXTENSION ".dftindex"

#endif  // __cplusplus

#endif  // DFTRACER_UTILS_CORE_COMMON_CONSTANTS_H
