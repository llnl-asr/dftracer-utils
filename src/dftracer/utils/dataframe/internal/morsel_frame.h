// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_MORSEL_FRAME_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_MORSEL_FRAME_H

#include <dftracer/utils/dataframe/internal/name_cache.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

namespace dftracer::utils::dataframe {

/// One standalone DataFrame from a morsel a cursor produced. Names come from
/// the morsel's own schema, else the cursor's data-dependent `out_names`, else
/// `static_names`; a rest column is unpacked into named columns.
dstd::Result<DataFrame> morsel_to_frame(
    Morsel&& m, NameCache& cache,
    const dstd::Vector<dstd::String>& static_names,
    const dstd::Optional<dstd::Vector<dstd::String>>& out_names);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_MORSEL_FRAME_H
