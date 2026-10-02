#ifndef DFTRACER_UTILS_DATAFRAME_ARROW_BRIDGE_H
#define DFTRACER_UTILS_DATAFRAME_ARROW_BRIDGE_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/dataframe/dataframe.h>

#include <vector>

struct ArrowSchema;
struct ArrowArray;

namespace dftracer::utils::dataframe {

/// Export `col` into (`schema`, `array`) using the Arrow C Data Interface, zero
/// copy: the array's buffers alias the column's, and the array's release keeps
/// them alive. FLAT exports as a primitive array; SELECTION exports as a
/// dictionary array (indices = the selection, dictionary = the base). The
/// caller owns both structs and must call their release callbacks.
void to_arrow(const Series& col, ArrowSchema* schema, ArrowArray* array);

/// Export a struct Series (a DataFrame) as record batches: one per chunk when
/// every field is a CHUNKED column with identical chunk starts and flat
/// chunks, else one joined batch. Zero copy; each array keeps its chunks
/// alive. The caller owns `schema` and every returned array.
std::vector<ArrowArray> to_arrow_batches(const Series& st, ArrowSchema* schema);

/// Import a FLAT primitive Arrow array as a Series, wrapping its buffers zero
/// copy. Ownership of `array` moves into the returned Series (the array's
/// release runs when the Series is destroyed); the caller's `array` is marked
/// released. Invalid (empty) Series on an unsupported type.
Series from_arrow(const ArrowSchema* schema, ArrowArray* array);

/// Import a STRUCT Arrow array as a DataFrame: one column per struct child,
/// named by the child schema, each imported zero copy. Ownership of `array`
/// moves into the returned frame (its release runs when the last column buffer
/// is dropped); the caller's `array` is marked released. An empty frame on a
/// non-struct or unsupported child type.
DataFrame dataframe_from_arrow(const ArrowSchema* schema, ArrowArray* array);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_DATAFRAME_ARROW_BRIDGE_H
