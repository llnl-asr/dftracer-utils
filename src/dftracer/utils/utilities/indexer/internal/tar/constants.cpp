#include <dftracer/utils/core/common/constants.h>

namespace dftracer::utils::utilities::indexer::internal::tar {

const char* tar_sql_schema = R"(
    CREATE TABLE IF NOT EXISTS files (
      id INTEGER PRIMARY KEY,
      logical_name TEXT UNIQUE NOT NULL,
      byte_size INTEGER NOT NULL,
      mtime_unix INTEGER NOT NULL,
      hash INTEGER NOT NULL
    );

    CREATE TABLE IF NOT EXISTS tar_archives (
      id INTEGER PRIMARY KEY,
      file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
      archive_name TEXT NOT NULL,
      uncompressed_size INTEGER NOT NULL DEFAULT 0,
      total_files INTEGER NOT NULL DEFAULT 0,
      UNIQUE(file_id)
    );

    CREATE TABLE IF NOT EXISTS tar_files (
      id INTEGER PRIMARY KEY,
      archive_id INTEGER NOT NULL REFERENCES tar_archives(id) ON DELETE CASCADE,
      file_name TEXT NOT NULL,
      file_size INTEGER NOT NULL,
      file_mtime INTEGER NOT NULL,
      typeflag CHAR(1) NOT NULL DEFAULT '0',
      data_offset INTEGER NOT NULL,
      uncompressed_offset INTEGER NOT NULL
    );

    CREATE TABLE IF NOT EXISTS tar_gzip_checkpoints (
      id INTEGER PRIMARY KEY,
      archive_id INTEGER NOT NULL REFERENCES tar_archives(id) ON DELETE CASCADE,
      checkpoint_idx INTEGER NOT NULL,
      uc_offset INTEGER NOT NULL,
      uc_size INTEGER NOT NULL,
      c_offset INTEGER NOT NULL,
      c_size INTEGER NOT NULL,
      bits INTEGER NOT NULL,
      dict_compressed BLOB NOT NULL,
      num_lines INTEGER NOT NULL,
      first_line_num INTEGER NOT NULL DEFAULT 0,
      last_line_num INTEGER NOT NULL DEFAULT 0,
      tar_files_count INTEGER NOT NULL DEFAULT 0
    );

    CREATE INDEX IF NOT EXISTS tar_files_archive_idx ON tar_files(archive_id, file_name);
    CREATE INDEX IF NOT EXISTS tar_files_offset_idx ON tar_files(archive_id, uncompressed_offset);
    CREATE INDEX IF NOT EXISTS tar_checkpoints_archive_idx ON tar_gzip_checkpoints(archive_id, checkpoint_idx);
    CREATE INDEX IF NOT EXISTS tar_checkpoints_uc_offset_idx ON tar_gzip_checkpoints(archive_id, uc_offset);
    CREATE INDEX IF NOT EXISTS tar_checkpoints_line_range_idx ON tar_gzip_checkpoints(archive_id, first_line_num, last_line_num);

    CREATE TABLE IF NOT EXISTS metadata (
      archive_id INTEGER NOT NULL REFERENCES tar_archives(id) ON DELETE CASCADE,
      checkpoint_size INTEGER NOT NULL,
      total_lines INTEGER NOT NULL DEFAULT 0,
      total_uc_size INTEGER NOT NULL DEFAULT 0,
      PRIMARY KEY(archive_id)
    );
  )";

const char* const& SQL_SCHEMA = tar_sql_schema;

}  // namespace dftracer::utils::utilities::indexer::internal::tar
