// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_FILEIO_JSON_LINE_FORMAT_H
#define DFTRACER_UTILS_FILEIO_JSON_LINE_FORMAT_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/fileio/line_format.h>
#include <dftracer/utils/json/fmt.h>

namespace dftracer::utils::fileio::line_format {

using json::JsonEscape;

using JsonLineFormat = BasicLineFormat<JsonEscape>;

}  // namespace dftracer::utils::fileio::line_format

#endif  // DFTRACER_UTILS_FILEIO_JSON_LINE_FORMAT_H
