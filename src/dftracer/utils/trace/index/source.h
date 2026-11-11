// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_SOURCE_H
#define DFTRACER_UTILS_TRACE_INDEX_SOURCE_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/trace/index/record_schema.h>

#include <utility>

namespace dftracer::utils::trace::index {

/// The duql roles of `schema`'s fields: each time or duration field (by name
/// and path) with its nanoseconds per unit.
duql::Roles duql_roles(const RecordSchema& schema);

/// `child`'s source members over `parent`'s, as canonical text: a member of
/// the same name replaces the parent's, in the parent's position. An
/// InvalidArgument error naming `origin` when either does not parse, a row set
/// is named `all`, `data` is anything but one `where`, or a name is declared
/// twice.
dstd::Result<dstd::String> merge_source(dstd::StringView parent,
                                        dstd::StringView child,
                                        dstd::StringView origin);

/// Compiles `schema`'s source with its roles. An InvalidArgument error naming
/// the schema and `origin` when it does not compile.
dstd::Result<void> check_source(const RecordSchema& schema,
                                dstd::StringView origin);

/// The condition of `schema`'s `data` row set, or empty when it reads
/// `all`. The source must have passed @ref check_source; one that does not
/// parse ends the process.
dstd::String data_condition(const RecordSchema& schema);

/// Whether `schema`'s source defines `args_fallback = true`. The source must
/// have passed @ref check_source; one that does not parse ends the process.
bool source_args_fallback(const RecordSchema& schema);

/// The `data` condition that leaves out the metadata phase, which a View
/// maps onto its phase selection instead of a filter.
inline constexpr dstd::StringView NO_METADATA = R"(ph not in ["M", 4])";

/// A row set the index build evaluates: its filter (every record when
/// absent), per output column its name and record path, and whether it
/// keeps each distinct row once, in the order rows first occur.
struct IndexedRowSet {
    dstd::String name;
    dstd::Optional<duql::Query> filter;
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> columns;
    bool distinct = false;
};

/// The row sets of `schema`'s source that are a `where` followed by at most
/// one `select` of record paths and an optional `distinct`, in source
/// order; `data` is not one.
dstd::Vector<IndexedRowSet> indexed_rowsets(const RecordSchema& schema);

}  // namespace dftracer::utils::trace::index

#endif  // DFTRACER_UTILS_TRACE_INDEX_SOURCE_H
