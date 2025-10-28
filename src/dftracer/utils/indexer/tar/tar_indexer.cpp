#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/indexer/common/gzip_inflater.h>
#include <dftracer/utils/indexer/error.h>
#include <dftracer/utils/indexer/helpers.h>
#include <dftracer/utils/indexer/sqlite/statement.h>
#include <dftracer/utils/indexer/tar/queries/queries.h>
#include <dftracer/utils/indexer/tar/tar_indexer.h>
#include <dftracer/utils/indexer/tar/tar_parser.h>

#include <chrono>
#include <fstream>
#include <sstream>

namespace dftracer::utils::tar_indexer {

// Import the SQL_SCHEMA from constants
extern const char *const &SQL_SCHEMA;

// Forward declare helper functions
static bool build_tar_index(const SqliteDatabase &db, int archive_id,
                            const std::string &tar_gz_path,
                            std::uint64_t ckpt_size);
static void init_tar_schema(const SqliteDatabase &db);

TarIndexer::TarIndexer(const std::string &tar_gz_file_path,
                       const std::string &index_path,
                       std::uint64_t checkpoint_size, bool rebuild_force)
    : tar_gz_path(tar_gz_file_path),
      idx_path(index_path),
      ckpt_size(checkpoint_size),
      force_rebuild(rebuild_force),
      cached_is_valid(false),
      cached_archive_id(-1),
      cached_max_bytes(0),
      cached_num_lines(0),
      cached_num_files(0),
      cached_checkpoint_size(0) {
    open();
}

TarIndexer::~TarIndexer() {
    try {
        DFTRACER_UTILS_LOG_DEBUG("Destroying TarIndexer for %s",
                                 tar_gz_path.c_str());
        if (db.is_open()) {
            close();
        }
        DFTRACER_UTILS_LOG_DEBUG("TarIndexer destruction completed for %s",
                                 tar_gz_path.c_str());
    } catch (const std::exception &e) {
        DFTRACER_UTILS_LOG_ERROR("Error during TarIndexer destruction: %s",
                                 e.what());
    } catch (...) {
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "Unknown error during TarIndexer destruction");
    }
}

TarIndexer::TarIndexer(TarIndexer &&other) noexcept
    : tar_gz_path(std::move(other.tar_gz_path)),
      tar_gz_path_logical_path(std::move(other.tar_gz_path_logical_path)),
      idx_path(std::move(other.idx_path)),
      ckpt_size(other.ckpt_size),
      force_rebuild(other.force_rebuild),
      db(std::move(other.db)),
      cached_is_valid(other.cached_is_valid),
      cached_archive_id(other.cached_archive_id),
      cached_max_bytes(other.cached_max_bytes),
      cached_num_lines(other.cached_num_lines),
      cached_num_files(other.cached_num_files),
      cached_checkpoint_size(other.cached_checkpoint_size),
      cached_archive_name(std::move(other.cached_archive_name)),
      cached_checkpoints(std::move(other.cached_checkpoints)) {}

TarIndexer &TarIndexer::operator=(TarIndexer &&other) noexcept {
    if (this != &other) {
        tar_gz_path = std::move(other.tar_gz_path);
        tar_gz_path_logical_path = std::move(other.tar_gz_path_logical_path);
        idx_path = std::move(other.idx_path);
        ckpt_size = other.ckpt_size;
        force_rebuild = other.force_rebuild;
        db = std::move(other.db);
        cached_is_valid = other.cached_is_valid;
        cached_archive_id = other.cached_archive_id;
        cached_max_bytes = other.cached_max_bytes;
        cached_num_lines = other.cached_num_lines;
        cached_num_files = other.cached_num_files;
        cached_checkpoint_size = other.cached_checkpoint_size;
        cached_archive_name = std::move(other.cached_archive_name);
        cached_checkpoints = std::move(other.cached_checkpoints);
    }
    return *this;
}

void TarIndexer::open() {
    DFTRACER_UTILS_LOG_DEBUG("Opening TAR indexer database: %s",
                             idx_path.c_str());

    tar_gz_path_logical_path = get_logical_path(tar_gz_path);

    if (!db.open(idx_path)) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to open database at " + idx_path);
    }
}

void TarIndexer::close() {
    db.close();
    // Reset all cache
    cached_is_valid = false;
    cached_archive_id = -1;
    cached_max_bytes = 0;
    cached_num_lines = 0;
    cached_num_files = 0;
    cached_checkpoint_size = 0;
    cached_archive_name.clear();
    cached_checkpoints.clear();
}

