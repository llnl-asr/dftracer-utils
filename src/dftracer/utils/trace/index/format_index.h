// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_FORMAT_INDEX_H
#define DFTRACER_UTILS_TRACE_INDEX_FORMAT_INDEX_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/file_format_registry.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/build/chunk_indexer.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/store/file.h>

#include <cstdint>

// The index of a file that a plugin file format reads. A chunk of the format
// is a granule of the summaries, and the file's @ref store::FormatRecord says
// which format and settings built them and how its chunks are named. The
// build side feeds one frame per chunk through the ColumnFeeder; the scan side
// reads the summaries back to name the chunks a filter can skip.
namespace dftracer::utils::trace::index {

/// The summaries a plugin-format file is indexed with: the pruning extensions
/// of `extensions`, built with the parameters of `config`.
struct FormatIndexSettings {
    build::ChunkIndexerConfig config;
};

/// The hash recorded in the manifest entry of a file's format record: the
/// format and the feeder. The settings of each summary are checked on their
/// own, so a file with more summaries than asked for is current. A file
/// indexed under another value is rebuilt.
std::uint64_t format_params_hash(dstd::StringView format,
                                 store::FeederKind feeder);

/// Whether `snapshot` holds a current index of the file at `path` as read by
/// `format`: its size and mtime are those of `now`, its format record is the
/// one `format` and `settings` give, and every summary and plugin extension
/// asked for is built with the current settings. The record names the plugin
/// version too: a plugin of the same name at another version is another reader.
/// False for a file that is not indexed, one that another reader (the built-in
/// JSON reader, or another format) indexed, and one whose settings changed.
dstd::Result<bool> format_file_current(const Snapshot& snapshot,
                                       dstd::StringView path,
                                       const SourceStat& now,
                                       dstd::StringView format,
                                       dstd::StringView plugin_version,
                                       const FormatIndexSettings& settings);

/// Indexes the file `source` reads into `index`, replacing every copy the
/// index holds of it, and no other file's. `stat` is the stat of the file. An
/// error from reading a chunk or from the commit leaves the old copy. `path`
/// is the name snapshots give the file (its logical path). `index`, `source`
/// and `settings` must outlive the task.
coro::CoroTask<dstd::Result<void>> build_format_index(
    const Index& index, dstd::String path, SourceStat stat,
    const dataframe::FormatFileSource& source,
    const FormatIndexSettings& settings);

/// What a scan of a format's file asks the index.
struct FormatScan {
    /// The index directory; empty puts the index beside the file.
    dstd::StringView index_dir;
    dstd::StringView path;
    dstd::StringView format;
    /// The version text of the plugin that registered the format.
    dstd::StringView plugin_version;
    /// The file's chunk tokens as the format gives them now.
    dstd::Span<const std::uint64_t> tokens;
    const dataframe::Schema* schema = nullptr;
    dstd::Span<const dstd::String> projection;
    dstd::Span<const dataframe::Expr> filters;
};

/// The chunks, ascending, that may hold a row every filter keeps, when the
/// index proves some chunk cannot; nullopt when it does not, and then the scan
/// reads every chunk. Nothing is skipped without evidence: no index, a file
/// that changed since it was indexed, another reader's index, chunks that are
/// not the indexed ones, a filter the summaries cannot judge, and an index
/// that cannot be read all give nullopt. A file whose format gives no chunk
/// tokens has one chunk, 0, and an empty result rules the whole file out.
dstd::Optional<dstd::Vector<std::uint64_t>> format_candidate_chunks(
    const FormatScan& scan);

}  // namespace dftracer::utils::trace::index

#endif  // DFTRACER_UTILS_TRACE_INDEX_FORMAT_INDEX_H
