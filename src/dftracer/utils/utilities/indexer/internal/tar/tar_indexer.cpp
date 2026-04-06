#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/rocksdb/async.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/common/gzip_inflater.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/tar/tar_indexer.h>
#include <dftracer/utils/utilities/indexer/internal/tar/tar_parser.h>
#include <dftracer/utils/utilities/indexer/internal/transaction_scope.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

namespace dftracer::utils::utilities::indexer::internal::tar {

using dftracer::utils::utilities::indexer::IndexDatabase;
namespace rocks = dftracer::utils::rocksdb;

namespace {

std::string normalize_idx_path(const std::string& path) {
    fs::path input(path);
    if (input.filename() == ".dftindex") {
        return input.string();
    }
    if (input.parent_path().filename() == ".dftindex") {
        return input.parent_path().string();
    }
    if (input.has_extension()) {
        return (input.parent_path() / ".dftindex").string();
    }
    return (input / ".dftindex").string();
}

dftracer::utils::coro::CoroTask<bool> build_tar_index(
    IndexDatabase& db, int file_id, const std::string& tar_gz_path,
    std::uint64_t ckpt_size) {
    int fd = ::open(tar_gz_path.c_str(), O_RDONLY);
    if (fd < 0) {
        co_return false;
    }

    GzipInflater inflater;
    off_t offset = 0;
    if (!(co_await inflater.initialize(fd))) {
        ::close(fd);
        co_return false;
    }

    std::uint64_t total_lines = 0;
    std::uint64_t total_uc_size = 0;
    std::uint64_t current_uc_offset = 0;

    TarParser parser;
    std::vector<unsigned char> accumulated_data;
    accumulated_data.reserve(1024 * 1024);

    while (true) {
        GzipInflaterResult result;
        if (!(co_await inflater.read(fd, offset, result))) {
            if (result.bytes_read == 0) {
                break;
            }
            ::close(fd);
            co_return false;
        }

        if (result.bytes_read == 0) {
            break;
        }

        accumulated_data.insert(accumulated_data.end(), inflater.out_buffer,
                                inflater.out_buffer + result.bytes_read);
        current_uc_offset += result.bytes_read;
        total_lines += result.lines_found;
    }

    std::vector<TarFileEntry> tar_entries;
    if (!parser.parse_headers(accumulated_data.data(), accumulated_data.size(),
                              0, tar_entries)) {
        DFTRACER_UTILS_LOG_DEBUG("%s", "Failed to parse TAR headers");
    }

    total_uc_size = current_uc_offset;

    auto* db_ptr = &db;
    auto* tar_entries_ptr = &tar_entries;
    const std::string archive_name = fs::path(tar_gz_path).filename().string();
    const auto* archive_name_ptr = &archive_name;
    co_await rocks::run([db_ptr, file_id, ckpt_size, total_lines, total_uc_size,
                         tar_entries_ptr, archive_name_ptr] {
        internal::TransactionScope txn(*db_ptr);
        std::uint64_t regular_files = 0;
        for (const auto& entry : *tar_entries_ptr) {
            if (!entry.is_regular_file()) {
                continue;
            }

            ++regular_files;
            db_ptr->insert_tar_file(
                file_id, IndexDatabase::TarFileRecord{
                             .file_name = entry.name,
                             .file_size = entry.size,
                             .file_mtime = entry.mtime,
                             .typeflag = entry.typeflag,
                             .data_offset = entry.data_offset,
                             .uncompressed_offset = entry.uncompressed_offset,
                         });
        }

        db_ptr->insert_file_metadata(file_id, ckpt_size, total_lines,
                                     total_uc_size);
        db_ptr->insert_tar_archive_metadata(file_id, *archive_name_ptr,
                                            ckpt_size, total_lines,
                                            total_uc_size, regular_files);
        txn.commit();
    });

    ::close(fd);
    co_return true;
}

}  // namespace

TarIndexer::TarIndexer(const std::string& tar_gz_file_path,
                       const std::string& index_path_value,
                       std::uint64_t checkpoint_size, bool rebuild_force)
    : tar_gz_path(tar_gz_file_path),
      tar_gz_path_logical_path(get_logical_path(tar_gz_file_path)),
      index_path(normalize_idx_path(index_path_value)),
      ckpt_size(checkpoint_size),
      force_rebuild(rebuild_force) {
    open();
}

TarIndexer::~TarIndexer() {
    DFTRACER_UTILS_LOG_DEBUG("Destroying TarIndexer for %s",
                             tar_gz_path.c_str());
    close();
}

TarIndexer::TarIndexer(TarIndexer&& other) noexcept
    : tar_gz_path(std::move(other.tar_gz_path)),
      tar_gz_path_logical_path(std::move(other.tar_gz_path_logical_path)),
      index_path(std::move(other.index_path)),
      ckpt_size(other.ckpt_size),
      force_rebuild(other.force_rebuild),
      cached_is_valid(std::move(other.cached_is_valid)),
      cached_archive_id(std::move(other.cached_archive_id)),
      cached_max_bytes(std::move(other.cached_max_bytes)),
      cached_num_lines(std::move(other.cached_num_lines)),
      cached_num_files(std::move(other.cached_num_files)),
      cached_checkpoint_size(std::move(other.cached_checkpoint_size)),
      cached_archive_name(std::move(other.cached_archive_name)),
      cached_checkpoints(std::move(other.cached_checkpoints)) {}

TarIndexer& TarIndexer::operator=(TarIndexer&& other) noexcept {
    if (this != &other) {
        tar_gz_path = std::move(other.tar_gz_path);
        tar_gz_path_logical_path = std::move(other.tar_gz_path_logical_path);
        index_path = std::move(other.index_path);
        ckpt_size = other.ckpt_size;
        force_rebuild = other.force_rebuild;
        std::scoped_lock lock(cache_mutex, other.cache_mutex);
        cached_is_valid = std::move(other.cached_is_valid);
        cached_archive_id = std::move(other.cached_archive_id);
        cached_max_bytes = std::move(other.cached_max_bytes);
        cached_num_lines = std::move(other.cached_num_lines);
        cached_num_files = std::move(other.cached_num_files);
        cached_checkpoint_size = std::move(other.cached_checkpoint_size);
        cached_archive_name = std::move(other.cached_archive_name);
        cached_checkpoints = std::move(other.cached_checkpoints);
    }
    return *this;
}

void TarIndexer::open() {}

void TarIndexer::close() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    cached_is_valid.reset();
    cached_archive_id.reset();
    cached_max_bytes.reset();
    cached_num_lines.reset();
    cached_num_files.reset();
    cached_checkpoint_size.reset();
    cached_archive_name.clear();
    cached_checkpoints.clear();
}

