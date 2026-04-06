#include <dftracer/utils/core/common/checkpointer.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/rocksdb/async.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_visitor.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint_size.h>
#include <dftracer/utils/utilities/indexer/internal/common/gzip_checkpointer.h>
#include <dftracer/utils/utilities/indexer/internal/common/gzip_inflater.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/transaction_scope.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace dftracer::utils::utilities::indexer::internal::gzip {

using dftracer::utils::utilities::indexer::IndexDatabase;
namespace rocks = dftracer::utils::rocksdb;

namespace {

void finalize_checkpoints(std::vector<IndexerCheckpoint>& checkpoints,
                          std::uint64_t total_uc_size,
                          std::uint64_t total_lines,
                          std::uint64_t tail_line_count) {
    for (std::size_t i = 0; i < checkpoints.size(); ++i) {
        auto& checkpoint = checkpoints[i];
        const std::uint64_t next_uc_offset = (i + 1 < checkpoints.size())
                                                 ? checkpoints[i + 1].uc_offset
                                                 : total_uc_size;
        const std::uint64_t next_c_offset = (i + 1 < checkpoints.size())
                                                ? checkpoints[i + 1].c_offset
                                                : checkpoint.c_offset;
        checkpoint.uc_size = next_uc_offset - checkpoint.uc_offset;
        checkpoint.c_size = next_c_offset - checkpoint.c_offset;
    }

    if (tail_line_count > 0 && total_lines > 0 && !checkpoints.empty()) {
        auto& last = checkpoints.back();
        last.last_line_num = total_lines;
        last.num_lines += tail_line_count;
    }
}

static dftracer::utils::coro::CoroTask<bool> process_chunks(
    int fd, std::uint64_t ckpt_size, std::uint64_t& total_lines,
    std::uint64_t& total_uc_size, std::uint64_t& tail_line_count,
    std::vector<IndexerCheckpoint>& checkpoints,
    const Indexer::VisitorList& visitors) {
    GzipInflater inflater;
    off_t offset = 0;
    if (!(co_await inflater.initialize(fd))) {
        co_return false;
    }

    std::uint64_t checkpoint_idx = 0;
    std::uint64_t current_uc_offset = 0;
    std::uint64_t next_ckpt_offset = ckpt_size;
    std::uint64_t line_count_in_chunk = 0;
    std::uint64_t first_line_in_chunk = total_lines + 1;

    std::string line_buf;
    const bool has_visitors = !visitors.empty();

    while (true) {
        GzipInflaterResult result;
        if (!(co_await inflater.read(fd, offset, result))) {
            if (result.bytes_read == 0) {
                break;
            }
            co_return false;
        }

        if (result.bytes_read == 0) {
            break;
        }

        current_uc_offset += result.bytes_read;
        total_lines += result.lines_found;
        line_count_in_chunk += result.lines_found;

        if (has_visitors) {
            const auto* data = inflater.out_buffer;
            const std::size_t n = result.bytes_read;
            std::size_t seg_start = 0;
            for (std::size_t i = 0; i < n; ++i) {
                if (data[i] == '\n') {
                    line_buf.append(
                        reinterpret_cast<const char*>(data + seg_start),
                        i - seg_start);
                    std::string_view line_sv(line_buf);
                    for (auto& visitor : visitors) {
                        visitor.get().on_line(line_sv, checkpoint_idx);
                    }
                    line_buf.clear();
                    seg_start = i + 1;
                }
            }
            if (seg_start < n) {
                line_buf.append(reinterpret_cast<const char*>(data + seg_start),
                                n - seg_start);
            }
        }

        if (current_uc_offset >= next_ckpt_offset && result.at_block_boundary) {
            const std::size_t chunk_start_uc = current_uc_offset;
            const std::size_t chunk_start_c =
                inflater.get_total_input_consumed();

            GzipCheckpointer checkpointer(inflater, chunk_start_uc);
            if (checkpointer.create(chunk_start_c)) {
                std::vector<unsigned char> compressed_dict;
                if (checkpointer.compress(compressed_dict)) {
                    IndexerCheckpoint checkpoint{
                        .checkpoint_idx = checkpoint_idx++,
                        .uc_offset = chunk_start_uc,
                        .uc_size = 0,
                        .c_offset = chunk_start_c,
                        .c_size = 0,
                        .bits = checkpointer.bits,
                        .dict_compressed = std::move(compressed_dict),
                        .num_lines = line_count_in_chunk,
                        .first_line_num = first_line_in_chunk,
                        .last_line_num = total_lines,
                    };
                    checkpoints.push_back(std::move(checkpoint));

                    if (has_visitors) {
                        for (auto& visitor : visitors) {
                            visitor.get().on_checkpoint(checkpoint_idx - 1);
                        }
                    }

                    line_count_in_chunk = 0;
                    first_line_in_chunk = total_lines + 1;
                    next_ckpt_offset = current_uc_offset + ckpt_size;
                }
            }
        }
    }

    total_uc_size = current_uc_offset;
    tail_line_count = line_count_in_chunk;
    co_return true;
}

static dftracer::utils::coro::CoroTask<bool> build_index(
    IndexDatabase& db, int file_id, const std::string& gz_path,
    std::uint64_t ckpt_size, const Indexer::VisitorList& visitors) {
    int fd = ::open(gz_path.c_str(), O_RDONLY);
    if (fd < 0) {
        co_return false;
    }

    if (!visitors.empty()) {
        const std::uint64_t compressed_bytes = file_size_bytes(gz_path);
        const std::size_t estimated = static_cast<std::size_t>(
            compressed_bytes / (ckpt_size > 0 ? ckpt_size : 1));
        for (auto& visitor : visitors) {
            visitor.get().begin(estimated);
        }
    }

    std::uint64_t total_lines = 0;
    std::uint64_t total_uc_size = 0;
    std::uint64_t tail_line_count = 0;
    std::vector<IndexerCheckpoint> checkpoints;

    const bool success =
        co_await process_chunks(fd, ckpt_size, total_lines, total_uc_size,
                                tail_line_count, checkpoints, visitors);
    ::close(fd);

    if (!success) {
        co_return false;
    }

    finalize_checkpoints(checkpoints, total_uc_size, total_lines,
                         tail_line_count);

    auto* db_ptr = &db;
    auto* checkpoints_ptr = &checkpoints;
    co_await rocks::run([db_ptr, file_id, ckpt_size, total_lines, total_uc_size,
                         checkpoints_ptr] {
        internal::TransactionScope txn(*db_ptr);
        for (const auto& checkpoint : *checkpoints_ptr) {
            db_ptr->insert_checkpoint(file_id, checkpoint);
        }
        db_ptr->insert_file_metadata(file_id, ckpt_size, total_lines,
                                     total_uc_size);
        txn.commit();
    });

    co_return true;
}

}  // namespace

