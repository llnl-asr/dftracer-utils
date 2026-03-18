#include <dftracer/utils/core/common/checkpointer.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/sqlite/async.h>
#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/indexer/index_visitor.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint_size.h>
#include <dftracer/utils/utilities/indexer/internal/common/gzip_checkpointer.h>
#include <dftracer/utils/utilities/indexer/internal/common/gzip_inflater.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/queries/queries.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <string>

namespace dftracer::utils::utilities::indexer::internal::gzip {

using dftracer::utils::sqlite::SqliteStmt;

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

static dftracer::utils::coro::CoroTask<bool> process_chunks(
    int fd, const SqliteDatabase &db, int file_id, std::uint64_t ckpt_size,
    std::uint64_t &total_lines, std::uint64_t &total_uc_size,
    std::uint64_t &tail_line_count, const Indexer::VisitorList &visitors) {
    GzipInflater inflater;
    off_t offset = 0;
    if (!(co_await inflater.initialize(fd))) {
        co_return false;
    }

    std::uint64_t checkpoint_idx = 0;
    std::uint64_t current_uc_offset = 0;
    std::uint64_t next_ckpt_offset = ckpt_size;
    std::uint64_t line_count_in_chunk = 0;
    std::uint64_t first_line_in_chunk = total_lines + 1;  // 1-based

    // Partial-line accumulator for visitor dispatch.
    std::string line_buf;
    const bool has_visitors = !visitors.empty();

    while (true) {
        GzipInflaterResult result;
        if (!(co_await inflater.read(fd, offset, result))) {
            if (result.bytes_read == 0) {
                break;        // EOF
            }
            co_return false;  // Error
        }

        if (result.bytes_read == 0) {
            break;  // EOF
        }

        current_uc_offset += result.bytes_read;
        total_lines += result.lines_found;
        line_count_in_chunk += result.lines_found;

        if (has_visitors) {
            const auto *data = inflater.out_buffer;
            const std::size_t n = result.bytes_read;
            std::size_t seg_start = 0;
            for (std::size_t i = 0; i < n; ++i) {
                if (data[i] == '\n') {
                    line_buf.append(
                        reinterpret_cast<const char *>(data + seg_start),
                        i - seg_start);
                    std::string_view line_sv(line_buf);
                    for (auto &v : visitors) {
                        v.get().on_line(line_sv, checkpoint_idx);
                    }
                    line_buf.clear();
                    seg_start = i + 1;
                }
            }
            // Accumulate any trailing bytes that don't end with '\n'.
            if (seg_start < n) {
                line_buf.append(
                    reinterpret_cast<const char *>(data + seg_start),
                    n - seg_start);
            }
        }

        // Create checkpoint when we cross a boundary and are at a deflate
        // block boundary (read() now stops at block boundaries via Z_BLOCK).
        if (current_uc_offset >= next_ckpt_offset && result.at_block_boundary) {
            std::size_t chunk_start_uc = current_uc_offset;
            std::size_t chunk_start_c = inflater.get_total_input_consumed();

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
                        total_lines};  // 1-based: last line = total_lines
                    co_await dftracer::utils::sqlite::run([&] {
                        insert_checkpoint_record(db, file_id, checkpoint_data);
                    });

                    if (has_visitors) {
                        for (auto &v : visitors) {
                            v.get().on_checkpoint(checkpoint_idx - 1);
                        }
                    }

                    // Reset chunk counters for next chunk
                    line_count_in_chunk = 0;
                    first_line_in_chunk = total_lines + 1;  // 1-based
                    next_ckpt_offset = current_uc_offset + ckpt_size;
                }
            }
        }
    }

    total_uc_size = current_uc_offset;
    tail_line_count = line_count_in_chunk;
    co_return true;
}

