#include <dftracer/utils/core/common/checkpointer.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint_size.h>
#include <dftracer/utils/utilities/indexer/internal/common/gzip_checkpointer.h>
#include <dftracer/utils/utilities/indexer/internal/common/gzip_inflater.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>

#include <cstdio>

namespace dftracer::utils::utilities::indexer::internal::gzip {

// Import the SQL_SCHEMA from constants
extern const char *const &SQL_SCHEMA;

static void init_schema(const SqliteDatabase &db) {
    DFTRACER_UTILS_LOG_DEBUG("%s", "Initializing GZIP indexer schema");
    int rc = sqlite3_exec(db.get(), SQL_SCHEMA, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to initialize schema: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

static bool process_chunks(FILE *fp, const SqliteDatabase &db, int file_id,
                           std::uint64_t ckpt_size, std::uint64_t &total_lines,
                           std::uint64_t &total_uc_size) {
    GzipInflater inflater;
    if (!inflater.initialize(fp)) {
        return false;
    }

    std::uint64_t checkpoint_idx = 0;
    std::uint64_t current_uc_offset = 0;
    std::uint64_t line_count_in_chunk = 0;
    std::uint64_t first_line_in_chunk = total_lines;

    while (true) {
        std::size_t chunk_start_uc = current_uc_offset;
        std::size_t chunk_start_c = inflater.get_total_input_consumed();

        // Create checkpoint if we've processed enough data
        if (current_uc_offset > 0 && (current_uc_offset % ckpt_size) == 0) {
            GzipCheckpointer checkpointer(inflater, chunk_start_uc);
            if (checkpointer.create(chunk_start_c)) {
                std::vector<unsigned char> compressed_dict;
                if (checkpointer.compress(compressed_dict)) {
                    InsertCheckpointData checkpoint_data = {
                        checkpoint_idx++,
                        chunk_start_uc,
                        0,  // uc_size - will be updated later
                        0,  // c_size - will be updated later
                        chunk_start_c,
                        checkpointer.bits,
                        compressed_dict.data(),
                        compressed_dict.size(),
                        line_count_in_chunk,
                        first_line_in_chunk,
                        total_lines - 1};
                    insert_checkpoint_record(db, file_id, checkpoint_data);
                }
            }
        }

        GzipInflaterResult result;
        if (!inflater.read(fp, result)) {
            if (result.bytes_read == 0) {
                break;     // EOF
            }
            return false;  // Error
        }

        if (result.bytes_read == 0) {
            break;  // EOF
        }

        current_uc_offset += result.bytes_read;
        total_lines += result.lines_found;
        line_count_in_chunk += result.lines_found;
    }

    total_uc_size = current_uc_offset;
    return true;
}

static bool build_index(const SqliteDatabase &db, int file_id,
                        const std::string &gz_path, std::uint64_t ckpt_size) {
    FILE *fp = std::fopen(gz_path.c_str(), "rb");
    if (!fp) {
        return false;
    }

    std::uint64_t total_lines = 0;
    std::uint64_t total_uc_size = 0;

    bool success =
        process_chunks(fp, db, file_id, ckpt_size, total_lines, total_uc_size);
    std::fclose(fp);

    if (success) {
        insert_file_metadata_record(db, file_id, ckpt_size, total_lines,
                                    total_uc_size);
    }

    return success;
}

GzipIndexer::GzipIndexer(const std::string &gz_path_,
                         const std::string &idx_path_, std::uint64_t ckpt_size_,
                         bool force_rebuild_)
    : gz_path(gz_path_),
      gz_path_logical_path(get_logical_path(gz_path_)),
      idx_path(idx_path_),
      ckpt_size(ckpt_size_),
      force_rebuild(force_rebuild_),
      cached_is_valid(false),
      cached_file_id(-1),
      cached_max_bytes(0),
      cached_num_lines(0),
      cached_checkpoint_size(0) {
    if (gz_path.empty()) {
        throw IndexerError(IndexerError::Type::INVALID_ARGUMENT,
                           "gz_path must not be empty");
    }

    if (!fs::exists(gz_path)) {
        throw IndexerError(IndexerError::Type::FILE_ERROR,
                           "gz_path does not exist: " + gz_path);
    }

    if (ckpt_size == 0) {
        throw IndexerError(IndexerError::Type::INVALID_ARGUMENT,
                           "ckpt_size must be greater than 0");
    }

    open();
}

GzipIndexer::~GzipIndexer() {
    DFTRACER_UTILS_LOG_DEBUG("Destroying GZIP indexer for %s", gz_path.c_str());
    close();
}

GzipIndexer::GzipIndexer(GzipIndexer &&other) noexcept
    : gz_path(std::move(other.gz_path)),
      gz_path_logical_path(std::move(other.gz_path_logical_path)),
      idx_path(std::move(other.idx_path)),
      ckpt_size(other.ckpt_size),
      force_rebuild(other.force_rebuild),
      db(std::move(other.db)),
      cached_is_valid(other.cached_is_valid),
      cached_file_id(other.cached_file_id),
      cached_max_bytes(other.cached_max_bytes),
      cached_num_lines(other.cached_num_lines),
      cached_checkpoint_size(other.cached_checkpoint_size),
      cached_checkpoints(std::move(other.cached_checkpoints)) {}

GzipIndexer &GzipIndexer::operator=(GzipIndexer &&other) noexcept {
    if (this != &other) {
        gz_path = std::move(other.gz_path);
        gz_path_logical_path = std::move(other.gz_path_logical_path);
        idx_path = std::move(other.idx_path);
        ckpt_size = other.ckpt_size;
        force_rebuild = other.force_rebuild;
        db = std::move(other.db);
        cached_is_valid = other.cached_is_valid;
        cached_file_id = other.cached_file_id;
        cached_max_bytes = other.cached_max_bytes;
        cached_num_lines = other.cached_num_lines;
        cached_checkpoint_size = other.cached_checkpoint_size;
        cached_checkpoints = std::move(other.cached_checkpoints);
    }
    return *this;
}

void GzipIndexer::open() {
    if (!db.open(idx_path)) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to open database at " + idx_path);
    }
}

void GzipIndexer::close() {
    DFTRACER_UTILS_LOG_DEBUG("Closing GZIP indexer database for %s",
                             gz_path.c_str());
    db.close();
}

void GzipIndexer::build() const {
    if (!force_rebuild && !need_rebuild()) {
        return;
    }

    init_schema(db);

    int file_id = find_file_id(gz_path_logical_path);
    if (file_id != -1) {
        delete_file_record(db, file_id);
    }

    std::time_t mtime = get_file_modification_time(gz_path);
    auto hash = calculate_file_hash(gz_path);
    std::uint64_t bytes = file_size_bytes(gz_path);
    std::uint64_t final_ckpt_size =
        determine_checkpoint_size(ckpt_size, gz_path);

    insert_file_record(db, gz_path_logical_path, bytes, mtime, hash, file_id);

    if (!build_index(db, file_id, gz_path, final_ckpt_size)) {
        throw IndexerError(IndexerError::Type::BUILD_ERROR,
                           "Failed to build index for " + gz_path);
    }

    cached_is_valid = true;
    cached_file_id = file_id;
}

bool GzipIndexer::is_valid() const { return cached_is_valid; }

bool GzipIndexer::exists() const { return fs::exists(idx_path); }

bool GzipIndexer::need_rebuild() const {
    if (is_valid()) return false;
    if (!exists()) return true;

    // Only query schema if database exists - matches original behavior
    if (!query_schema_validity(db)) return true;

    std::uint64_t stored_hash;
    std::time_t stored_mtime;
    if (!query_stored_file_info(db, gz_path_logical_path, stored_hash,
                                stored_mtime)) {
        return true;
    }

    std::time_t current_mtime = get_file_modification_time(gz_path);
    std::uint64_t current_hash = calculate_file_hash(gz_path);

    return (stored_mtime != current_mtime) || (stored_hash != current_hash);
}

const std::string &GzipIndexer::get_idx_path() const { return idx_path; }

const std::string &GzipIndexer::get_archive_path() const { return gz_path; }

const std::string &GzipIndexer::get_gz_path() const { return gz_path; }

std::uint64_t GzipIndexer::get_max_bytes() const {
    if (cached_max_bytes == 0) {
        cached_max_bytes = query_max_bytes(db, gz_path_logical_path);
    }
    return cached_max_bytes;
}

std::uint64_t GzipIndexer::get_checkpoint_size() const {
    if (cached_checkpoint_size == 0) {
        int file_id = get_file_id();
        if (file_id != -1) {
            cached_checkpoint_size = query_checkpoint_size(db, file_id);
        }
    }
    return cached_checkpoint_size;
}

std::uint64_t GzipIndexer::get_num_lines() const {
    if (cached_num_lines == 0) {
        cached_num_lines = query_num_lines(db, gz_path_logical_path);
    }
    return cached_num_lines;
}

int GzipIndexer::get_file_id() const {
    if (cached_file_id == -1) {
        cached_file_id = query_file_id(db, gz_path_logical_path);
    }
    return cached_file_id;
}

int GzipIndexer::find_file_id(const std::string &path) const {
    return query_file_id(db, get_logical_path(path));
}

bool GzipIndexer::find_checkpoint(std::size_t target_offset,
                                  IndexerCheckpoint &checkpoint) const {
    int file_id = get_file_id();
    if (file_id == -1) return false;
    return query_checkpoint(db, target_offset, file_id, checkpoint);
}

std::vector<IndexerCheckpoint> GzipIndexer::get_checkpoints() const {
    if (cached_checkpoints.empty()) {
        int file_id = get_file_id();
        if (file_id != -1) {
            cached_checkpoints = query_checkpoints(db, file_id);
        }
    }
    return cached_checkpoints;
}

std::vector<IndexerCheckpoint> GzipIndexer::get_checkpoints_for_line_range(
    std::uint64_t start_line, std::uint64_t end_line) const {
    int file_id = get_file_id();
    if (file_id == -1) return {};
    return query_checkpoints_for_line_range(db, file_id, start_line, end_line);
}

}  // namespace dftracer::utils::utilities::indexer::internal::gzip