dftracer::utils::coro::CoroTask<void> TarIndexer::build_async() const {
    if (!force_rebuild && !need_rebuild()) {
        co_return;
    }

    IndexDatabase db(index_path);
    const auto hash = calculate_file_hash(tar_gz_path);
    const std::string logical = tar_gz_path_logical_path;
    const auto* logical_ptr = &logical;
    const int file_id = co_await rocks::run([db_ptr = &db, logical_ptr, hash] {
        return db_ptr->get_or_create_file_info(*logical_ptr, hash);
    });

    if (!(co_await build_tar_index(db, file_id, tar_gz_path, ckpt_size))) {
        throw IndexerError(IndexerError::Type::BUILD_ERROR,
                           "Failed to build TAR index for " + tar_gz_path);
    }

    struct CacheSnapshot {
        std::uint64_t checkpoint_size = 0;
        std::uint64_t num_lines = 0;
        std::uint64_t max_bytes = 0;
        std::uint64_t num_files = 0;
        std::string archive_name;
        std::vector<IndexerCheckpoint> checkpoints;
    };
    const std::string fallback_archive_name =
        fs::path(tar_gz_path).filename().string();
    const auto* fallback_archive_name_ptr = &fallback_archive_name;
    auto snapshot =
        co_await rocks::run([db_ptr = &db, file_id, fallback_archive_name_ptr] {
            CacheSnapshot cache;
            cache.checkpoint_size = db_ptr->get_checkpoint_size(file_id);
            cache.num_lines = db_ptr->get_num_lines(file_id);
            cache.max_bytes = db_ptr->get_max_bytes(file_id);
            if (auto metadata = db_ptr->query_tar_archive_metadata(file_id)) {
                cache.num_files = metadata->total_files;
                cache.archive_name = metadata->archive_name;
            } else {
                cache.archive_name = *fallback_archive_name_ptr;
            }
            cache.checkpoints = db_ptr->query_checkpoints(file_id);
            return cache;
        });

    std::lock_guard<std::mutex> lock(cache_mutex);
    cached_is_valid = true;
    cached_archive_id = file_id;
    cached_checkpoint_size = snapshot.checkpoint_size;
    cached_num_lines = snapshot.num_lines;
    cached_max_bytes = snapshot.max_bytes;
    cached_num_files = snapshot.num_files;
    cached_archive_name = std::move(snapshot.archive_name);
    cached_checkpoints = std::move(snapshot.checkpoints);
    co_return;
}