void TarIndexer::build() const {
    if (!force_rebuild && !need_rebuild()) {
        return;
    }

    init_tar_schema(db);

    int archive_id = find_archive_id(tar_gz_path_logical_path);
    if (archive_id != -1) {
        delete_archive_record(db, archive_id);
    }

    printf("Get modifcation time for %s\n", tar_gz_path.c_str());
    std::time_t mtime = get_file_modification_time(tar_gz_path);
    printf("Calculate hash for %s\n", tar_gz_path.c_str());
    auto hash = calculate_file_hash(tar_gz_path);
    printf("Get size for %s\n", tar_gz_path.c_str());
    std::uint64_t bytes = file_size_bytes(tar_gz_path);
    // TODO: use determine_checkpoint_size like GZIP
    std::uint64_t final_ckpt_size = ckpt_size;

    int file_id;
    insert_file_record(db, tar_gz_path_logical_path, bytes, mtime, hash,
                       file_id);

    std::string archive_name = fs::path(tar_gz_path).filename().string();

    // Will update sizes later
    insert_archive_record(db, file_id, archive_name, 0, 0, archive_id);

    if (!build_tar_index(db, archive_id, tar_gz_path, final_ckpt_size)) {
        throw IndexerError(IndexerError::Type::BUILD_ERROR,
                           "Failed to build TAR index for " + tar_gz_path);
    }

    // Reset cache to force refresh
    cached_is_valid = true;
    cached_archive_id = archive_id;
    cached_max_bytes = 0;
    cached_num_lines = 0;
    cached_num_files = 0;
    cached_checkpoint_size = final_ckpt_size;
    cached_archive_name.clear();
    cached_checkpoints.clear();
}

bool TarIndexer::need_rebuild() const {
    if (force_rebuild) {
        return true;
    }

    try {
        // Check if index exists and has valid schema
        if (!query_schema_validity(db)) {
            return true;
        }

        // Check if file has been modified since last index
        std::uint64_t stored_hash;
        std::time_t stored_mtime;
        if (query_stored_file_info(db, tar_gz_path_logical_path, stored_hash,
                                   stored_mtime)) {
            std::uint64_t current_hash = calculate_file_hash(tar_gz_path);
            std::time_t current_mtime = get_file_modification_time(tar_gz_path);

            return (stored_hash != current_hash ||
                    stored_mtime != current_mtime);
        }
    } catch (...) {
        return true;
    }

    return true;  // If we can't determine, rebuild to be safe
}

bool TarIndexer::is_valid() const {
    if (!cached_is_valid) {
        try {
            bool schema_valid = query_schema_validity(db);
            bool has_data = (find_archive_id(tar_gz_path_logical_path) != -1);
            cached_is_valid = schema_valid && has_data;
        } catch (...) {
            cached_is_valid = false;
        }
    }
    return cached_is_valid;
}

bool TarIndexer::exists() const {
    return fs::exists(idx_path) && fs::is_regular_file(idx_path);
}

const std::string &TarIndexer::get_idx_path() const { return idx_path; }

const std::string &TarIndexer::get_archive_path() const { return tar_gz_path; }

const std::string &TarIndexer::get_tar_gz_path() const { return tar_gz_path; }

std::uint64_t TarIndexer::get_checkpoint_size() const { return ckpt_size; }

std::uint64_t TarIndexer::get_max_bytes() const {
    if (cached_max_bytes == 0) {
        cached_max_bytes = query_max_bytes(db, tar_gz_path_logical_path);
    }
    return cached_max_bytes;
}

std::uint64_t TarIndexer::get_num_lines() const {
    if (cached_num_lines == 0) {
        cached_num_lines = query_num_lines(db, tar_gz_path_logical_path);
    }
    return cached_num_lines;
}

std::uint64_t TarIndexer::get_num_files() const {
    if (cached_num_files == 0) {
        cached_num_files = query_num_files(db, tar_gz_path_logical_path);
    }
    return cached_num_files;
}

std::string TarIndexer::get_archive_name() const {
    if (cached_archive_name.empty()) {
        cached_archive_name = query_archive_name(db, tar_gz_path_logical_path);
        if (cached_archive_name.empty()) {
            cached_archive_name = fs::path(tar_gz_path).filename().string();
        }
    }
    return cached_archive_name;
}

int TarIndexer::get_archive_id() const {
    if (cached_archive_id == -1) {
        cached_archive_id = find_archive_id(tar_gz_path_logical_path);
    }
    return cached_archive_id;
}

int TarIndexer::find_archive_id(const std::string &tar_gz_file_path) const {
    return query_archive_id(db, tar_gz_file_path);
}

bool TarIndexer::find_checkpoint(std::size_t target_offset,
                                 IndexerCheckpoint &checkpoint) const {
    int archive_id = get_archive_id();
    if (archive_id == -1) return false;
    return query_tar_checkpoint(db, target_offset, archive_id, checkpoint);
}

std::vector<IndexerCheckpoint> TarIndexer::get_checkpoints() const {
    if (cached_checkpoints.empty()) {
        int archive_id = get_archive_id();
        if (archive_id != -1) {
            cached_checkpoints = query_tar_checkpoints(db, archive_id);
        }
    }
    return cached_checkpoints;
}

