#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_GZIP_QUERIES_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_GZIP_QUERIES_H

#include <dftracer/utils/utilities/indexer/internal/checkpoint.h>
#include <dftracer/utils/utilities/indexer/internal/sqlite/database.h>

#include <cstddef>
#include <cstdint>
#include <ctime>

namespace dftracer::utils::utilities::indexer::internal::gzip {

void insert_file_record(const SqliteDatabase &db,
                        const std::string &gz_path_logical_path,
                        std::size_t bytes, std::time_t file_mtime,
                        std::uint64_t file_hash, int &db_file_id);
void insert_file_metadata_record(const SqliteDatabase &db, int file_id,
                                 std::size_t ckpt_size,
                                 std::uint64_t total_lines,
                                 std::uint64_t total_uc_size);
bool query_stored_file_info(const SqliteDatabase &db,
                            const std::string &gz_path,
                            std::uint64_t &stored_hash,
                            std::time_t &stored_mtime);

struct InsertCheckpointData {
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
};
void insert_checkpoint_record(const SqliteDatabase &db, int file_id,
                              const InsertCheckpointData &data);

bool query_schema_validity(const SqliteDatabase &db);
bool delete_file_record(const SqliteDatabase &db, int file_id);
std::uint64_t query_max_bytes(const SqliteDatabase &db,
                              const std::string &gz_path_logical_path);
std::uint64_t query_num_lines(const SqliteDatabase &db,
                              const std::string &gz_path_logical_path);
int query_file_id(const SqliteDatabase &db,
                  const std::string &gz_path_logical_path);
bool query_checkpoint(const SqliteDatabase &db, std::size_t target_offset,
                      int file_id, IndexerCheckpoint &checkpoint);
std::vector<IndexerCheckpoint> query_checkpoints(const SqliteDatabase &db,
                                                 int file_id);
std::vector<IndexerCheckpoint> query_checkpoints_for_line_range(
    const SqliteDatabase &db, int file_id, std::uint64_t start_line,
    std::uint64_t end_line);
std::uint64_t query_checkpoint_size(const SqliteDatabase &db, int file_id);

}  // namespace dftracer::utils::utilities::indexer::internal::gzip

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_GZIP_QUERIES_H
