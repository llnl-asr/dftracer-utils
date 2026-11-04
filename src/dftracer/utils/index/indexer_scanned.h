// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_INDEXER_SCANNED_H
#define DFTRACER_UTILS_INDEX_INDEXER_SCANNED_H

#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/utilities/filesystem/types.h>

namespace dftracer::utils::index::detail {

/// Opens trace files a caller already scanned, with the size and mtime that
/// scan read. The first status(), build() or rebuild() of the @ref Indexer
/// uses them without stat'ing. Later calls stat the files. Throws
/// INVALID_ARGUMENT when `files` is empty.
Indexer open_scanned(dstd::Vector<utilities::filesystem::FileEntry> files,
                     IndexerOptions options = {});

}  // namespace dftracer::utils::index::detail

#endif  // DFTRACER_UTILS_INDEX_INDEXER_SCANNED_H
