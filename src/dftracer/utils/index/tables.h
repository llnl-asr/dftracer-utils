// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_TABLES_H
#define DFTRACER_UTILS_INDEX_TABLES_H

#include <dftracer/utils/coro/async_mutex.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/gzip_member_record.h>
#include <dftracer/utils/index/gzip/member.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/schemas/dft/agg/chunk_columns.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/schemas/dft/statistics.h>
#include <dftracer/utils/index/store/file.h>
#include <dftracer/utils/index/store/types.h>
#include <dftracer/utils/store/store.h>

#include <cstdint>
#include <utility>

// The typed tables of the index: one reader per record kind on a Snapshot and
// a FileRef, and one writer per record kind on a FileBuild. A reader returns
// owned values. A later step can return views into the snapshot for the hot
// readers. Only index/ uses this header.
namespace dftracer::utils::index {

namespace store {

/// The type a column reports in a schema, derived from the path catalog.
enum class ColumnType : std::uint8_t {
    Unknown = 0,
    Int64 = 1,
    Float64 = 2,
    String = 3,
    /// Text in some records and numbers or bools in others.
    Json = 4,
};

/// Folds two observations of one column's type. Unknown yields to the other,
/// Int64 and Float64 widen to Float64 and any other mix is Json. Commutative
/// and associative, so it merges across files.
ColumnType merge_column_type(ColumnType a, ColumnType b);

/// The type of a catalog path from the bitmask of PathType values seen for
/// it. Unknown when only nulls, objects or arrays were seen.
ColumnType seen_column_type(std::uint8_t seen);

/// The lowercase name of a ColumnType, such as "int64". Empty for Unknown.
const char* column_type_name(ColumnType t);

}  // namespace store

/// (granule, payload) in granule order.
using GranuleList = dstd::Vector<dstd::Pair<std::uint64_t, dstd::String>>;

/// The data of a plugin extension of one file.
struct PluginData {
    GranuleList granules;
    dstd::Optional<dstd::String> file;
};

/// One plugin extension built for a file.
struct PluginCopy {
    dstd::String name;
    std::uint32_t version = 0;
    std::uint64_t params_hash = 0;
    bool ready = false;
};

/// The store namespace of a built-in extension.
dftracer::utils::store::NamespaceSpec ext_spec(store::IndexExtension ext);
/// The store namespace of plugin extension `name` at `version`.
dftracer::utils::store::NamespaceSpec plugin_spec(dstd::StringView name,
                                                  std::uint32_t version);

// Readers. An absent record is an empty result or nullopt, never an error.

dstd::Result<dstd::Vector<gzip::GzipMemberRecord>> members(
    const Snapshot& snapshot, const FileRef& file);
/// `[i]` describes pruner chunk `i`. Empty when the file has no ready members.
dstd::Result<dstd::Vector<gzip::ChunkSpan>> spans(const Snapshot& snapshot,
                                                  const FileRef& file);
dstd::Result<dstd::Optional<dstd::String>> restart_window(
    const Snapshot& snapshot, const FileRef& file, std::uint64_t member_idx);
dstd::Result<dstd::Optional<store::FileMetadataResult>> file_metadata(
    const Snapshot& snapshot, const FileRef& file);

dstd::Result<dstd::Vector<schemas::dft::ChunkStatisticsResult>> chunk_stats(
    const Snapshot& snapshot, const FileRef& file);
dstd::Result<dstd::Optional<schemas::dft::MergedStatisticsResult>> scalar_stats(
    const Snapshot& snapshot, const FileRef& file);
dstd::Result<dstd::Optional<dstd::StringViewMap<std::uint64_t>>> pid_tid_counts(
    const Snapshot& snapshot, const FileRef& file);
/// Nullopt when the metadata extension is not ready for the file.
dstd::Result<dstd::Optional<dstd::HashMap<std::uint64_t, store::ChunkMetadata>>>
chunk_meta(const Snapshot& snapshot, const FileRef& file);

/// The file's path catalog, sorted by path.
dstd::Result<dstd::Vector<dstd::Pair<dstd::String, store::PathStat>>> catalog(
    const Snapshot& snapshot, const FileRef& file);
/// The id of the schema the file was indexed with.
dstd::Result<dstd::Optional<dstd::String>> schema(const Snapshot& snapshot,
                                                  const FileRef& file);
dstd::Result<dstd::Optional<dstd::String>> rowset(const Snapshot& snapshot,
                                                  const FileRef& file,
                                                  dstd::StringView name);

/// The paths a path-keyed extension indexed for the file.
dstd::Result<dstd::Vector<dstd::String>> paths(const Snapshot& snapshot,
                                               const FileRef& file,
                                               store::IndexExtension ext);
dstd::Result<GranuleList> granules(const Snapshot& snapshot,
                                   const FileRef& file,
                                   store::IndexExtension ext,
                                   dstd::StringView path);
/// The file-level payload of `path`.
dstd::Result<dstd::Optional<dstd::String>> path_file(const Snapshot& snapshot,
                                                     const FileRef& file,
                                                     store::IndexExtension ext,
                                                     dstd::StringView path);

dstd::Result<bool> has_postings(const Snapshot& snapshot, const FileRef& file,
                                dstd::StringView path);
dstd::Result<bool> has_posting(const Snapshot& snapshot, const FileRef& file,
                               dstd::StringView path, std::uint64_t value_hash);
dstd::Result<dstd::Vector<std::uint64_t>> posting_granules(
    const Snapshot& snapshot, const FileRef& file, dstd::StringView path,
    std::uint64_t value_hash);

/// Every plugin extension built for the file, by name.
dstd::Result<dstd::Vector<PluginCopy>> plugins(const Snapshot& snapshot,
                                               const FileRef& file);
/// Nullopt when no copy of plugin `name` exists for the file.
dstd::Result<dstd::Optional<PluginData>> plugin(const Snapshot& snapshot,
                                                const FileRef& file,
                                                dstd::StringView name);
/// The granules alone, in granule order. Nullopt when no copy of plugin `name`
/// exists for the file.
dstd::Result<dstd::Optional<GranuleList>> plugin_granules(
    const Snapshot& snapshot, const FileRef& file, dstd::StringView name);
/// The file payload alone. Nullopt when the file has none, or holds no copy of
/// plugin `name`.
dstd::Result<dstd::Optional<dstd::String>> plugin_file(const Snapshot& snapshot,
                                                       const FileRef& file,
                                                       dstd::StringView name);

/// The aggregation tier of one file. `bytes` is the serialized AggPartial and
/// stays valid during the call.
using AggPartialFn = dstd::Function<bool(
    std::uint64_t chunk, std::uint32_t cat_id, std::uint32_t name_id,
    std::uint32_t host_id, std::uint32_t extra_id,
    schemas::dft::agg::PartialKind kind, dstd::Span<const std::uint8_t> bytes)>;
using AggChunkColumnsFn = dstd::Function<bool(
    std::uint64_t chunk, const schemas::dft::agg::ChunkColumns& block)>;

/// Calls fn for each partial of the file until fn returns false. A malformed
/// record is an error, not a throw.
dstd::Result<void> for_each_agg_partial(const Snapshot& snapshot,
                                        const FileRef& file,
                                        const AggPartialFn& fn);
/// `block.texts` holds the text of each id of `block.ids`, empty where the file
/// holds none.
dstd::Result<void> for_each_agg_chunk_columns(const Snapshot& snapshot,
                                              const FileRef& file,
                                              const AggChunkColumnsFn& fn);
dstd::Result<dstd::Optional<schemas::dft::agg::ChunkColumns>> agg_chunk_columns(
    const Snapshot& snapshot, const FileRef& file, std::uint64_t chunk);
dstd::Result<void> agg_file_texts(
    const Snapshot& snapshot, const FileRef& file,
    dstd::HashMap<std::uint32_t, dstd::String>& texts);
/// Every dictionary entry the index holds, in key order.
dstd::Result<void> agg_dictionary(const Snapshot& snapshot,
                                  schemas::dft::agg::AggInternTable& table);
/// The index-wide aggregation record `name`.
dstd::Result<dstd::Optional<dstd::String>> agg_meta(const Snapshot& snapshot,
                                                    dstd::StringView name);

/// The hash the file was indexed with.
dstd::Optional<std::uint64_t> file_hash(const Snapshot& snapshot,
                                        const FileRef& file);
/// The file's copy of built-in extension `ext`: the version its build recorded,
/// its params hash and whether it finished. Nullopt when the file holds no
/// copy, and for a plugin extension.
dstd::Optional<store::ExtensionState> extension_state(
    const Snapshot& snapshot, const FileRef& file, store::IndexExtension ext);
/// The extension is ready, and at the format version this code reads. False for
/// a plugin extension, which is current only for its params hash.
bool extension_current(const Snapshot& snapshot, const FileRef& file,
                       store::IndexExtension ext);
/// The type of each catalog path, in path order, without the paths whose
/// observed values have none.
dstd::Result<dstd::Vector<dstd::Pair<dstd::String, store::ColumnType>>>
file_column_types(const Snapshot& snapshot, const FileRef& file);

// Writers. Each writes the keys and values of the current format into the
// building copy of the extension. A failed put is reported by commit().

void put_file_metadata(FileBuild& build, std::uint64_t checkpoint_size,
                       std::uint64_t total_lines, std::uint64_t total_uc_size,
                       bool truncated);
void put_gzip_member(FileBuild& build, const gzip::GzipMemberRecord& member);
void put_restart_window(FileBuild& build, std::uint64_t member_idx,
                        dstd::StringView window);

void put_chunk_statistics(FileBuild& build, std::uint64_t checkpoint_idx,
                          const schemas::dft::ChunkStatistics& stats);
void put_chunk_metadata(FileBuild& build, std::uint64_t checkpoint_idx,
                        const store::ChunkMetadata& metadata);
void put_file_scalar_stats(FileBuild& build,
                           const schemas::dft::ChunkStatistics& stats,
                           std::uint64_t num_chunks);
void put_file_pid_tid_counts(FileBuild& build,
                             const dstd::StringViewMap<std::uint64_t>& counts);

void put_profile(FileBuild& build, dstd::StringView profile_id);
void put_catalog_path(FileBuild& build, dstd::StringView path,
                      const store::PathStat& stat);
void put_rowset(FileBuild& build, dstd::StringView name,
                dstd::StringView frame);

/// Path-keyed data of the pruning extensions, not of a plugin.
void put_path(FileBuild& build, store::IndexExtension ext,
              dstd::StringView path);
void put_path_granule(FileBuild& build, store::IndexExtension ext,
                      dstd::StringView path, std::uint64_t granule,
                      dstd::StringView payload);

/// Erases every record of one path of `ext`, so a run that wrote a path the
/// file's selection did not keep leaves nothing of it.
void erase_path(FileBuild& build, store::IndexExtension ext,
                dstd::StringView path);
void put_path_file(FileBuild& build, store::IndexExtension ext,
                   dstd::StringView path, dstd::StringView payload);
void put_posting(FileBuild& build, dstd::StringView path,
                 std::uint64_t value_hash);
void put_posting_granule(FileBuild& build, dstd::StringView path,
                         std::uint64_t value_hash, std::uint64_t granule);

/// `payload` is the serialized AggPartial. The ids are dictionary ids.
void put_agg_partial(FileBuild& build, std::uint64_t chunk,
                     std::uint32_t cat_id, std::uint32_t name_id,
                     std::uint32_t host_id, std::uint32_t extra_id,
                     std::uint8_t kind, dstd::StringView payload);
void put_agg_chunk_columns(FileBuild& build, std::uint64_t chunk,
                           dstd::StringView payload);
void put_agg_dictionary_entry(FileBuild& build, std::uint32_t id,
                              dstd::StringView text);

/// As put_agg_partial, with `value` the stored value, so it is stored without a
/// copy.
void put_agg_partial_value(FileBuild& build, std::uint64_t chunk,
                           std::uint32_t cat_id, std::uint32_t name_id,
                           std::uint32_t host_id, std::uint32_t extra_id,
                           std::uint8_t kind, dstd::StringView value);
/// As put_agg_chunk_columns, with `value` the stored value.
void put_agg_chunk_columns_value(FileBuild& build, std::uint64_t chunk,
                                 dstd::StringView value);

/// Writes and finishes plugin extension `name` in one call.
void put_plugin(FileBuild& build, dstd::StringView name, std::uint32_t version,
                std::uint64_t params_hash, const GranuleList& granules,
                const dstd::Optional<dstd::String>& file);

/// As above, with views, which are stored as they are, without a copy.
using GranuleViews = dstd::Vector<dstd::Pair<std::uint64_t, dstd::StringView>>;
void put_plugin_views(FileBuild& build, dstd::StringView name,
                      std::uint32_t version, std::uint64_t params_hash,
                      const GranuleViews& granules,
                      const dstd::Optional<dstd::StringView>& file);

/// Marks plugin extension `name` failed for `params_hash`.
void fail_plugin(FileBuild& build, dstd::StringView name, std::uint32_t version,
                 std::uint64_t params_hash);

/// Makes the commit drop every copy of the file the build does not write,
/// plugin copies included, except the built-in extensions in `keep`. For a
/// full rebuild, whose new members renumber the chunks the old copies describe.
void clear_unwritten(FileBuild& build,
                     dstd::Span<const store::IndexExtension> keep = {});

/// @ref clear_unwritten that keeps the per-file copies (catalog, profile, row
/// sets), for a build that writes new members and not every chunk extension.
void clear_chunk_copies(FileBuild& build);

/// Starts a build of a file the index holds, with the stat it was indexed
/// with, to add or replace extensions at query time.
dstd::Result<FileBuild> rebuild(const Index& index, dstd::StringView path);

/// Marks the extension failed for `params_hash`. The failure lands after the
/// publish of the finished extensions, not in the same record.
void fail_extension(FileBuild& build, store::IndexExtension ext,
                    std::uint64_t params_hash);

/// One task writes a spilled run into a build at a time: it holds the claim
/// while it puts the run and stages it. The build must stay alive until the
/// task ends.
class Claim {
   public:
    Claim(FileBuild& build, coro::AsyncMutexGuard held)
        : build_(&build), held_(std::move(held)) {}

