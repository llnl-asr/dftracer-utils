#ifndef DFTRACER_UTILS_CORE_ROCKSDB_COLUMN_FAMILIES_H
#define DFTRACER_UTILS_CORE_ROCKSDB_COLUMN_FAMILIES_H

#include <array>
#include <string_view>

namespace dftracer::utils::rocksdb::cf {

inline constexpr std::string_view DEFAULT = "default";
inline constexpr std::string_view CHECKPOINTS = "checkpoints";
inline constexpr std::string_view METADATA = "metadata";
inline constexpr std::string_view CHUNK_BLOOM = "chunk_bloom";
inline constexpr std::string_view FILE_BLOOM = "file_bloom";
inline constexpr std::string_view CHUNK_STATS = "chunk_stats";
inline constexpr std::string_view DIMENSIONS = "dimensions";
inline constexpr std::string_view CHUNK_DIM_STATS = "chunk_dim_stats";
inline constexpr std::string_view FILE_SCALAR_STATS = "file_scalar_stats";
inline constexpr std::string_view FILE_CAT_COUNTS = "file_cat_counts";
inline constexpr std::string_view FILE_NAME_COUNTS = "file_name_counts";
inline constexpr std::string_view FILE_PID_TID_COUNTS = "file_pid_tid_counts";
inline constexpr std::string_view ROOT_SCALAR_STATS = "root_scalar_stats";
inline constexpr std::string_view ROOT_CAT_COUNTS = "root_cat_counts";
inline constexpr std::string_view ROOT_NAME_COUNTS = "root_name_counts";
inline constexpr std::string_view ROOT_PID_TID_COUNTS = "root_pid_tid_counts";
inline constexpr std::string_view NAME_DICTIONARY = "name_dictionary";
inline constexpr std::string_view NAME_FILE_POSTINGS = "name_file_postings";
inline constexpr std::string_view NAME_CHUNK_POSTINGS = "name_chunk_postings";
inline constexpr std::string_view MANIFEST = "manifest";
inline constexpr std::string_view PROVENANCE = "provenance";
inline constexpr std::string_view ARCHIVES = "archives";
inline constexpr std::string_view TAR_FILES = "tar_files";
inline constexpr std::string_view AGGREGATION = "aggregation";
inline constexpr std::string_view SYSTEM_METRICS = "system_metrics";
inline constexpr std::string_view HASH_TABLES = "hash_tables";
inline constexpr auto ALL =
    std::to_array<std::string_view>({DEFAULT,
                                     CHECKPOINTS,
                                     METADATA,
                                     CHUNK_BLOOM,
                                     FILE_BLOOM,
                                     CHUNK_STATS,
                                     DIMENSIONS,
                                     CHUNK_DIM_STATS,
                                     FILE_SCALAR_STATS,
                                     FILE_CAT_COUNTS,
                                     FILE_NAME_COUNTS,
                                     FILE_PID_TID_COUNTS,
                                     ROOT_SCALAR_STATS,
                                     ROOT_CAT_COUNTS,
                                     ROOT_NAME_COUNTS,
                                     ROOT_PID_TID_COUNTS,
                                     NAME_DICTIONARY,
                                     NAME_FILE_POSTINGS,
                                     NAME_CHUNK_POSTINGS,
                                     MANIFEST,
                                     PROVENANCE,
                                     ARCHIVES,
                                     TAR_FILES,
                                     AGGREGATION,
                                     SYSTEM_METRICS,
                                     HASH_TABLES});

}  // namespace dftracer::utils::rocksdb::cf

#endif  // DFTRACER_UTILS_CORE_ROCKSDB_COLUMN_FAMILIES_H
