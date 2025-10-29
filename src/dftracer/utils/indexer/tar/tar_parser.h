#ifndef DFTRACER_UTILS_INDEXER_TAR_PARSER_H
#define DFTRACER_UTILS_INDEXER_TAR_PARSER_H

#include <dftracer/utils/core/common/logging.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dftracer::utils::tar_parser {

/**
 * TAR header structure (POSIX tar format)
 */
struct TarHeader {
    char name[100];      // File name
    char mode[8];        // File mode (octal)
    char uid[8];         // Owner user ID (octal)
    char gid[8];         // Owner group ID (octal)
    char size[12];       // File size (octal)
    char mtime[12];      // Modification time (octal)
    char checksum[8];    // Header checksum (octal)
    char typeflag;       // File type
    char linkname[100];  // Link name (for links)
    char magic[6];       // "ustar\0"
    char version[2];     // "00"
    char uname[32];      // Owner user name
    char gname[32];      // Owner group name
    char devmajor[8];    // Device major number (octal)
    char devminor[8];    // Device minor number (octal)
    char prefix[155];    // Path prefix
    char padding[12];    // Padding to 512 bytes
};

static_assert(sizeof(TarHeader) == 512, "TAR header must be 512 bytes");

/**
 * TAR file entry information
 */
struct TarFileEntry {
    std::string name;           // Full file name (prefix + name)
    std::uint64_t size;         // File size in bytes
    std::uint64_t data_offset;  // Offset to file data in archive
    std::uint64_t mtime;        // Modification time (Unix timestamp)
    char typeflag;              // File type flag

    // For tar.gz indexing
    std::uint64_t uncompressed_offset;  // Offset in uncompressed stream

    bool is_regular_file() const { return typeflag == '0' || typeflag == '\0'; }

    bool is_directory() const { return typeflag == '5'; }
};

/**
 * TAR archive parser for extracting file metadata from TAR streams
 */
class TarParser {
   public:
    TarParser() = default;
    ~TarParser() = default;

    /**
     * Parse TAR headers from a data buffer.
     * Returns false if the data doesn't contain valid TAR headers.
     */
    bool parse_headers(const unsigned char* data, std::size_t data_size,
                       std::uint64_t stream_offset,
                       std::vector<TarFileEntry>& entries);

    /**
     * Check if data starts with a valid TAR header
     */
    static bool is_tar_header(const unsigned char* data, std::size_t data_size);

    /**
     * Get the expected size of file data (rounded up to 512-byte blocks)
     */
    static std::uint64_t get_padded_size(std::uint64_t size) {
        return ((size + 511) / 512) * 512;
    }

   private:
    /**
     * Parse a single TAR header from the data buffer
     */
    bool parse_single_header(const TarHeader* header,
                             std::uint64_t header_offset, TarFileEntry& entry);

    /**
     * Verify TAR header checksum
     */
    static bool verify_checksum(const TarHeader* header);

    /**
     * Convert octal string to integer
     */
    static std::uint64_t parse_octal(const char* str, std::size_t len);

    /**
     * Extract full file name (handle prefix + name)
     */
    std::string extract_filename(const TarHeader* header);

    /**
     * Check if we've reached the end of the TAR archive (two zero blocks)
     */
    bool is_end_of_archive(const unsigned char* data, std::size_t offset,
                           std::size_t data_size);
};

}  // namespace dftracer::utils::tar_parser

#endif  // DFTRACER_UTILS_INDEXER_TAR_PARSER_H
