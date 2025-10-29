#ifndef DFTRACER_UTILS_CORE_COMMON_CONSTANTS_H
#define DFTRACER_UTILS_CORE_COMMON_CONSTANTS_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace dftracer::utils::constants {
namespace indexer {
static constexpr std::size_t ZLIB_WINDOW_SIZE = 32768;
static constexpr int ZLIB_GZIP_WINDOW_BITS = 31;  // 15 + 16 for gzip format
static constexpr std::uint64_t DEFAULT_CHECKPOINT_SIZE =
    32 * 1024 * 1024;                             // 32MB
extern const char* const& SQL_SCHEMA;
inline const char* EXTENSION = ".idx";
}  // namespace indexer

namespace reader {
static constexpr std::size_t DEFAULT_BUFFER_SIZE = 65536;  // 64KB
static constexpr std::size_t SKIP_BUFFER_SIZE = 131072;    // 128KB
static constexpr std::size_t FILE_IO_BUFFER_SIZE =
    262144;                                                // 256KB for file I/O
}  // namespace reader
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
#define DFTRACER_UTILS_INDEX_EXTENSION ".idx"

extern const char *DFTRACER_UTILS_SQL_SCHEMA;

#endif  // __cplusplus

#endif  // DFTRACER_UTILS_CORE_COMMON_CONSTANTS_H
