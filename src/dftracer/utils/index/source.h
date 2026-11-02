// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_SOURCE_H
#define DFTRACER_UTILS_INDEX_SOURCE_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/record_schema.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::index {

/// The duql roles of `schema`'s fields: each time or duration field (by name
/// and path) with its nanoseconds per unit.
duql::Roles duql_roles(const RecordSchema& schema);

/// `child`'s source members over `parent`'s, as canonical text: a member of
/// the same name replaces the parent's, in the parent's position. Throws
/// DFTUtilsException INVALID_ARGUMENT naming `origin` when either does not
/// parse, a row set is named `all`, `data` is anything but one `where`, or a
/// name is declared twice.
std::string merge_source(dstd::StringView parent, dstd::StringView child,
                         dstd::StringView origin);

/// Compiles `schema`'s source with its roles. Throws DFTUtilsException
/// INVALID_ARGUMENT naming the schema and `origin` when it does not compile.
void check_source(const RecordSchema& schema, dstd::StringView origin);

/// The condition of `schema`'s `data` row set, or empty when it reads
/// `all`.
std::string data_condition(const RecordSchema& schema);

/// Whether `schema`'s source defines `args_fallback = true`.
bool source_args_fallback(const RecordSchema& schema);

/// The `data` condition that leaves out the metadata phase, which a View
/// maps onto its phase selection instead of a filter.
inline constexpr dstd::StringView NO_METADATA = R"(ph not in ["M", 4])";

/// A row set the index build evaluates: its filter (every record when
/// absent), per output column its name and record path, and whether it
/// keeps each distinct row once, in the order rows first occur.
struct IndexedRowSet {
    std::string name;
    dstd::Optional<duql::Query> filter;
    std::vector<dstd::Pair<std::string, std::string>> columns;
    bool distinct = false;
};

/// The row sets of `schema`'s source that are a `where` followed by at most
/// one `select` of record paths and an optional `distinct`, in source
/// order; `data` is not one.
std::vector<IndexedRowSet> indexed_rowsets(const RecordSchema& schema);

}  // namespace dftracer::utils::index

#endif  // DFTRACER_UTILS_INDEX_SOURCE_H
