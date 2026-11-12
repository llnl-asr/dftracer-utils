// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_FORMAT_DETECTOR_H
#define DFTRACER_UTILS_COMMON_FORMAT_DETECTOR_H

#include <dftracer/utils/common/archive_format.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdio>

namespace dftracer::utils::common {
class FormatDetector {
   public:
    static ArchiveFormat detect(dstd::StringView file_path);
    static ArchiveFormat detect_from_content(FILE* file);
    static bool is_gzip(FILE* file);

   private:
    static bool has_gzip_magic(FILE* file);
};
}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_FORMAT_DETECTOR_H
