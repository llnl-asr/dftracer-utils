#ifndef DFTRACER_UTILS_INDEX_STORE_INDEX_WRITE_H
#define DFTRACER_UTILS_INDEX_STORE_INDEX_WRITE_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/gzip/gzip_member_record.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/layout.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store {

/// One atomic write to an index database: a RocksDB batch or a set of SSTs
/// ingested in one call. Nothing is visible until the owner commits it.
class IndexWrite {
   public:
    virtual ~IndexWrite() = default;
    virtual void put(layout::Family family, std::string_view key,
                     std::string_view value) = 0;
    /// Only for the aggregation and system_metrics families.
    virtual void merge(layout::Family family, std::string_view key,
                       std::string_view operand) = 0;
    virtual void delete_range(layout::Family family, std::string_view begin,
                              std::string_view end) = 0;
};

/// Record writers. Each value gets the layout header of its (ext, kind).
namespace records {

/// Removes every key `file_id` owns for `ext`, its manifest entry included.
void clear_file(IndexWrite& w, layout::Ext ext, int file_id);

/// Marks `ext` built for `file_id` with the current code version. Written in
/// the same IndexWrite as the data, it is the commit point of that data.
void put_manifest(IndexWrite& w, int file_id, layout::Ext ext,
                  std::uint64_t params_hash,
                  layout::ExtStatus status = layout::ExtStatus::READY);

/// Marks plugin extension `name` built for `file_id` at the plugin's
/// `version`; its data is keyed by `name` as a path of Ext::PLUGIN.
void put_plugin_manifest(IndexWrite& w, int file_id, std::string_view name,
                         std::uint32_t version, std::uint64_t params_hash,
                         layout::ExtStatus status = layout::ExtStatus::READY);

/// Removes plugin extension `name`'s data and manifest entry for `file_id`.
void clear_plugin(IndexWrite& w, int file_id, std::string_view name);

void put_file_record(IndexWrite& w, std::string_view logical_path,
                     const layout::FileRecord& record);

void put_file_metadata(IndexWrite& w, int file_id,
                       std::uint64_t checkpoint_size, std::uint64_t total_lines,
                       std::uint64_t total_uc_size, bool truncated);
void put_gzip_member(IndexWrite& w, int file_id,
                     const index::gzip::GzipMemberRecord& member);

/// Path-keyed data of the pruning extensions (zonemap, bloom, counts,
/// postings): the path joins the file's indexed paths.
void put_path(IndexWrite& w, layout::Ext ext, int file_id,
              std::string_view path);
/// One granule's payload for `path`.
void put_path_granule(IndexWrite& w, layout::Ext ext, int file_id,
                      std::string_view path, std::uint64_t granule,
                      std::string_view payload);
/// The whole file's payload for `path` (bloom).
void put_path_file(IndexWrite& w, layout::Ext ext, int file_id,
                   std::string_view path, std::string_view payload);
/// `value_hash` of `path` occurs in the file, and in `granule`.
void put_posting(IndexWrite& w, int file_id, std::string_view path,
                 std::uint64_t value_hash);
void put_posting_granule(IndexWrite& w, int file_id, std::string_view path,
                         std::uint64_t value_hash, std::uint64_t granule);

/// dft.stats records.
void put_chunk_statistics(IndexWrite& w, int file_id,
                          std::uint64_t checkpoint_idx,
                          const index::schemas::dft::ChunkStatistics& stats);
void put_chunk_metadata(IndexWrite& w, int file_id,
                        std::uint64_t checkpoint_idx,
                        const ChunkMetadata& metadata);
void put_file_scalar_stats(IndexWrite& w, int file_id,
                           const index::schemas::dft::ChunkStatistics& stats,
                           std::uint64_t num_chunks);
void put_file_pid_tid_counts(IndexWrite& w, int file_id,
                             const StringViewMap<std::uint64_t>& counts);
/// Range-deletes every record of `path` of a path-keyed extension for the
/// file, in every kind keyed by path.
void clear_path(IndexWrite& w, layout::Ext ext, int file_id,
                std::string_view path);

/// The schema the file was decoded with, and its manifest entry.
void put_profile(IndexWrite& w, int file_id, std::string_view profile_id,
                 std::uint64_t params_hash);

/// One path of the file's catalog.
void put_catalog_path(IndexWrite& w, int file_id, std::string_view path,
                      const PathStat& stat);

/// Index-wide dictionary row `key` of `dict`, with a reverse entry per field
/// value.
void put_dict_row(
    IndexWrite& w, std::string_view dict, std::string_view key,
    const std::vector<std::pair<std::string, std::string>>& fields);

}  // namespace records

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_INDEX_WRITE_H
