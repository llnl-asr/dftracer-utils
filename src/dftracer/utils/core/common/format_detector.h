#ifndef DFTRACER_UTILS_CORE_COMMON_FORMAT_DETECTOR_H
#define DFTRACER_UTILS_CORE_COMMON_FORMAT_DETECTOR_H

#include <dftracer/utils/core/common/archive_format.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace dftracer::utils {
class FormatDetector {
   public:
    static ArchiveFormat detect(const std::string& file_path);
    static ArchiveFormat detect_from_content(FILE* file);
    static bool is_tar_gz(FILE* file);
    static bool is_gzip(FILE* file);

   private:
    static bool has_gzip_magic(FILE* file);
    static bool has_tar_header_after_gzip(FILE* file);
    static bool is_valid_tar_header(const unsigned char* header);
    static unsigned int calculate_tar_checksum(const unsigned char* header);
};
}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_FORMAT_DETECTOR_H