bool TarIndexer::need_rebuild() const {
    if (force_rebuild) {
        return true;
    }

    try {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        const auto stored_hash = db.get_file_hash(tar_gz_path_logical_path);
        if (!stored_hash.has_value()) {
            return true;
        }

        const int file_id = db.get_file_info_id(tar_gz_path_logical_path);
        if (file_id < 0) {
            return true;
        }

        if (db.get_checkpoint_size(file_id) == 0) {
            return true;
        }

        if (!db.query_tar_archive_metadata(file_id).has_value()) {
            return true;
        }

        return *stored_hash != calculate_file_hash(tar_gz_path);
    } catch (...) {
        return true;
    }
}

bool TarIndexer::is_valid() const {
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (!cached_is_valid.has_value()) {
        try {
            IndexDatabase db(
                index_path,
                dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
            const auto file_id = db.get_file_info_id(tar_gz_path_logical_path);
            cached_is_valid =
                file_id != -1 &&
                db.query_tar_archive_metadata(file_id).has_value();
        } catch (...) {
            cached_is_valid = false;
        }
    }
    return *cached_is_valid;
}

bool TarIndexer::exists() const {
    return fs::exists(index_path) && fs::is_directory(index_path);
}

const std::string& TarIndexer::get_index_path() const { return index_path; }

const std::string& TarIndexer::get_archive_path() const { return tar_gz_path; }

const std::string& TarIndexer::get_tar_gz_path() const { return tar_gz_path; }

std::uint64_t TarIndexer::get_checkpoint_size() const {
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (cached_checkpoint_size.has_value()) {
            return *cached_checkpoint_size;
        }
    }
    const int file_id = get_archive_id();
    if (file_id != -1) {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        const auto value = db.get_checkpoint_size(file_id);
        std::lock_guard<std::mutex> lock(cache_mutex);
        cached_checkpoint_size = value;
    }
    std::lock_guard<std::mutex> lock(cache_mutex);
    return cached_checkpoint_size.value_or(0);
}

std::uint64_t TarIndexer::get_max_bytes() const {
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (cached_max_bytes.has_value()) {
            return *cached_max_bytes;
        }
    }
    const int file_id = get_archive_id();
    if (file_id != -1) {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        const auto value = db.get_max_bytes(file_id);
        std::lock_guard<std::mutex> lock(cache_mutex);
        cached_max_bytes = value;
    }
    std::lock_guard<std::mutex> lock(cache_mutex);
    return cached_max_bytes.value_or(0);
}

std::uint64_t TarIndexer::get_num_lines() const {
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (cached_num_lines.has_value()) {
            return *cached_num_lines;
        }
    }
    const int file_id = get_archive_id();
    if (file_id != -1) {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        const auto value = db.get_num_lines(file_id);
        std::lock_guard<std::mutex> lock(cache_mutex);
        cached_num_lines = value;
    }
    std::lock_guard<std::mutex> lock(cache_mutex);
    return cached_num_lines.value_or(0);
}

std::uint64_t TarIndexer::get_num_files() const {
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (cached_num_files.has_value()) {
            return *cached_num_files;
        }
    }
    const int file_id = get_archive_id();
    if (file_id != -1) {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        std::uint64_t value = 0;
        if (auto metadata = db.query_tar_archive_metadata(file_id)) {
            value = metadata->total_files;
        }
        std::lock_guard<std::mutex> lock(cache_mutex);
        cached_num_files = value;
    }
    std::lock_guard<std::mutex> lock(cache_mutex);
    return cached_num_files.value_or(0);
}

