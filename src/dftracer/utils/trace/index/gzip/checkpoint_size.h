// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_GZIP_CHECKPOINT_SIZE_H
#define DFTRACER_UTILS_TRACE_INDEX_GZIP_CHECKPOINT_SIZE_H

#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::trace::index::gzip {

std::size_t determine_checkpoint_size(
    std::size_t user_checkpoint_size, dstd::CStringView path,
    // Tunables:
    std::size_t max_parts = 100000000, std::size_t max_chk = (512u << 20),
    // default:
    std::size_t window = common::constants::indexer::ZLIB_WINDOW_SIZE);

}  // namespace dftracer::utils::trace::index::gzip

#endif  // DFTRACER_UTILS_TRACE_INDEX_GZIP_CHECKPOINT_SIZE_H