std::vector<IndexerCheckpoint> TarIndexer::get_checkpoints_for_line_range(
    std::uint64_t start_line, std::uint64_t end_line) const {
    int archive_id = get_archive_id();
    if (archive_id == -1) return {};
    return query_tar_checkpoints_for_line_range(db, archive_id, start_line,
                                                end_line);
}

std::vector<TarIndexer::TarFileInfo> TarIndexer::list_files() const {
    int archive_id = get_archive_id();
    if (archive_id == -1) return {};

    auto tar_files = query_tar_files(db, archive_id);
    std::vector<TarFileInfo> result;
    result.reserve(tar_files.size());

    for (const auto &tf : tar_files) {
        result.emplace_back(
            TarFileInfo{tf.file_name, tf.file_size, tf.file_mtime, tf.typeflag,
                        tf.data_offset, tf.uncompressed_offset});
    }

    return result;
}

bool TarIndexer::find_file(const std::string &file_name,
                           TarFileInfo &file_info) const {
    int archive_id = get_archive_id();
    if (archive_id == -1) return false;

    return query_tar_file(db, archive_id, file_name, file_info);
}

std::vector<TarIndexer::TarFileInfo> TarIndexer::find_files_in_range(
    std::uint64_t start_offset, std::uint64_t end_offset) const {
    int archive_id = get_archive_id();
    if (archive_id == -1) return {};

    auto tar_files =
        query_tar_files_in_range(db, archive_id, start_offset, end_offset);
    std::vector<TarFileInfo> result;
    result.reserve(tar_files.size());

    for (const auto &tf : tar_files) {
        result.emplace_back(
            TarFileInfo{tf.file_name, tf.file_size, tf.file_mtime, tf.typeflag,
                        tf.data_offset, tf.uncompressed_offset});
    }

    return result;
}

// Include the helper functions from the impl file
static void init_tar_schema(const SqliteDatabase &db) {
    DFTRACER_UTILS_LOG_DEBUG("%s", "Initializing TAR indexer schema");
    int rc = sqlite3_exec(db.get(), SQL_SCHEMA, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to initialize TAR schema: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

static bool build_tar_index(const SqliteDatabase &db, int archive_id,
                            const std::string &tar_gz_path,
                            std::uint64_t ckpt_size) {
    FILE *fp = std::fopen(tar_gz_path.c_str(), "rb");
    if (!fp) {
        return false;
    }

    GzipInflater inflater;
    if (!inflater.initialize(fp)) {
        std::fclose(fp);
        return false;
    }

    std::uint64_t total_lines = 0;
    std::uint64_t total_uc_size = 0;
    std::uint64_t current_uc_offset = 0;

    // Parse TAR format and extract file entries
    tar_parser::TarParser parser;
    std::vector<unsigned char> accumulated_data;
    accumulated_data.reserve(1024 * 1024);  // Pre-allocate 1MB

    while (true) {
        // std::size_t chunk_start_uc = current_uc_offset;
        // std::size_t chunk_start_c = inflater.get_total_input_consumed();

        GzipInflaterResult result;
        if (!inflater.read(fp, result)) {
            if (result.bytes_read == 0) {
                break;     // EOF
            }
            std::fclose(fp);
            return false;  // Error
        }

        if (result.bytes_read == 0) {
            break;  // EOF
        }

        // Accumulate data for TAR parsing
        accumulated_data.insert(accumulated_data.end(), inflater.out_buffer,
                                inflater.out_buffer + result.bytes_read);

        current_uc_offset += result.bytes_read;
        total_lines += result.lines_found;
    }

    // Parse TAR entries from accumulated data
    std::vector<tar_parser::TarFileEntry> tar_entries;
    if (!parser.parse_headers(accumulated_data.data(), accumulated_data.size(),
                              0, tar_entries)) {
        DFTRACER_UTILS_LOG_DEBUG(
            "%s", "Failed to parse TAR headers from accumulated data");
        // Continue anyway - might be a malformed TAR or not actually TAR.GZ
    }

    // Insert TAR file entries into database
    for (const auto &entry : tar_entries) {
        if (entry.is_regular_file()) {
            InsertTarFileData file_data;
            file_data.file_name = entry.name;
            file_data.file_size = entry.size;
            file_data.file_mtime = entry.mtime;
            file_data.typeflag = entry.typeflag;
            file_data.data_offset = entry.data_offset;
            file_data.uncompressed_offset = entry.uncompressed_offset;

            insert_tar_file_record(db, archive_id, file_data);
        }
    }

    DFTRACER_UTILS_LOG_DEBUG("Parsed %zu TAR file entries", tar_entries.size());

    total_uc_size = current_uc_offset;

    // Insert metadata record
    insert_archive_metadata_record(db, archive_id, ckpt_size, total_lines,
                                   total_uc_size);

    std::fclose(fp);
    return true;
}

}  // namespace dftracer::utils::tar_indexer