// After all checkpoints are inserted, compute uc_size / c_size for each
// and extend the last checkpoint's line range to cover the tail data.
static void finalize_checkpoints(const SqliteDatabase &db, int file_id,
                                 std::uint64_t total_uc_size,
                                 std::uint64_t total_lines,
                                 std::uint64_t tail_line_count) {
    // 1. Set uc_size = distance to next checkpoint (or total_uc_size for last).
    {
        SqliteStmt stmt(
            db,
            "UPDATE checkpoints SET "
            "uc_size = COALESCE("
            "  (SELECT c2.uc_offset FROM checkpoints c2 "
            "   WHERE c2.file_id = checkpoints.file_id "
            "   AND c2.checkpoint_idx = checkpoints.checkpoint_idx + 1), ?"
            ") - uc_offset, "
            "c_size = COALESCE("
            "  (SELECT c2.c_offset FROM checkpoints c2 "
            "   WHERE c2.file_id = checkpoints.file_id "
            "   AND c2.checkpoint_idx = checkpoints.checkpoint_idx + 1), "
            "  c_offset"
            ") - c_offset "
            "WHERE file_id = ?");
        stmt.bind_int64(1, static_cast<int64_t>(total_uc_size));
        stmt.bind_int(2, file_id);
        sqlite3_step(stmt.get());
    }

    // 2. Extend the last checkpoint's line range to cover the tail data
    //    (lines after the last block boundary that didn't trigger a new
    //    checkpoint).
    if (tail_line_count > 0 && total_lines > 0) {
        SqliteStmt stmt(
            db,
            "UPDATE checkpoints SET last_line_num = ?, "
            "num_lines = num_lines + ? "
            "WHERE file_id = ? AND checkpoint_idx = "
            "(SELECT MAX(checkpoint_idx) FROM checkpoints WHERE file_id = ?)");
        stmt.bind_int64(1, static_cast<int64_t>(total_lines));
        stmt.bind_int64(2, static_cast<int64_t>(tail_line_count));
        stmt.bind_int(3, file_id);
        stmt.bind_int(4, file_id);
        sqlite3_step(stmt.get());
    }
}

static dftracer::utils::coro::CoroTask<bool> build_index(
    const SqliteDatabase &db, int file_id, const std::string &gz_path,
    std::uint64_t ckpt_size, const Indexer::VisitorList &visitors) {
    int fd = ::open(gz_path.c_str(), O_RDONLY);
    if (fd < 0) {
        co_return false;
    }

    if (!visitors.empty()) {
        std::uint64_t compressed_bytes = file_size_bytes(gz_path);
        std::size_t estimated = static_cast<std::size_t>(
            compressed_bytes / (ckpt_size > 0 ? ckpt_size : 1));
        for (auto &v : visitors) {
            v.get().begin(estimated);
        }
    }

    std::uint64_t total_lines = 0;
    std::uint64_t total_uc_size = 0;
    std::uint64_t tail_line_count = 0;

    bool success =
        co_await process_chunks(fd, db, file_id, ckpt_size, total_lines,
                                total_uc_size, tail_line_count, visitors);
    ::close(fd);

    if (success) {
        co_await dftracer::utils::sqlite::run([&] {
            finalize_checkpoints(db, file_id, total_uc_size, total_lines,
                                 tail_line_count);
            insert_file_metadata_record(db, file_id, ckpt_size, total_lines,
                                        total_uc_size);
        });
    }

    co_return success;
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
      visitors_(std::move(other.visitors_)),
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
        visitors_ = std::move(other.visitors_);
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

dftracer::utils::coro::CoroTask<void> GzipIndexer::build_async() const {
    if (!force_rebuild && !need_rebuild()) {
        co_return;
    }

    co_await dftracer::utils::sqlite::run([&] {
        init_schema(db);

        int fid = find_file_id(gz_path_logical_path);
        if (fid != -1) {
            delete_file_record(db, fid);
        }
    });

    std::time_t mtime = get_file_modification_time(gz_path);
    auto hash = calculate_file_hash(gz_path);
    std::uint64_t bytes = file_size_bytes(gz_path);
    std::uint64_t final_ckpt_size =
        determine_checkpoint_size(ckpt_size, gz_path);

    int file_id = co_await dftracer::utils::sqlite::run([&] {
        int fid;
        insert_file_record(db, gz_path_logical_path, bytes, mtime, hash, fid);
        return fid;
    });

    if (!(co_await build_index(db, file_id, gz_path, final_ckpt_size,
                               visitors_))) {
        throw IndexerError(IndexerError::Type::BUILD_ERROR,
                           "Failed to build index for " + gz_path);
    }

    cached_is_valid = true;
    cached_file_id = file_id;
    co_return;
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
