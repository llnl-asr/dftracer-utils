// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_TABLE_TABLE_SOURCE_H
#define DFTRACER_UTILS_TRACE_TABLE_TABLE_SOURCE_H

#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::trace::table {

/// One input file of a view, with the index root that serves it.
struct TableInput {
    dstd::String path;
    dstd::String index_path;  ///< empty for none
};

/// What the table reads of this process have done, for tests and tuning.
struct ReadCounters {
    /// Index chunks (gzip members) a read inflated, and chunks that a fresh
    /// index proved hold no row of a filter.
    std::uint64_t chunks_read = 0;
    std::uint64_t chunks_skipped = 0;
    /// Files read by the chosen path: through a fresh index, in batches with
    /// no index, or whole in one parallel decode.
    std::uint64_t indexed_files = 0;
    std::uint64_t streamed_files = 0;
    std::uint64_t direct_files = 0;
    /// Bytes that the indexed and streamed reads handed to the decoder.
    std::uint64_t bytes_decoded = 0;
};

ReadCounters read_counters();
void reset_read_counters();

/// Reads the first line of each file of `inputs` (with bounded concurrency,
/// and only the first gzip member) and decides whether they are tables.
///
/// A file is a table when its line 1 has `ph` 4, `name` "dftu.table" and
/// `args.format` "dftu.table". A line 1 that claims the format but is
/// malformed, or has an unknown format_version, is an error: it never falls
/// back to a trace read. A file that cannot be opened or read is not a table,
/// so the trace path reports it. When no file is a table the result is empty.
/// A mix of tables and traces is an InvalidArgument error that names one file
/// of each kind. Tables must agree on their fields (name, dtype, ordered,
/// elem, arrow_type, the kind of their labels) and index names, else it is an
/// InvalidArgument error before any row is read.
///
/// The files of one folder are read in part-number order (`part-2` before
/// `part-10`); folders keep the order of their first file in `inputs`. The
/// source decodes only the columns of a scan's projection, in bounded batches,
/// and takes candidate chunks from an index that is fresh for the file (mtime,
/// size and schema) when the scan has a filter it can prove no match for;
/// else it reads every chunk. It never builds or changes an index.
dstd::Result<dstd::Optional<dstd::SharedPtr<const dataframe::Source>>>
open_table_inputs(dstd::Span<const TableInput> inputs);

/// The args object (JSON text) of the header record of each file of a source
/// made by @ref open_table_inputs, in read order; empty for any other source.
dstd::Vector<dstd::String> table_headers(const dataframe::Source& source);

/// The paths of those files, in the same order.
dstd::Vector<dstd::String> table_paths(const dataframe::Source& source);

}  // namespace dftracer::utils::trace::table

#endif  // DFTRACER_UTILS_TRACE_TABLE_TABLE_SOURCE_H
