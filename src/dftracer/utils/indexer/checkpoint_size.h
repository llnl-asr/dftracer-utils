#ifndef __DFTRACER_UTILS_INDEXER_CHECKPOINT_SIZE_H
#define __DFTRACER_UTILS_INDEXER_CHECKPOINT_SIZE_H

#include <dftracer/utils/core/common/constants.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace dftracer::utils {

std::size_t determine_checkpoint_size(
    std::size_t user_checkpoint_size, const std::string& path,
    // Tunables:
    std::size_t max_chk = (512u << 20), std::size_t max_parts = 100000000,
    // default:
    std::size_t window = constants::indexer::ZLIB_WINDOW_SIZE);

}  // namespace dftracer::utils

#endif  // __DFTRACER_UTILS_INDEXER_CHECKPOINT_SIZE_H
