// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_ARROW_INTERNAL_H
#define DFTRACER_UTILS_ARROW_INTERNAL_H

#include <dftracer/utils/arrow/batch.h>
#include <dftracer/utils/arrow/c_api.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::arrow::detail {

/// Fills `schema` and `array` from `col`, zero copy; the caller releases both.
void to_arrow(const dataframe::Series& col, ArrowSchema* schema,
              ArrowArray* array);

/// Fills `schema` with the Arrow type of `col`, without exporting a buffer.
void column_schema(const dataframe::Series& col, ArrowSchema* schema);

/// Fills `array` with the buffers of `col`, zero copy; the caller releases it.
void column_array(const dataframe::Series& col, ArrowArray* array);

/// `frame` as one struct column whose children share its buffers.
dataframe::Series struct_column(const dataframe::DataFrame& frame);

/// One exported array per chunk of `st`, and in `schema` one export of the
/// column's schema that all of them follow. The caller releases the schema and
/// every array.
dstd::Vector<ArrowArray> to_arrow_batches(const dataframe::Series& st,
                                          ArrowSchema* schema);

/// Wraps `array`, which it takes ownership of, as a Series; `keep` is co-owned
/// by every wrapped buffer and outlives the last of them. An invalid Series for
/// an Arrow type the engine cannot hold.
dataframe::Series import_series(const ArrowSchema* schema, ArrowArray* array,
                                dstd::SharedPtr<void> keep);

/// The same for a struct `array`: one column per child.
dataframe::DataFrame import_frame(const ArrowSchema* schema, ArrowArray* array,
                                  dstd::SharedPtr<void> keep);

/// True when `schema` describes a struct array, the only shape a
/// `dataframe::DataFrame` has.
bool is_struct(const ArrowSchema* schema);

/// The engine type of the Arrow field `schema`; a type id of Unknown for a
/// nested type the plan cannot name without a batch.
dataframe::DataType field_type(const ArrowSchema* schema);

// Test hooks: they let a test observe the read-ahead of an IPC scan, or make
// one writer call fail where a real export can fail.
/// The largest total size of the files one IPC read-ahead held at once, since
/// the last reset.
std::uint64_t peak_ipc_in_flight_bytes_for_test();

/// Clears that peak.
void reset_ipc_in_flight_bytes_for_test();

/// Makes the next writer call fail before it writes the schema, without making
/// that writer fail for good.
void fail_next_ipc_write_for_test();

}  // namespace dftracer::utils::arrow::detail

#endif  // DFTRACER_UTILS_ARROW_INTERNAL_H