std::string TarIndexer::get_archive_name() const {
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (!cached_archive_name.empty()) {
            return cached_archive_name;
        }
    }
    std::string value;
    const int file_id = get_archive_id();
    if (file_id != -1) {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        if (auto metadata = db.query_tar_archive_metadata(file_id)) {
            value = metadata->archive_name;
        }
    }
    if (value.empty()) {
        value = fs::path(tar_gz_path).filename().string();
    }
    std::lock_guard<std::mutex> lock(cache_mutex);
    cached_archive_name = value;
    return cached_archive_name;
}

int TarIndexer::get_archive_id() const {
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (!cached_archive_id.has_value()) {
        cached_archive_id = find_archive_id(tar_gz_path_logical_path);
    }
    return *cached_archive_id;
}

int TarIndexer::find_archive_id(const std::string& tar_gz_file_path) const {
    IndexDatabase db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
    return db.get_file_info_id(tar_gz_file_path);
}

bool TarIndexer::find_checkpoint(std::size_t target_offset,
                                 IndexerCheckpoint& checkpoint) const {
    const int archive_id = get_archive_id();
    if (archive_id == -1) {
        return false;
    }
    IndexDatabase db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
    return db.find_checkpoint(archive_id, target_offset, checkpoint);
}

std::vector<IndexerCheckpoint> TarIndexer::get_checkpoints() const {
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (!cached_checkpoints.empty()) {
            return cached_checkpoints;
        }
    }
    const int archive_id = get_archive_id();
    if (archive_id != -1) {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        auto checkpoints = db.query_checkpoints(archive_id);
        std::lock_guard<std::mutex> lock(cache_mutex);
        cached_checkpoints = std::move(checkpoints);
    }
    std::lock_guard<std::mutex> lock(cache_mutex);
    return cached_checkpoints;
}

std::vector<IndexerCheckpoint> TarIndexer::get_checkpoints_for_line_range(
    std::uint64_t start_line, std::uint64_t end_line) const {
    const int archive_id = get_archive_id();
    if (archive_id == -1) {
        return {};
    }
    IndexDatabase db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
    return db.query_checkpoints_for_line_range(archive_id, start_line,
                                               end_line);
}

std::vector<TarIndexer::TarFileInfo> TarIndexer::list_files() const {
    const int archive_id = get_archive_id();
    if (archive_id == -1) {
        return {};
    }

    IndexDatabase db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
    auto tar_files = db.query_tar_files(archive_id);
    std::vector<TarFileInfo> result;
    result.reserve(tar_files.size());
    for (const auto& tf : tar_files) {
        result.push_back(TarFileInfo{tf.file_name, tf.file_size, tf.file_mtime,
                                     tf.typeflag, tf.data_offset,
                                     tf.uncompressed_offset});
    }
    return result;
}

bool TarIndexer::find_file(const std::string& file_name,
                           TarFileInfo& file_info) const {
    const int archive_id = get_archive_id();
    if (archive_id == -1) {
        return false;
    }

    IndexDatabase db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
    IndexDatabase::TarFileRecord record;
    if (!db.find_tar_file(archive_id, file_name, record)) {
        return false;
    }

    file_info = TarFileInfo{record.file_name,   record.file_size,
                            record.file_mtime,  record.typeflag,
                            record.data_offset, record.uncompressed_offset};
    return true;
}

std::vector<TarIndexer::TarFileInfo> TarIndexer::find_files_in_range(
    std::uint64_t start_offset, std::uint64_t end_offset) const {
    const int archive_id = get_archive_id();
    if (archive_id == -1) {
        return {};
    }

    IndexDatabase db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
    auto tar_files =
        db.query_tar_files_in_range(archive_id, start_offset, end_offset);
    std::vector<TarFileInfo> result;
    result.reserve(tar_files.size());
    for (const auto& tf : tar_files) {
        result.push_back(TarFileInfo{tf.file_name, tf.file_size, tf.file_mtime,
                                     tf.typeflag, tf.data_offset,
                                     tf.uncompressed_offset});
    }
    return result;
}

}  // namespace dftracer::utils::utilities::indexer::internal::tar
