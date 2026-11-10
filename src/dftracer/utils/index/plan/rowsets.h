// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_PLAN_ROWSETS_H
#define DFTRACER_UTILS_INDEX_PLAN_ROWSETS_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/record_schema.h>

namespace dftracer::utils::index {
class Snapshot;
}

namespace dftracer::utils::index::plan {

/// The schema the index recorded for `trace`, or null; an error when the index
/// cannot be read or the recorded schema is not registered.
dstd::Result<const RecordSchema*> recorded_schema(const Snapshot& snapshot,
                                                  dstd::CStringView trace);

/// A trace and the index that covers it.
struct RowSetFile {
    dstd::String trace;
    dstd::String index;
    /// An open `trace` the caller owns and keeps open, or null. A schema probe
    /// reads through it instead of opening the file.
    const dstd::File* file = nullptr;
};

/// The record schema of `files`: each one's recorded schema, or the detected
/// one when its index holds none (dftracer when the trace is missing too). A
/// file without records (empty, or only metadata) takes no vote; files of no
/// records are the first one's with objects, else generic. Never null. An
/// InvalidArgument error when files with records differ, and the errors of
/// reading an index or a trace head.
dstd::Result<const RecordSchema*> files_schema(
    const dstd::Vector<RowSetFile>& files);

/// Row set `name` as the index build stored it, the files' rows in file
/// order; nullopt when a file's index is missing, stale or holds no rows
/// for it, or the files' frames cannot be joined. Reports no error.
dstd::Optional<dataframe::DataFrame> stored_rowset(
    const dstd::Vector<RowSetFile>& files, dstd::StringView name);

/// Whether every file's index is fresh and holds its row sets, read from
/// the manifests only.
bool has_stored_rowsets(const dstd::Vector<RowSetFile>& files);

}  // namespace dftracer::utils::index::plan

#endif  // DFTRACER_UTILS_INDEX_PLAN_ROWSETS_H