    FileBuild& build() const { return *build_; }

    /// Commits the building batches the claim's task put so far, so their
    /// memory is released, and publishes nothing. Later puts join the same
    /// building copies, and commit() publishes all of them.
    coro::CoroTask<dstd::Result<void>> stage();

   private:
    FileBuild* build_;
    coro::AsyncMutexGuard held_;
};

/// Publishes the builds in one record. The result has one slot per build: its
/// error or empty. A build that failed a put or is unfinished is skipped, and
/// a failed publish errors every published build. The builds must come from
/// one Index and stay alive until the task ends.
coro::CoroTask<dstd::Vector<dstd::Optional<dstd::Error>>> commit_group(
    dstd::Span<FileBuild* const> builds);

/// Waits for the build's current writer, if any, to give it up.
coro::CoroTask<dstd::UniquePtr<Claim>> claim(FileBuild& build);

// Writes that are not about one file. Each commits in one record, and its
// arguments must stay alive until the task ends.

/// Replaces the index-wide aggregation record `name` with `payload`.
coro::CoroTask<dstd::Result<void>> put_agg_meta(const Index& index,
                                                dstd::StringView name,
                                                dstd::StringView payload);
/// Deletes the index-wide aggregation record `name`. A missing record is not an
/// error.
coro::CoroTask<dstd::Result<void>> delete_agg_meta(const Index& index,
                                                   dstd::StringView name);
/// Drops the copy of one built-in extension of every file in `paths`. A file
/// without a copy is skipped.
coro::CoroTask<dstd::Result<void>> remove_extension(
    const Index& index, store::IndexExtension ext,
    dstd::Span<const dstd::StringView> paths);

/// Drops the copy of plugin extension `name` of every file in `paths`. A file
/// without a copy, and a name the index holds no copy of, are skipped.
coro::CoroTask<dstd::Result<void>> remove_plugin(
    const Index& index, dstd::StringView name,
    dstd::Span<const dstd::StringView> paths);

}  // namespace dftracer::utils::index

#endif  // DFTRACER_UTILS_INDEX_TABLES_H
