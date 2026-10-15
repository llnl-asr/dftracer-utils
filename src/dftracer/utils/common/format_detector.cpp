#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/format_detector.h>
#include <dftracer/utils/common/logging.h>

namespace dftracer::utils {

ArchiveFormat FormatDetector::detect(const std::string& file_path) {
    if (file_path.size() >= 3 &&
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
    return has_gzip_magic(file) ? ArchiveFormat::GZIP : ArchiveFormat::UNKNOWN;
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

    return magic[0] == constants::indexer::GZIP_MAGIC_BYTE_0 &&
           magic[1] == constants::indexer::GZIP_MAGIC_BYTE_1;
}

}  // namespace dftracer::utils
