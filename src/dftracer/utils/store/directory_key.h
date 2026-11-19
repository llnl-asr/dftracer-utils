// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_STORE_DIRECTORY_KEY_H
#define DFTRACER_UTILS_STORE_DIRECTORY_KEY_H

#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::store {

/// The key a store directory is known by in this process: absolute and without
/// a trailing separator, so two spellings of one directory share one store.
dstd::Path directory_key(dstd::StringView dir);

}  // namespace dftracer::utils::store

#endif  // DFTRACER_UTILS_STORE_DIRECTORY_KEY_H
