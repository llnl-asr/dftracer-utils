// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_CONSTANTS_H
#define DFTRACER_UTILS_COMMON_CONSTANTS_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

namespace dftracer::utils::common::constants {

/// Finished tasks a runtime keeps for progress.
inline constexpr std::size_t TASK_HISTORY_LIMIT = 1024;
/// Directory that holds every spill file; when unset or empty, a writable
/// node-local disk mount, else the system temp directory (spill_dir()).
inline constexpr const char* SPILL_DIR_ENV = "DFTRACER_UTILS_SPILL_DIR";
/// "off" or "0" stores spilled morsels raw, "on", "1" or "zstd" compresses each
/// one with zstd level 1 (stored raw when that does not shrink it), and unset,
/// "auto" or any other value compresses only when the spill file is on a
/// network file system, where the bytes written cost more than the
/// compression.
inline constexpr const char* SPILL_COMPRESS_ENV =
    "DFTRACER_UTILS_SPILL_COMPRESS";

/// Repeated in memory/detail/constants.h. A test keeps the two equal. The
/// backend, decay, debug and exit check are read by the memory library before
/// its first allocation. Byte values take a number or a unit such as "512MB".
namespace memory_settings {
inline constexpr const char* MEMORY_BACKEND_ENV =
    "DFTRACER_UTILS_MEMORY_BACKEND";
inline constexpr const char* DECAY_MS_ENV = "DFTRACER_UTILS_MEM_DECAY_MS";
inline constexpr std::uint32_t DEFAULT_DECAY_MS = 1000;
inline constexpr const char* BUDGET_ENV = "DFTRACER_UTILS_MEM_BUDGET";
inline constexpr std::uint64_t DEFAULT_BUDGET_BYTES = 0;
inline constexpr const char* HARD_LIMIT_ENV = "DFTRACER_UTILS_MEM_HARD_LIMIT";
inline constexpr std::uint64_t DEFAULT_HARD_LIMIT_BYTES = 0;
inline constexpr const char* LIMIT_HEADROOM_ENV =
    "DFTRACER_UTILS_MEM_LIMIT_HEADROOM";
inline constexpr std::uint64_t LIMIT_HEADROOM_PERCENT = 5;
inline constexpr std::uint64_t LIMIT_HEADROOM_MIN_BYTES = std::uint64_t{256}
                                                          << 20;
inline constexpr const char* SPILL_HEADROOM_ENV =
    "DFTRACER_UTILS_MEM_SPILL_HEADROOM";
inline constexpr std::uint64_t SPILL_HEADROOM_PERCENT = 2;
inline constexpr std::uint64_t SPILL_HEADROOM_MIN_BYTES = std::uint64_t{32}
                                                          << 20;
inline constexpr const char* DEBUG_ENV = "DFTRACER_UTILS_MEMORY_DEBUG";
inline constexpr bool DEFAULT_DEBUG = false;
inline constexpr const char* EXIT_CHECK_ENV =
    "DFTRACER_UTILS_MEMORY_EXIT_CHECK";
inline constexpr bool DEFAULT_EXIT_CHECK = false;
inline constexpr const char* REPORT_ENV = "DFTRACER_UTILS_MEMORY_REPORT";
inline constexpr bool DEFAULT_REPORT = false;
}  // namespace memory_settings

namespace io_settings {
/// auto, io_uring, epoll, kqueue or threadpool.
inline constexpr const char* BACKEND_ENV = "DFTRACER_UTILS_IO_BACKEND";
}  // namespace io_settings

namespace executor_settings {
/// Seconds without progress before the executor reports a stall, for a tool
/// that runs one long task. The executor's own default is 10.
inline constexpr std::uint32_t TOOL_DEADLOCK_TIMEOUT_SECONDS = 600;
}  // namespace executor_settings

namespace store {
/// The raw bytes of a data extent of a new index store file. A Lustre file
/// takes its largest stripe size, capped at STORE_MAX_EXTENT_BYTES, and
/// anything else STORE_DEFAULT_EXTENT_BYTES. STORE_EXTENT_BYTES_ENV overrides
/// both and must be a multiple of 4096 between STORE_MIN_EXTENT_BYTES and
/// STORE_MAX_EXTENT_BYTES.
inline constexpr const char* STORE_EXTENT_BYTES_ENV =
    "DFTRACER_UTILS_STORE_EXTENT_BYTES";
inline constexpr std::uint32_t STORE_DEFAULT_EXTENT_BYTES = std::uint32_t{1}
                                                            << 20;
inline constexpr std::uint32_t STORE_MIN_EXTENT_BYTES = 4096;
inline constexpr std::uint32_t STORE_MAX_EXTENT_BYTES = std::uint32_t{16} << 20;
/// The most bytes of decoded pages one store snapshot keeps for its queries.
/// STORE_PAGE_CACHE_BYTES_ENV overrides it with a plain number of bytes.
inline constexpr const char* STORE_PAGE_CACHE_BYTES_ENV =
    "DFTRACER_UTILS_STORE_PAGE_CACHE_BYTES";
inline constexpr std::size_t STORE_DEFAULT_PAGE_CACHE_BYTES = std::size_t{64}
                                                              << 20;
}  // namespace store

/// What a file in flight and each piece parsed at once reserve from the memory
/// budget, in bytes held per byte of plaintext. Set from the manager's peak on
/// traces of 1 to 30 MiB compressed.
namespace file_admission {
/// The intern, folds, driver and queues of a file.
inline constexpr std::uint64_t FIXED_BYTES = std::uint64_t{2} << 20;
/// A piece holds its plaintext and its decoded events.
inline constexpr std::uint64_t PIECE_BYTES_PER_PLAINTEXT_BYTE = 3;
/// A trace file inflates to about this many times its size.
inline constexpr std::uint64_t PLAINTEXT_PER_COMPRESSED_BYTE = 16;
/// State that grows with the file and can go to disk: ELASTIC_PERCENT of the
/// plaintext, at least MIN_ELASTIC_BYTES and at most 1 / ELASTIC_POOL_DIVISOR
/// of the pool.
inline constexpr std::uint64_t ELASTIC_PERCENT = 150;
inline constexpr std::uint64_t MIN_ELASTIC_BYTES = std::uint64_t{4} << 20;
inline constexpr std::uint64_t ELASTIC_POOL_DIVISOR = 4;
/// Share of the plaintext the aggregation tier holds per file until it is
/// written.
inline constexpr std::uint64_t AGG_PERCENT = 8;
/// The sinks of the workers take at most 1 / SINK_BUDGET_DIVISOR of a budget,
/// each at least MIN_SINK_BYTES; a smaller budget runs fewer workers.
inline constexpr std::uint64_t SINK_BUDGET_DIVISOR = 2;
inline constexpr std::uint64_t MIN_SINK_BYTES = std::uint64_t{8} << 20;
/// 1 / UNTRACKED_DIVISOR of a budget is left for the runtime, the index
/// database and memory the allocator keeps after it is freed.
inline constexpr std::uint64_t UNTRACKED_DIVISOR = 8;
/// A file holds at most all but 1 / PIECE_ROOM_DIVISOR of the budget, so the
/// pieces of a larger file still find room.
inline constexpr std::uint64_t PIECE_ROOM_DIVISOR = 4;
/// The smallest spill run. A run doubles every SPILL_RUNS_PER_DOUBLING runs, up
/// to 2^MAX_SPILL_DOUBLINGS times that, so a tier of up to MAX_SPILL_TIER_BYTES
/// stays within MAX_SPILL_RUNS.
inline constexpr std::uint64_t MIN_SPILL_RUN_BYTES = std::uint64_t{4} << 20;
inline constexpr std::size_t SPILL_RUNS_PER_DOUBLING = 16;
inline constexpr std::size_t MAX_SPILL_DOUBLINGS = 6;
inline constexpr std::uint64_t MAX_SPILL_TIER_BYTES = std::uint64_t{4} << 30;
inline constexpr std::size_t MAX_SPILL_RUNS = 128;
/// Parse workers that one inflate worker feeds.
inline constexpr std::size_t INFLATE_PARSERS_PER_WORKER = 4;
/// A file parses pieces at once only while at least 1 / SPAWN_FREE_DIVISOR of
/// the workers have no file.
inline constexpr std::size_t SPAWN_FREE_DIVISOR = 4;
}  // namespace file_admission

namespace indexer {
/// How often, at most, a process that holds a store index checks whether
/// other processes changed it, in milliseconds. 0 checks on every read.
inline constexpr const char* INDEX_STORE_REFRESH_MS_ENV =
    "DFTRACER_UTILS_INDEX_STORE_REFRESH_MS";
inline constexpr std::uint64_t INDEX_STORE_REFRESH_MS = 1000;
static constexpr std::size_t ZLIB_WINDOW_SIZE = 32768;
static constexpr int ZLIB_GZIP_WINDOW_BITS = 31;    ///< 15 + 16 for gzip format
static constexpr int ZLIB_FORMAT_WINDOW_BITS = 15;  ///< zlib format
static constexpr int ZLIB_RAW_WINDOW_BITS = -15;    ///< raw deflate, no header
/// GZIP member magic bytes.
static constexpr unsigned char GZIP_MAGIC_BYTE_0 = 0x1f;
static constexpr unsigned char GZIP_MAGIC_BYTE_1 = 0x8b;
#if defined(__APPLE__)
static constexpr std::size_t INFLATE_BUFFER_SIZE =
    131072;  ///< 128KB (macOS 512KB thread stack)
#else
static constexpr std::size_t INFLATE_BUFFER_SIZE = 262144;  ///< 256KB
#endif
static constexpr std::uint64_t DEFAULT_CHECKPOINT_SIZE =
    32 * 1024 * 1024;  ///< 32MB
inline constexpr const char* EXTENSION = ".dftindex";
}  // namespace indexer

namespace reader {
static constexpr std::size_t DEFAULT_BUFFER_SIZE = 65536;  ///< 64KB
static constexpr std::size_t SKIP_BUFFER_SIZE = 131072;    ///< 128KB
static constexpr std::size_t FILE_IO_BUFFER_SIZE =
    262144;  ///< 256KB for file I/O
/// The most nested containers an on-demand walk descends into. A container
/// below it stays under its path as its JSON text. The walks cost up to about
/// 450 bytes of stack a level, and several times more under a sanitizer, while
/// a worker thread has 512 KiB on macOS, so the limit stays well under the
/// 1024 levels the DOM parser accepts.
static constexpr std::size_t MAX_READ_DEPTH = 128;
}  // namespace reader

}  // namespace dftracer::utils::common::constants

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

#endif  // DFTRACER_UTILS_COMMON_CONSTANTS_H
