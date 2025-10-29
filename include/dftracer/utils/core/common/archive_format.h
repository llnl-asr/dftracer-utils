#ifndef DFTRACER_UTILS_CORE_COMMON_ARCHIVE_FORMAT_H
#define DFTRACER_UTILS_CORE_COMMON_ARCHIVE_FORMAT_H

namespace dftracer::utils {

/**
 * Enumeration of supported archive formats
 */
enum class ArchiveFormat {
    GZIP,    // Standard GZIP file
    TAR_GZ,  // TAR.GZ archive (tar files compressed with gzip)
    UNKNOWN  // Unrecognized or unsupported format
};

inline const char* get_format_name(ArchiveFormat format) {
    switch (format) {
        case ArchiveFormat::GZIP:
            return "GZIP";
        case ArchiveFormat::TAR_GZ:
            return "TAR.GZ";
        case ArchiveFormat::UNKNOWN:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}
}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_ARCHIVE_FORMAT_H