GzipIndexer::GzipIndexer(const std::string& gz_path_,
                         const std::string& idx_path_, std::uint64_t ckpt_size_,
                         bool force_rebuild_)
    : gz_path(gz_path_),
      gz_path_logical_path(get_logical_path(gz_path_)),
      index_path(normalize_index_root(idx_path_)),
      ckpt_size(ckpt_size_),
      force_rebuild(force_rebuild_),
      cached_is_valid(false),
      cached_file_id(-1),
      cached_max_bytes(0),
      cached_max_bytes_ready(false),
      cached_num_lines(0),
      cached_num_lines_ready(false),
      cached_checkpoint_size(0),
      cached_checkpoint_size_ready(false) {
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

GzipIndexer::GzipIndexer(GzipIndexer&& other) noexcept
    : gz_path(std::move(other.gz_path)),
      gz_path_logical_path(std::move(other.gz_path_logical_path)),
      index_path(std::move(other.index_path)),
      ckpt_size(other.ckpt_size),
      force_rebuild(other.force_rebuild),
      visitors_(std::move(other.visitors_)),
      cached_is_valid(other.cached_is_valid.load()),
      cached_file_id(other.cached_file_id.load()),
      cached_max_bytes(other.cached_max_bytes.load()),
      cached_max_bytes_ready(other.cached_max_bytes_ready.load()),
      cached_num_lines(other.cached_num_lines.load()),
      cached_num_lines_ready(other.cached_num_lines_ready.load()),
      cached_checkpoint_size(other.cached_checkpoint_size.load()),
      cached_checkpoint_size_ready(other.cached_checkpoint_size_ready.load()),
      cached_checkpoints(std::move(other.cached_checkpoints)) {}

GzipIndexer& GzipIndexer::operator=(GzipIndexer&& other) noexcept {
    if (this != &other) {
        gz_path = std::move(other.gz_path);
        gz_path_logical_path = std::move(other.gz_path_logical_path);
        index_path = std::move(other.index_path);
        ckpt_size = other.ckpt_size;
        force_rebuild = other.force_rebuild;
        visitors_ = std::move(other.visitors_);
        cached_is_valid.store(other.cached_is_valid.load());
        cached_file_id.store(other.cached_file_id.load());
        cached_max_bytes.store(other.cached_max_bytes.load());
        cached_max_bytes_ready.store(other.cached_max_bytes_ready.load());
        cached_num_lines.store(other.cached_num_lines.load());
        cached_num_lines_ready.store(other.cached_num_lines_ready.load());
        cached_checkpoint_size.store(other.cached_checkpoint_size.load());
        cached_checkpoint_size_ready.store(
            other.cached_checkpoint_size_ready.load());
        std::lock_guard<std::mutex> lock(cached_checkpoints_mutex);
        cached_checkpoints = std::move(other.cached_checkpoints);
    }
    return *this;
}

void GzipIndexer::open() {}

void GzipIndexer::close() {}

dftracer::utils::coro::CoroTask<void> GzipIndexer::build_async() const {
    if (!force_rebuild && !need_rebuild()) {
        co_return;
    }

    IndexDatabase db(index_path);
    const std::time_t mtime = get_file_modification_time(gz_path);
    const auto hash = calculate_file_hash(gz_path);
    const std::uint64_t bytes = file_size_bytes(gz_path);
    const std::uint64_t final_ckpt_size =
        determine_checkpoint_size(ckpt_size, gz_path);
    const std::string logical = gz_path_logical_path;
    const auto* logical_ptr = &logical;
    const int file_id = co_await rocks::run([db_ptr = &db, logical_ptr, hash] {
        return db_ptr->get_or_create_file_info(*logical_ptr, hash);
    });

    if (!(co_await build_index(db, file_id, gz_path, final_ckpt_size,
                               visitors_))) {
        throw IndexerError(IndexerError::Type::BUILD_ERROR,
                           "Failed to build index for " + gz_path);
    }

    (void)mtime;
    (void)bytes;
    struct CacheSnapshot {
        std::uint64_t num_lines = 0;
        std::uint64_t max_bytes = 0;
        std::vector<IndexerCheckpoint> checkpoints;
    };
    auto snapshot = co_await rocks::run([db_ptr = &db, file_id] {
        CacheSnapshot cache;
        cache.num_lines = db_ptr->get_num_lines(file_id);
        cache.max_bytes = db_ptr->get_max_bytes(file_id);
        cache.checkpoints = db_ptr->query_checkpoints(file_id);
        return cache;
    });

    cached_is_valid = true;
    cached_file_id = file_id;
    cached_checkpoint_size = final_ckpt_size;
    cached_checkpoint_size_ready = true;
    cached_num_lines = snapshot.num_lines;
    cached_num_lines_ready = true;
    cached_max_bytes = snapshot.max_bytes;
    cached_max_bytes_ready = true;
    std::lock_guard<std::mutex> lock(cached_checkpoints_mutex);
    cached_checkpoints = std::move(snapshot.checkpoints);
    co_return;
}

bool GzipIndexer::is_valid() const { return cached_is_valid; }

bool GzipIndexer::exists() const {
    return fs::exists(index_path) && fs::is_directory(index_path);
}

bool GzipIndexer::need_rebuild() const {
    if (is_valid()) {
        return false;
    }
    if (!exists()) {
        return true;
    }

    try {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        const auto stored_hash = db.get_file_hash(gz_path_logical_path);
        const int file_id = db.get_file_info_id(gz_path_logical_path);
        if (!stored_hash || file_id < 0) {
            return true;
        }

        const auto current_hash = calculate_file_hash(gz_path);
        const auto current_ckpt_size = db.get_checkpoint_size(file_id);
        return current_hash != *stored_hash || current_ckpt_size == 0;
    } catch (...) {
        return true;
    }
}

const std::string& GzipIndexer::get_index_path() const { return index_path; }

const std::string& GzipIndexer::get_archive_path() const { return gz_path; }

const std::string& GzipIndexer::get_gz_path() const { return gz_path; }

std::uint64_t GzipIndexer::get_max_bytes() const {
    if (!cached_max_bytes_ready.load(std::memory_order_acquire)) {
        const int file_id = get_file_id();
        if (file_id != -1) {
            IndexDatabase db(
                index_path,
                dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
            auto val = db.get_max_bytes(file_id);
            cached_max_bytes.store(val, std::memory_order_relaxed);
            cached_max_bytes_ready.store(true, std::memory_order_release);
        }
    }
    return cached_max_bytes.load(std::memory_order_relaxed);
}

std::uint64_t GzipIndexer::get_checkpoint_size() const {
    if (!cached_checkpoint_size_ready.load(std::memory_order_acquire)) {
        const int file_id = get_file_id();
        if (file_id != -1) {
            IndexDatabase db(
                index_path,
                dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
            auto val = db.get_checkpoint_size(file_id);
            cached_checkpoint_size.store(val, std::memory_order_relaxed);
            cached_checkpoint_size_ready.store(true, std::memory_order_release);
        }
    }
    return cached_checkpoint_size.load(std::memory_order_relaxed);
}

std::uint64_t GzipIndexer::get_num_lines() const {
    if (!cached_num_lines_ready.load(std::memory_order_acquire)) {
        const int file_id = get_file_id();
        if (file_id != -1) {
            IndexDatabase db(
                index_path,
                dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
            auto val = db.get_num_lines(file_id);
            cached_num_lines.store(val, std::memory_order_relaxed);
            cached_num_lines_ready.store(true, std::memory_order_release);
        }
    }
    return cached_num_lines.load(std::memory_order_relaxed);
}

int GzipIndexer::get_file_id() const {
    auto val = cached_file_id.load(std::memory_order_relaxed);
    if (val == -1) {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        val = db.get_file_info_id(gz_path_logical_path);
        cached_file_id.store(val, std::memory_order_relaxed);
    }
    return val;
}

int GzipIndexer::find_file_id(const std::string& path) const {
    IndexDatabase db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
    return db.get_file_info_id(get_logical_path(path));
}

bool GzipIndexer::find_checkpoint(std::size_t target_offset,
                                  IndexerCheckpoint& checkpoint) const {
    const int file_id = get_file_id();
    if (file_id == -1) {
        return false;
    }
    IndexDatabase db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
    return db.find_checkpoint(file_id, target_offset, checkpoint);
}

std::vector<IndexerCheckpoint> GzipIndexer::get_checkpoints() const {
    std::lock_guard<std::mutex> lock(cached_checkpoints_mutex);
    if (cached_checkpoints.empty()) {
        const int file_id = get_file_id();
        if (file_id != -1) {
            IndexDatabase db(
                index_path,
                dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
            cached_checkpoints = db.query_checkpoints(file_id);
        }
    }
    return cached_checkpoints;
}

std::vector<IndexerCheckpoint> GzipIndexer::get_checkpoints_for_line_range(
    std::uint64_t start_line, std::uint64_t end_line) const {
    const int file_id = get_file_id();
    if (file_id == -1) {
        return {};
    }
    IndexDatabase db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
    return db.query_checkpoints_for_line_range(file_id, start_line, end_line);
}

}  // namespace dftracer::utils::utilities::indexer::internal::gzip
