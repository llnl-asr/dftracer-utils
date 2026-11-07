// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_ARCHIVE_FORMAT_H
#define DFTRACER_UTILS_COMMON_ARCHIVE_FORMAT_H

namespace dftracer::utils::common {

/**
 * Enumeration of supported archive formats
 */
enum class ArchiveFormat {
    Gzip,    ///< Standard GZIP file
    Unknown  ///< Unrecognized or unsupported format
};

inline const char* get_format_name(ArchiveFormat format) {
    switch (format) {
        case ArchiveFormat::Gzip:
            return "GZIP";
        case ArchiveFormat::Unknown:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}
}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_ARCHIVE_FORMAT_H
