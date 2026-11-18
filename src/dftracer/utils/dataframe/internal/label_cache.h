// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_LABEL_CACHE_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_LABEL_CACHE_H

#include <dftracer/utils/dstd/memory.h>

#include <mutex>

namespace dftracer::utils::dataframe {

class LabelIndex;

/// The lookup structure of one set of row labels, built on the first lookup.
/// `once` makes the build happen exactly once even when several threads look
/// up at the same time: the others wait for it, then read `index`, which no
/// one writes again. A build that fails leaves `index` null, and every lookup
/// then builds and reports the error itself.
struct LabelCache {
    std::once_flag once;
    dstd::SharedPtr<const LabelIndex> index;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_LABEL_CACHE_H
