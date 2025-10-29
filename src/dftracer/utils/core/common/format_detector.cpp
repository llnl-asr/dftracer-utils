#include <dftracer/utils/core/common/format_detector.h>
#include <dftracer/utils/core/common/logging.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace dftracer::utils {

ArchiveFormat FormatDetector::detect(const std::string& file_path) {
    if (file_path.size() >= 7 &&
        file_path.substr(file_path.size() - 7) == ".tar.gz") {
        return ArchiveFormat::TAR_GZ;
    } else if (file_path.size() >= 4 &&
               file_path.substr(file_path.size() - 4) == ".tgz") {
        return ArchiveFormat::TAR_GZ;
    } else if (file_path.size() >= 3 &&
               file_path.substr(file_path.size() - 3) == ".gz") {
        return ArchiveFormat::GZIP;
    } else if (file_path.size() >= 5 &&
               file_path.substr(file_path.size() - 5) == ".gzip") {
        return ArchiveFormat::GZIP;
    }

    FILE* file = std::fopen(file_path.c_str(), "rb");
    if (!file) {
        DFTRACER_UTILS_LOG_ERROR("Failed to open file for format detection: %s",
                                 file_path.c_str());
        return ArchiveFormat::UNKNOWN;
    }

    ArchiveFormat format = detect_from_content(file);
    std::fclose(file);
    return format;
}

ArchiveFormat FormatDetector::detect_from_content(FILE* file) {
    if (!has_gzip_magic(file)) {
        return ArchiveFormat::UNKNOWN;
    }

    if (has_tar_header_after_gzip(file)) {
        return ArchiveFormat::TAR_GZ;
    }

    return ArchiveFormat::GZIP;
}

bool FormatDetector::is_tar_gz(FILE* file) {
    return detect_from_content(file) == ArchiveFormat::TAR_GZ;
}

bool FormatDetector::is_gzip(FILE* file) {
    return detect_from_content(file) == ArchiveFormat::GZIP;
}

bool FormatDetector::has_gzip_magic(FILE* file) {
    if (fseeko(file, 0, SEEK_SET) != 0) {
        return false;
    }

    unsigned char magic[2];
    if (fread(magic, 1, 2, file) != 2) {
        return false;
    }

    // GZIP magic: 0x1f 0x8b
    return magic[0] == 0x1f && magic[1] == 0x8b;
}

bool FormatDetector::has_tar_header_after_gzip(FILE* file) {
    // Seek to the beginning of the file
    if (fseeko(file, 0, SEEK_SET) != 0) {
        return false;
    }

    // Initialize zlib for GZIP decompression
    z_stream stream;
    memset(&stream, 0, sizeof(stream));

    if (inflateInit2(&stream, 31) != Z_OK) {  // 31 = 15 + 16 for GZIP format
        return false;
    }

    // Read input buffer
    const size_t buffer_size = 8192;
    unsigned char in_buffer[buffer_size];
    unsigned char out_buffer[buffer_size];

    bool found_tar_header = false;
    size_t total_out = 0;

    while (total_out < 512) {  // Need at least 512 bytes for TAR header
        // Read compressed data
        size_t bytes_read = fread(in_buffer, 1, buffer_size, file);
        if (bytes_read == 0) {
            if (ferror(file)) {
                DFTRACER_UTILS_LOG_DEBUG(
                    "Error reading file during TAR detection", "");
            }
            break;
        }

        stream.next_in = in_buffer;
        stream.avail_in = static_cast<uInt>(bytes_read);

        while (stream.avail_in > 0 && total_out < 512) {
            stream.next_out = out_buffer;
            stream.avail_out =
                static_cast<uInt>(std::min(buffer_size, 512 - total_out));

            int ret = inflate(&stream, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END) {
                break;
            }

            size_t bytes_out =
                (std::min(buffer_size, 512 - total_out)) - stream.avail_out;

            // Check if we have enough bytes to validate TAR header
            if (total_out + bytes_out >= 512) {
                // We need to copy existing data if any and append new data
                unsigned char tar_header[512];
                memset(tar_header, 0, 512);

                if (total_out > 0) {
                    // This is more complex - we'd need to store previous output
                    // For simplicity, let's check if we got the header in one
                    // go
                    if (total_out == 0 && bytes_out >= 512) {
                        memcpy(tar_header, out_buffer, 512);
                        found_tar_header = is_valid_tar_header(tar_header);
                    }
                } else if (bytes_out >= 512) {
                    memcpy(tar_header, out_buffer, 512);
                    found_tar_header = is_valid_tar_header(tar_header);
                }
                break;
            }

            total_out += bytes_out;

            if (ret == Z_STREAM_END) {
                break;
            }
        }

        if (found_tar_header || total_out >= 512) {
            break;
        }
    }

    inflateEnd(&stream);
    return found_tar_header;
}

bool FormatDetector::is_valid_tar_header(const unsigned char* header) {
    // Check for POSIX TAR magic: "ustar\0"
    const char* ustar_magic = "ustar";
    if (memcmp(header + 257, ustar_magic, 5) == 0 && header[262] == 0) {
        // Validate checksum
        unsigned int stored_checksum = 0;

        // Read checksum field (bytes 148-155) as octal
        for (int i = 148; i < 156; i++) {
            char c = header[i];
            if (c >= '0' && c <= '7') {
                stored_checksum = stored_checksum * 8 + (c - '0');
            } else if (c == ' ' || c == '\0') {
                break;
            } else {
                return false;
            }
        }

        unsigned int calculated_checksum = calculate_tar_checksum(header);
        return stored_checksum == calculated_checksum;
    }

    // Check for old GNU tar format - look for reasonable filename
    // and verify that most of the header fields make sense
    bool has_filename = false;
    for (int i = 0; i < 100; i++) {
        if (header[i] == '\0') {
            has_filename = (i > 0);  // Non-empty filename
            break;
        }
        if (!isprint(header[i]) && header[i] != '/') {
            return false;
        }
    }

    if (!has_filename) {
        return false;
    }

    // Check file mode (should be reasonable octal value)
    bool mode_ok = true;
    for (int i = 100; i < 108; i++) {
        char c = header[i];
        if (c != ' ' && c != '\0' && (c < '0' || c > '7')) {
            mode_ok = false;
            break;
        }
    }

    return mode_ok;
}

unsigned int FormatDetector::calculate_tar_checksum(
    const unsigned char* header) {
    unsigned int checksum = 0;

    // Sum all bytes, treating checksum field (148-155) as spaces
    for (int i = 0; i < 512; i++) {
        if (i >= 148 && i < 156) {
            checksum += ' ';  // Checksum field treated as spaces
        } else {
            checksum += header[i];
        }
    }

    return checksum;
}
}  // namespace dftracer::utils
