#include "tar_parser.h"

#include <algorithm>
#include <cstring>

namespace dftracer::utils::utilities::indexer::internal::tar {

bool TarParser::parse_headers(const unsigned char* data, std::size_t data_size,
                              std::uint64_t stream_offset,
                              std::vector<TarFileEntry>& entries) {
    if (data_size < 512) {
        return false;  // Need at least one header block
    }

    std::size_t offset = 0;

    while (offset + 512 <= data_size) {
        const auto* header = reinterpret_cast<const TarHeader*>(data + offset);

        // Check for end of archive (two consecutive zero blocks)
        if (is_end_of_archive(data, offset, data_size)) {
            break;
        }

        // Skip if this doesn't look like a valid header
        if (!is_tar_header(data + offset, data_size - offset)) {
            offset += 512;
            continue;
        }

        TarFileEntry entry;
        if (parse_single_header(header, stream_offset + offset, entry)) {
            entries.push_back(entry);

            // Skip to next header (current header + file data, rounded to 512
            // bytes)
            offset += 512;                          // Header size
            offset += get_padded_size(entry.size);  // File data size (padded)
        } else {
            offset += 512;                          // Skip invalid header
        }
    }

    return !entries.empty();
}

bool TarParser::is_tar_header(const unsigned char* data,
                              std::size_t data_size) {
    if (data_size < 512) {
        return false;
    }

    const auto* header = reinterpret_cast<const TarHeader*>(data);

    // Check magic number for POSIX tar
    if (std::strncmp(header->magic, "ustar", 5) == 0) {
        return TarParser::verify_checksum(header);
    }

    // Check for old tar format (no magic, but has valid checksum and reasonable
    // values)
    if (header->name[0] != '\0' && TarParser::verify_checksum(header)) {
        // Additional sanity checks for old format
        std::uint64_t size =
            TarParser::parse_octal(header->size, sizeof(header->size));
        if (size <= (1ULL << 33)) {  // Reasonable file size limit (8GB)
            return true;
        }
    }

    return false;
}

bool TarParser::parse_single_header(const TarHeader* header,
                                    std::uint64_t header_offset,
                                    TarFileEntry& entry) {
    if (!TarParser::verify_checksum(header)) {
        DFTRACER_UTILS_LOG_DEBUG(
            "TAR header checksum verification failed at offset %llu",
            header_offset);
        return false;
    }

    entry.name = extract_filename(header);
    entry.size = TarParser::parse_octal(header->size, sizeof(header->size));
    entry.mtime = TarParser::parse_octal(header->mtime, sizeof(header->mtime));
    entry.typeflag = header->typeflag;
    entry.data_offset = header_offset + 512;  // Data starts after header
    entry.uncompressed_offset = header_offset;

    return true;
}

bool TarParser::verify_checksum(const TarHeader* header) {
    // Calculate checksum by summing all bytes in header,
    // treating checksum field as spaces
    std::uint64_t calculated_sum = 0;
    const auto* bytes = reinterpret_cast<const unsigned char*>(header);

    // Sum bytes before checksum field
    for (std::size_t i = 0; i < offsetof(TarHeader, checksum); i++) {
        calculated_sum += bytes[i];
    }

    // Add 8 spaces for the checksum field
    calculated_sum += 8 * ' ';

    // Sum bytes after checksum field
    for (std::size_t i =
             offsetof(TarHeader, checksum) + sizeof(header->checksum);
         i < 512; i++) {
        calculated_sum += bytes[i];
    }

    // Parse stored checksum
    std::uint64_t stored_checksum =
        TarParser::parse_octal(header->checksum, sizeof(header->checksum));

    return calculated_sum == stored_checksum;
}

std::uint64_t TarParser::parse_octal(const char* str, std::size_t len) {
    std::uint64_t result = 0;

    for (std::size_t i = 0; i < len && str[i] != '\0' && str[i] != ' '; i++) {
        if (str[i] >= '0' && str[i] <= '7') {
            result = result * 8 + (str[i] - '0');
        } else {
            break;  // Invalid octal digit
        }
    }

    return result;
}

std::string TarParser::extract_filename(const TarHeader* header) {
    std::string filename;

    // Check if prefix is used (POSIX tar format)
    if (header->prefix[0] != '\0') {
        // Use prefix + "/" + name
        std::size_t prefix_len =
            std::min(sizeof(header->prefix), std::strlen(header->prefix));
        filename.assign(header->prefix, prefix_len);

        if (!filename.empty() && filename.back() != '/') {
            filename += '/';
        }
    }

    // Append main name
    std::size_t name_len =
        std::min(sizeof(header->name), std::strlen(header->name));
    filename.append(header->name, name_len);

    return filename;
}

bool TarParser::is_end_of_archive(const unsigned char* data, std::size_t offset,
                                  std::size_t data_size) {
    // Need at least two 512-byte blocks for end-of-archive marker
    if (offset + 1024 > data_size) {
        return false;
    }

    // Check if both blocks are all zeros
    const unsigned char* block1 = data + offset;
    const unsigned char* block2 = data + offset + 512;

    for (std::size_t i = 0; i < 512; i++) {
        if (block1[i] != 0 || block2[i] != 0) {
            return false;
        }
    }

    return true;
}

}  // namespace dftracer::utils::utilities::indexer::internal::tar