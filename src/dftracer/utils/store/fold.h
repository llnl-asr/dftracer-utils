// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_STORE_FOLD_H
#define DFTRACER_UTILS_STORE_FOLD_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/store/segment.h>
#include <dftracer/utils/store/store.h>

#include <cstdint>

namespace dftracer::utils::store::engine {

/// True for the folds that store one 8-byte little-endian number: an integer
/// or an IEEE double.
bool is_numeric_fold(FoldKind kind);

FoldFn fold_fn(FoldKind kind);

/// InvalidArgument when a numeric fold gets a value that is not 8 bytes.
/// FoldKind::None, Count, Last and Concat take any bytes.
dstd::Expected<void, dstd::Error> check_fold_value(FoldKind kind,
                                                   dstd::ByteView value);

}  // namespace dftracer::utils::store::engine

#endif  // DFTRACER_UTILS_STORE_FOLD_H
