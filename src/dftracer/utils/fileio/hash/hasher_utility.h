// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_FILEIO_HASH_HASHER_UTILITY_H
#define DFTRACER_UTILS_FILEIO_HASH_HASHER_UTILITY_H

#include <dftracer/utils/fileio/hash/fnv1a_hasher_utility.h>

namespace dftracer::utils::fileio::hash {

/// The project hasher is incremental FNV-1a. This was once a
/// runtime-selectable hierarchy (BaseHasher + a std::hash variant), but only
/// FNV-1a was ever used, so it collapses to the one concrete hasher.
using HasherUtility = Fnv1aHasherUtility;

}  // namespace dftracer::utils::fileio::hash

#endif  // DFTRACER_UTILS_FILEIO_HASH_HASHER_UTILITY_H
