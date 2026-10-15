#ifndef DFTRACER_UTILS_COMMON_ARCHIVE_FORMAT_H
#define DFTRACER_UTILS_COMMON_ARCHIVE_FORMAT_H

namespace dftracer::utils {

/**
 * Enumeration of supported archive formats
 */
enum class ArchiveFormat {
    GZIP,    ///< Standard GZIP file
    UNKNOWN  ///< Unrecognized or unsupported format
};

inline const char* get_format_name(ArchiveFormat format) {
    switch (format) {
        case ArchiveFormat::GZIP:
            return "GZIP";
        case ArchiveFormat::UNKNOWN:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}
}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_COMMON_ARCHIVE_FORMAT_H
