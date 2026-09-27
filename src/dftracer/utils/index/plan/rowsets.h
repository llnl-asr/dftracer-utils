#ifndef DFTRACER_UTILS_INDEX_PLAN_ROWSETS_H
#define DFTRACER_UTILS_INDEX_PLAN_ROWSETS_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/index_database.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::plan {

/// The schema the index recorded for `trace`, or null.
const RecordSchema* recorded_schema(const store::IndexDatabase& db,
                                    const std::string& trace);

/// A trace and the index that covers it.
struct RowSetFile {
    std::string trace;
    std::string index;
};

/// The record schema of `files`: each one's recorded schema, or the detected
/// one when its index holds none (dftracer when the trace is missing too). A
/// file without records (empty, or only metadata) takes no vote; files of no
/// records are the first one's with objects, else generic. Throws
/// DFTUtilsException INVALID_ARGUMENT when files with records differ.
const RecordSchema& files_schema(const std::vector<RowSetFile>& files);

/// Row set `name` as the index build stored it, the files' rows in file
/// order; nullopt when a file's index is missing, stale or holds no rows
/// for it. Never throws for a missing or unreadable index.
std::optional<dataframe::DataFrame> stored_rowset(
    const std::vector<RowSetFile>& files, std::string_view name);

/// Whether every file's index is fresh and holds its row sets, read from
/// the manifests only.
bool has_stored_rowsets(const std::vector<RowSetFile>& files);

}  // namespace dftracer::utils::index::plan

#endif  // DFTRACER_UTILS_INDEX_PLAN_ROWSETS_H
