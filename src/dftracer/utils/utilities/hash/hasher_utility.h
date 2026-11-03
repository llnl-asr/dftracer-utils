// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_HASH_HASHER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_HASH_HASHER_UTILITY_H

#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>

namespace dftracer::utils::utilities::hash {

/// The project hasher is incremental FNV-1a. This was once a
/// runtime-selectable hierarchy (BaseHasher + a std::hash variant), but only
/// FNV-1a was ever used, so it collapses to the one concrete hasher.
using HasherUtility = Fnv1aHasherUtility;

}  // namespace dftracer::utils::utilities::hash

#endif  // DFTRACER_UTILS_UTILITIES_HASH_HASHER_UTILITY_H
