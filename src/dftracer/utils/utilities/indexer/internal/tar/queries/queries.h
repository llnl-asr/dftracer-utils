#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_TAR_QUERIES_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_TAR_QUERIES_H

#include <dftracer/utils/utilities/indexer/internal/checkpoint.h>
#include <dftracer/utils/utilities/indexer/internal/sqlite/database.h>
#include <dftracer/utils/utilities/indexer/internal/tar/tar_indexer.h>

#include <cstddef>
#include <cstdint>
#include <ctime>

namespace dftracer::utils::utilities::indexer::internal::tar {

// File and archive management
void insert_file_record(const SqliteDatabase &db,
                        const std::string &tar_gz_path_logical_path,
                        std::size_t bytes, std::time_t file_mtime,
                        std::uint64_t file_sha256, int &db_file_id);

void insert_archive_record(const SqliteDatabase &db, int file_id,
                           const std::string &archive_name,
                           std::uint64_t uncompressed_size,
                           std::uint64_t total_files, int &archive_id);

void insert_archive_metadata_record(const SqliteDatabase &db, int archive_id,
                                    std::size_t ckpt_size,
                                    std::uint64_t total_lines,
                                    std::uint64_t total_uc_size);

bool query_stored_file_info(const SqliteDatabase &db,
                            const std::string &tar_gz_path,
                            std::uint64_t &stored_hash,
                            std::time_t &stored_mtime);

// TAR file entries
struct InsertTarFileData {
    std::string file_name;
    std::uint64_t file_size;
    std::uint64_t file_mtime;
    char typeflag;
    std::uint64_t data_offset;
    std::uint64_t uncompressed_offset;
};

void insert_tar_file_record(const SqliteDatabase &db, int archive_id,
                            const InsertTarFileData &data);

std::vector<TarIndexer::TarFileInfo> query_tar_files(const SqliteDatabase &db,
                                                     int archive_id);

bool query_tar_file(const SqliteDatabase &db, int archive_id,
                    const std::string &file_name,
                    TarIndexer::TarFileInfo &file_info);

std::vector<TarIndexer::TarFileInfo> query_tar_files_in_range(
    const SqliteDatabase &db, int archive_id, std::uint64_t start_offset,
    std::uint64_t end_offset);

// GZIP checkpoints for TAR archives
struct InsertTarCheckpointData {
    std::uint64_t idx;
    std::uint64_t uc_offset;
    std::uint64_t uc_size;
    std::uint64_t c_size;
    std::uint64_t c_offset;
    int bits;
    const void *compressed_dict;
    std::size_t compressed_dict_size;
    std::uint64_t num_lines;
    std::uint64_t first_line_num;
    std::uint64_t last_line_num;
    std::uint64_t tar_files_count;
};

void insert_tar_checkpoint_record(const SqliteDatabase &db, int archive_id,
                                  const InsertTarCheckpointData &data);

// Database queries
bool query_schema_validity(const SqliteDatabase &db);
bool delete_archive_record(const SqliteDatabase &db, int archive_id);
std::uint64_t query_max_bytes(const SqliteDatabase &db,
                              const std::string &tar_gz_path_logical_path);
std::uint64_t query_num_lines(const SqliteDatabase &db,
                              const std::string &tar_gz_path_logical_path);
std::uint64_t query_num_files(const SqliteDatabase &db,
                              const std::string &tar_gz_path_logical_path);
std::string query_archive_name(const SqliteDatabase &db,
                               const std::string &tar_gz_path_logical_path);
int query_archive_id(const SqliteDatabase &db,
                     const std::string &tar_gz_path_logical_path);

bool query_tar_checkpoint(const SqliteDatabase &db, std::size_t target_offset,
                          int archive_id, IndexerCheckpoint &checkpoint);
std::vector<IndexerCheckpoint> query_tar_checkpoints(const SqliteDatabase &db,
                                                     int archive_id);
std::vector<IndexerCheckpoint> query_tar_checkpoints_for_line_range(
    const SqliteDatabase &db, int archive_id, std::uint64_t start_line,
    std::uint64_t end_line);
std::uint64_t query_checkpoint_size(const SqliteDatabase &db, int archive_id);

}  // namespace dftracer::utils::utilities::indexer::internal::tar

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_TAR_QUERIES_H
