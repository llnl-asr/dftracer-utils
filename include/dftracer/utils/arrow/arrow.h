// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_ARROW_ARROW_H
#define DFTRACER_UTILS_ARROW_ARROW_H

#include <dftracer/utils/arrow/c_api.h>
#include <dftracer/utils/arrow/owned_arrow.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::arrow {

/// Export `col` into (`schema`, `array`) using the Arrow C Data Interface, zero
/// copy: the array's buffers alias the column's, and the array's release keeps
/// them alive. FLAT exports as a primitive array; SELECTION exports as a
/// dictionary array (indices = the selection, dictionary = the base). The
/// caller owns both structs and must call their release callbacks.
void to_arrow(const dataframe::Series& col, ArrowSchema* schema,
              ArrowArray* array);

/// The same export of `col`, owned by the returned @ref OwnedArrow.
OwnedArrow to_arrow(const dataframe::Series& col);

/// Export `frame` as a struct array with one child per column, zero copy: the
/// exported buffers alias the columns' and the owner's release callbacks keep
/// them alive.
OwnedArrow to_arrow(const dataframe::DataFrame& frame);

/// Export a struct Series (a DataFrame) as record batches: one per chunk when
/// every field is a CHUNKED column with identical chunk starts and flat
/// chunks, else one joined batch. Zero copy; each array keeps its chunks
/// alive. The caller owns `schema` and every returned array.
dstd::Vector<ArrowArray> to_arrow_batches(const dataframe::Series& st,
                                          ArrowSchema* schema);

/// Import an Arrow array (viewed through `schema`) as a Series, wrapping its
/// buffers zero copy. ADOPTS `array`: ownership of its buffers moves into the
/// returned Series (the array's release runs when the Series is destroyed) and
/// the passed array is marked released, so the caller must not release it
/// again. `schema` is only read. An invalid (empty) Series on an unsupported
/// type.
dataframe::Series series_from_arrow(const ArrowSchema* schema,
                                    ArrowArray* array);

/// Import a STRUCT Arrow array (viewed through `schema`) as a DataFrame: one
/// column per struct child, named by its child schema, wrapped zero copy.
/// ADOPTS `array` (ownership of its buffers moves into the frame and the passed
/// array is marked released, so the caller must not release it again);
/// `schema` is only read. An empty frame on a non-struct or unsupported child
/// type.
dataframe::DataFrame dataframe_from_arrow(const ArrowSchema* schema,
                                          ArrowArray* array);

/// Serialize `frame` to an Arrow IPC stream (schema message + one record batch
/// + EOS) as bytes any Arrow IPC reader can open. An
/// @ref common::ErrorCode::Internal error result on an encode failure.
dstd::Result<dstd::Vector<std::uint8_t>> to_ipc(
    const dataframe::DataFrame& frame);

/// The frame an Arrow IPC stream of one record batch (@ref to_ipc) holds, or
/// nullopt when `bytes` is not one.
dstd::Optional<dataframe::DataFrame> from_ipc(dstd::StringView bytes);

}  // namespace dftracer::utils::arrow

#endif  // DFTRACER_UTILS_ARROW_ARROW_H
