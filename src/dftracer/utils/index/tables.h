#ifndef DFTRACER_UTILS_INDEX_TABLES_H
#define DFTRACER_UTILS_INDEX_TABLES_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/gzip_member_record.h>
#include <dftracer/utils/index/gzip/member.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/schemas/dft/statistics.h>
#include <dftracer/utils/index/store/file.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/store/store.h>

#include <cstdint>
#include <utility>

// The typed tables of the index: one reader per record kind on a Snapshot and
// a FileRef, and one writer per record kind on a FileBuild. A reader returns
// owned values. A later step can return views into the snapshot for the hot
// readers. Only index/ uses this header.
namespace dftracer::utils::index {

template <class T>
using Result = dstd::Expected<T, dstd::Error>;

/// (granule, payload) in granule order.
using GranuleList = dstd::Vector<std::pair<std::uint64_t, dstd::String>>;

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

Result<dstd::Vector<gzip::GzipMemberRecord>> members(const Snapshot& snapshot,
                                                     const FileRef& file);
/// `[i]` describes pruner chunk `i`. Empty when the file has no ready members.
Result<dstd::Vector<gzip::ChunkSpan>> spans(const Snapshot& snapshot,
                                            const FileRef& file);
Result<dstd::Optional<dstd::String>> restart_window(const Snapshot& snapshot,
                                                    const FileRef& file,
                                                    std::uint64_t member_idx);
Result<dstd::Optional<store::FileMetadataResult>> file_metadata(
    const Snapshot& snapshot, const FileRef& file);

Result<dstd::Vector<schemas::dft::ChunkStatisticsResult>> chunk_stats(
    const Snapshot& snapshot, const FileRef& file);
Result<dstd::Optional<schemas::dft::MergedStatisticsResult>> scalar_stats(
    const Snapshot& snapshot, const FileRef& file);
Result<dstd::Optional<dstd::StringViewMap<std::uint64_t>>> pid_tid_counts(
    const Snapshot& snapshot, const FileRef& file);
/// Nullopt when the metadata extension is not ready for the file.
Result<dstd::Optional<dstd::HashMap<std::uint64_t, store::ChunkMetadata>>>
chunk_meta(const Snapshot& snapshot, const FileRef& file);

/// The file's path catalog, sorted by path.
Result<dstd::Vector<std::pair<dstd::String, store::PathStat>>> catalog(
    const Snapshot& snapshot, const FileRef& file);
/// The id of the schema the file was indexed with.
Result<dstd::Optional<dstd::String>> schema(const Snapshot& snapshot,
                                            const FileRef& file);
Result<dstd::Optional<dstd::String>> rowset(const Snapshot& snapshot,
                                            const FileRef& file,
                                            dstd::StringView name);

/// The paths a path-keyed extension indexed for the file.
Result<dstd::Vector<dstd::String>> paths(const Snapshot& snapshot,
                                         const FileRef& file,
                                         store::IndexExtension ext);
Result<GranuleList> granules(const Snapshot& snapshot, const FileRef& file,
                             store::IndexExtension ext, dstd::StringView path);
/// The file-level payload of `path`.
Result<dstd::Optional<dstd::String>> path_file(const Snapshot& snapshot,
                                               const FileRef& file,
                                               store::IndexExtension ext,
                                               dstd::StringView path);

Result<bool> has_postings(const Snapshot& snapshot, const FileRef& file,
                          dstd::StringView path);
Result<bool> has_posting(const Snapshot& snapshot, const FileRef& file,
                         dstd::StringView path, std::uint64_t value_hash);
Result<dstd::Vector<std::uint64_t>> posting_granules(const Snapshot& snapshot,
                                                     const FileRef& file,
                                                     dstd::StringView path,
                                                     std::uint64_t value_hash);

/// Every plugin extension built for the file, by name.
Result<dstd::Vector<PluginCopy>> plugins(const Snapshot& snapshot,
                                         const FileRef& file);
/// Nullopt when no copy of plugin `name` exists for the file.
Result<dstd::Optional<PluginData>> plugin(const Snapshot& snapshot,
                                          const FileRef& file,
                                          dstd::StringView name);

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

/// As put_agg_partial, with `value` the whole stored value including the layout
/// header (layout::append_value), so it is stored without a copy.
void put_agg_partial_value(FileBuild& build, std::uint64_t chunk,
                           std::uint32_t cat_id, std::uint32_t name_id,
                           std::uint32_t host_id, std::uint32_t extra_id,
                           std::uint8_t kind, dstd::StringView value);
/// As put_agg_chunk_columns, with `value` the whole stored value including the
/// layout header.
void put_agg_chunk_columns_value(FileBuild& build, std::uint64_t chunk,
                                 dstd::StringView value);

/// Writes and finishes plugin extension `name` in one call.
void put_plugin(FileBuild& build, dstd::StringView name, std::uint32_t version,
                std::uint64_t params_hash, const GranuleList& granules,
                const dstd::Optional<dstd::String>& file);

/// As above, with views the build copies. Each payload is copied once, into the
/// stored value that carries the layout header.
using GranuleViews = dstd::Vector<std::pair<std::uint64_t, dstd::StringView>>;
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

/// Starts a build of a file the index holds, with the stat it was indexed
/// with, to add or replace extensions at query time.
Result<FileBuild> rebuild(const Index& index, dstd::StringView path);

/// Marks the extension failed for `params_hash`. The failure lands after the
/// publish of the finished extensions, not in the same record.
void fail_extension(FileBuild& build, store::IndexExtension ext,
                    std::uint64_t params_hash);

/// Commits the building batches the build holds so far, so their memory is
/// released, and publishes nothing. Later puts join the same building copies,
/// and commit() publishes all of them. The build must stay alive until the task
/// ends.
coro::CoroTask<Result<void>> stage(FileBuild& build);

// Writes that are not about one file. Each commits in one record, and its
// arguments must stay alive until the task ends.

/// Replaces the index-wide aggregation record `name` with `payload`.
coro::CoroTask<Result<void>> put_agg_meta(const Index& index,
                                          dstd::StringView name,
                                          dstd::StringView payload);
/// Deletes the index-wide aggregation record `name`. A missing record is not an
/// error.
coro::CoroTask<Result<void>> delete_agg_meta(const Index& index,
                                             dstd::StringView name);
/// Drops the copy of one built-in extension of every file in `paths`. A file
/// without a copy is skipped.
coro::CoroTask<Result<void>> remove_extension(
    const Index& index, store::IndexExtension ext,
    dstd::Span<const dstd::StringView> paths);

}  // namespace dftracer::utils::index

#endif  // DFTRACER_UTILS_INDEX_TABLES_H
