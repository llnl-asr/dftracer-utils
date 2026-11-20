// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_TABLE_TABLE_CODEC_H
#define DFTRACER_UTILS_TRACE_TABLE_TABLE_CODEC_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>

namespace dftracer::utils::trace::table {

/// The table file format dftu.table v1: gzip or plain JSON lines. Line 1 is a
/// ph 4 record whose args name the format, the table, the row count, the index
/// levels and every field with its pandas dtype. Every other line is a ph 3
/// record with one row in args, in field order. A float NaN, +inf and -inf are
/// the strings "NaN", "Infinity" and "-Infinity"; a null is JSON null.

struct WriteOptions {
    /// Columns of String type whose cells hold JSON text, written as is.
    dstd::Span<const dstd::StringView> raw_columns;
    int level = 6;
    std::size_t workers = 0;  ///< compression workers; 0 is the machine's
    std::size_t member_size = 4 * 1024 * 1024;  ///< gzip member size
    bool compress = true;                       ///< false writes a plain file
};

/// Writes `frame` to `path` as a table. `header_args_json` is the args object
/// of the header record. Its "table" names the rows, its "nrows" must equal
/// the frame's row count, and its "fields" must name the frame's columns in
/// order. The frame's row index is not written; the caller passes the index
/// levels as columns.
dstd::Result<void> write_table(const dataframe::DataFrame& frame,
                               dstd::StringView path,
                               dstd::StringView header_args_json,
                               const WriteOptions& options = {});

struct ReadOptions {
    /// Parts the rows are cut into, parsed in parallel; 0 is automatic.
    std::size_t parts = 0;
};

/// A decoded table. A column has one type for its header dtype: Int64 for the
/// signed integers, Uint64, Float64, Bool, and String for string, object and
/// category dtypes whose labels are strings. A column with elem set, an object
/// column of arrays, holds the JSON text of each cell; so does a category of
/// mixed labels. A category of integers is Int64 and of numbers Float64. A
/// null cell is null; NaN is a float value.
struct Table {
    dataframe::DataFrame frame;
    dstd::String header_json;  ///< the args object of the header record
    /// Gzip members inflated in parallel; 0 for a plain file or an in-order
    /// decode.
    std::size_t members_in_parallel = 0;
    /// True when the members' marks did not hold and the file was inflated
    /// one member after another.
    bool inflated_in_order = false;
};

/// Reads a table written by @ref write_table or by DFAnalyzer's checkpoint
/// writer, gzip or plain. A row needs only its "args". Refuses an unknown
/// format or format_version, a row that lacks a field or holds a value its
/// dtype cannot take, a row that is not JSON, and a row count that differs
/// from the header's. Blank lines are skipped.
dstd::Result<Table> read_table(dstd::StringView path,
                               const ReadOptions& options = {});

}  // namespace dftracer::utils::trace::table

#endif  // DFTRACER_UTILS_TRACE_TABLE_TABLE_CODEC_H
