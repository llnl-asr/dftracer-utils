#include <dftracer/utils/core/common/constants.h>

namespace dftracer::utils::gzip_indexer {

const char* gzip_sql_schema = R"(
    CREATE TABLE IF NOT EXISTS files (
      id INTEGER PRIMARY KEY,
      logical_name TEXT UNIQUE NOT NULL,
      byte_size INTEGER NOT NULL,
      mtime_unix INTEGER NOT NULL,
      hash INTEGER NOT NULL
    );

    CREATE TABLE IF NOT EXISTS checkpoints (
      id INTEGER PRIMARY KEY,
      file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
      checkpoint_idx INTEGER NOT NULL,
      uc_offset INTEGER NOT NULL,
      uc_size INTEGER NOT NULL,
      c_offset INTEGER NOT NULL,
      c_size INTEGER NOT NULL,
      bits INTEGER NOT NULL,
      dict_compressed BLOB NOT NULL,
      num_lines INTEGER NOT NULL,
      first_line_num INTEGER NOT NULL DEFAULT 0,
      last_line_num INTEGER NOT NULL DEFAULT 0
    );

    CREATE INDEX IF NOT EXISTS checkpoints_file_idx ON checkpoints(file_id, checkpoint_idx);
    CREATE INDEX IF NOT EXISTS checkpoints_file_uc_off_idx ON checkpoints(file_id, uc_offset);
    CREATE INDEX IF NOT EXISTS checkpoints_line_range_idx ON checkpoints(file_id, first_line_num, last_line_num);

    CREATE TABLE IF NOT EXISTS metadata (
      file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
      checkpoint_size INTEGER NOT NULL,
      total_lines INTEGER NOT NULL DEFAULT 0,
      total_uc_size INTEGER NOT NULL DEFAULT 0,
      PRIMARY KEY(file_id)
    );
  )";

const char* const& SQL_SCHEMA = gzip_sql_schema;

}  // namespace dftracer::utils::gzip_indexer
