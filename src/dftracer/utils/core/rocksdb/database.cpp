#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/env.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/core/rocksdb/filesystem.h>
#include <rocksdb/slice.h>
#include <rocksdb/table.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace dftracer::utils::rocksdb {

namespace {

std::atomic<bool>& process_exiting_flag() {
    static std::atomic<bool> flag{false};
    return flag;
}

const ::rocksdb::ReadOptions& read_options() {
    static const ::rocksdb::ReadOptions options;
    return options;
}

const ::rocksdb::WriteOptions& write_options() {
    static const auto options = [] {
        ::rocksdb::WriteOptions wo;
        wo.disableWAL = true;
        return wo;
    }();
    return options;
}

void cleanup_failed_open(::rocksdb::DB*& db,
                         std::vector<::rocksdb::ColumnFamilyHandle*>& handles) {
    if (db != nullptr) {
        for (auto* handle : handles) {
            if (handle != nullptr) {
                db->DestroyColumnFamilyHandle(handle);
            }
        }
        static_cast<void>(db->Close());
        delete db;
        db = nullptr;
    }
    handles.clear();
}

}  // namespace

void mark_process_exiting_for_rocksdb() {
    process_exiting_flag().store(true, std::memory_order_relaxed);
}

RocksDatabase::RocksDatabase() = default;

RocksDatabase::RocksDatabase(const std::string& db_path, OpenMode open_mode) {
    open(db_path, open_mode);
}

RocksDatabase::~RocksDatabase() { close(); }

RocksDatabase::RocksDatabase(RocksDatabase&& other) noexcept
    : db_path_(std::move(other.db_path_)),
      open_mode_(other.open_mode_),
      file_system_(std::move(other.file_system_)),
      env_(std::move(other.env_)),
      db_(std::exchange(other.db_, nullptr)),
      column_families_(std::move(other.column_families_)) {}

RocksDatabase& RocksDatabase::operator=(RocksDatabase&& other) noexcept {
    if (this != &other) {
        close();
        db_path_ = std::move(other.db_path_);
        open_mode_ = other.open_mode_;
        file_system_ = std::move(other.file_system_);
        env_ = std::move(other.env_);
        db_ = std::exchange(other.db_, nullptr);
        column_families_ = std::move(other.column_families_);
    }
    return *this;
}

const decltype(cf::ALL)& RocksDatabase::default_column_families() {
    return cf::ALL;
}

::rocksdb::Options RocksDatabase::default_options() {
    ::rocksdb::Options options;
    options.create_if_missing = true;
    options.create_missing_column_families = true;
    options.allow_concurrent_memtable_write = true;
    options.enable_pipelined_write = true;
    options.max_open_files = Env::rocksdb_max_open_files();
    options.max_background_jobs = 8;
    options.max_subcompactions = 8;
    options.write_buffer_size = 256 * 1024 * 1024;
    options.max_write_buffer_number = 4;
    return options;
}

::rocksdb::ColumnFamilyOptions RocksDatabase::default_column_family_options() {
    ::rocksdb::ColumnFamilyOptions options;

    ::rocksdb::BlockBasedTableOptions bbt;
    bbt.block_size = 32 * 1024;
    bbt.format_version = 5;
    bbt.index_block_restart_interval = 16;
    options.table_factory.reset(::rocksdb::NewBlockBasedTableFactory(bbt));

#ifdef DFTRACER_UTILS_ENABLE_ZSTD
    options.compression = ::rocksdb::kZSTD;
    options.compression_opts.level = constants::rocksdb::ZSTD_COMPRESSION_LEVEL;
    options.compression_opts.max_dict_bytes =
        constants::rocksdb::ZSTD_MAX_DICT_BYTES;
    options.compression_opts.zstd_max_train_bytes =
        constants::rocksdb::ZSTD_MAX_TRAIN_BYTES;
    options.compression_opts.enabled = true;
    options.bottommost_compression = ::rocksdb::kZSTD;
    options.bottommost_compression_opts.level =
        constants::rocksdb::ZSTD_COMPRESSION_LEVEL;
    options.bottommost_compression_opts.max_dict_bytes =
        constants::rocksdb::ZSTD_MAX_DICT_BYTES;
    options.bottommost_compression_opts.zstd_max_train_bytes =
        constants::rocksdb::ZSTD_MAX_TRAIN_BYTES;
    options.bottommost_compression_opts.enabled = true;
#elif defined(DFTRACER_UTILS_ENABLE_LZ4)
    options.compression = ::rocksdb::kLZ4Compression;
    options.bottommost_compression = ::rocksdb::kZlibCompression;
#else
    options.compression = ::rocksdb::kZlibCompression;
    options.bottommost_compression = ::rocksdb::kZlibCompression;
#endif
    return options;
}

bool RocksDatabase::open(const std::string& db_path, OpenMode open_mode) {
    close();
    db_path_ = db_path;
    open_mode_ = open_mode;

    std::error_code ec;
    if (open_mode_ == OpenMode::ReadWrite) {
        fs::create_directories(fs::path(db_path_), ec);
    }

    auto db_options = default_options();
    if (open_mode_ == OpenMode::ReadOnly) {
        db_options.create_if_missing = false;
        db_options.create_missing_column_families = false;
    }
    file_system_ = make_dftracer_file_system();
    env_ = make_dftracer_env(file_system_);
    db_options.env = env_.get();
    auto cf_options = default_column_family_options();

    std::vector<std::string> column_family_names;
    auto list_status = ::rocksdb::DB::ListColumnFamilies(db_options, db_path_,
                                                         &column_family_names);
    if (!list_status.ok()) {
        if (open_mode_ == OpenMode::ReadOnly) {
            throw DFTUtilsException(
                ErrorCode::IO, "Failed to list RocksDB column families at '" +
                                   db_path_ + "': " + list_status.ToString());
        }
        column_family_names.reserve(default_column_families().size());
        for (auto name : default_column_families()) {
            column_family_names.emplace_back(name);
        }
    } else {
        if (open_mode_ == OpenMode::ReadWrite) {
            for (const auto& name : default_column_families()) {
                if (std::find(column_family_names.begin(),
                              column_family_names.end(),
                              name) == column_family_names.end()) {
                    column_family_names.emplace_back(name);
                }
            }
        }
    }

    std::vector<::rocksdb::ColumnFamilyDescriptor> descriptors;
    descriptors.reserve(column_family_names.size());
    for (const auto& name : column_family_names) {
        auto opts = cf_options;
        if (cf_options_override_) {
            cf_options_override_(name, opts);
        }
        descriptors.emplace_back(name, opts);
    }

    std::vector<::rocksdb::ColumnFamilyHandle*> handles;
    ::rocksdb::Status status;
    if (open_mode_ == OpenMode::ReadOnly) {
        status = ::rocksdb::DB::OpenForReadOnly(
            db_options, db_path_, descriptors, &handles, &db_, false);
    } else {
        status = ::rocksdb::DB::Open(db_options, db_path_, descriptors,
                                     &handles, &db_);
    }
    if (!status.ok()) {
        cleanup_failed_open(db_, handles);
        throw DFTUtilsException(ErrorCode::IO, "Failed to open RocksDB at '" +
                                                   db_path_ +
                                                   "': " + status.ToString());
    }

    column_families_.clear();
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        column_families_.emplace(descriptors[i].name, handles[i]);
    }

    return true;
}

void RocksDatabase::close() {
    if (db_ == nullptr) {
        column_families_.clear();
        return;
    }

    if (process_exiting_flag().load(std::memory_order_relaxed)) {
        db_ = nullptr;
        column_families_.clear();
        env_.reset();
        file_system_.reset();
        db_path_.clear();
        return;
    }

    for (auto& entry : column_families_) {
        if (entry.second != nullptr) {
            db_->DestroyColumnFamilyHandle(entry.second);
            entry.second = nullptr;
        }
    }
    column_families_.clear();

    auto* db = db_;
    db_ = nullptr;
    static_cast<void>(db->Close());
    delete db;
    env_.reset();
    file_system_.reset();
    db_path_.clear();
}

bool RocksDatabase::is_open() const noexcept { return db_ != nullptr; }

bool RocksDatabase::is_read_only() const noexcept {
    return open_mode_ == OpenMode::ReadOnly;
}

const std::string& RocksDatabase::path() const noexcept { return db_path_; }

::rocksdb::DB* RocksDatabase::get() const noexcept { return db_; }

::rocksdb::ColumnFamilyHandle* RocksDatabase::column_family_handle(
    std::string_view column_family) const {
    const auto name = column_family.empty() ? std::string(cf::DEFAULT)
                                            : std::string(column_family);
    const auto it = column_families_.find(name);
    if (it == column_families_.end() || it->second == nullptr) {
        throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                "Unknown RocksDB column family: " + name);
    }
    return it->second;
}

::rocksdb::Status RocksDatabase::put(std::string_view key,
                                     std::string_view value,
                                     std::string_view column_family) {
    return db_->Put(write_options(), column_family_handle(column_family),
                    ::rocksdb::Slice(key.data(), key.size()),
                    ::rocksdb::Slice(value.data(), value.size()));
}

::rocksdb::Status RocksDatabase::get(std::string_view key, std::string* value,
                                     std::string_view column_family) const {
    return db_->Get(read_options(), column_family_handle(column_family),
                    ::rocksdb::Slice(key.data(), key.size()), value);
}

::rocksdb::Status RocksDatabase::merge(std::string_view key,
                                       std::string_view value,
                                       std::string_view column_family) {
    return db_->Merge(write_options(), column_family_handle(column_family),
                      ::rocksdb::Slice(key.data(), key.size()),
                      ::rocksdb::Slice(value.data(), value.size()));
}

void RocksDatabase::set_cf_options_override(CfOptionsOverride override) {
    cf_options_override_ = std::move(override);
}

::rocksdb::Status RocksDatabase::merge(Batch& batch,
                                       std::string_view column_family,
                                       std::string_view key,
                                       std::string_view value) {
    return batch.Merge(column_family_handle(column_family),
                       ::rocksdb::Slice(key.data(), key.size()),
                       ::rocksdb::Slice(value.data(), value.size()));
}

::rocksdb::Status RocksDatabase::del(std::string_view key,
                                     std::string_view column_family) {
    return db_->Delete(write_options(), column_family_handle(column_family),
                       ::rocksdb::Slice(key.data(), key.size()));
}

::rocksdb::Status RocksDatabase::delete_range(std::string_view begin_key,
                                              std::string_view end_key,
                                              std::string_view column_family) {
    return db_->DeleteRange(
        write_options(), column_family_handle(column_family),
        ::rocksdb::Slice(begin_key.data(), begin_key.size()),
        ::rocksdb::Slice(end_key.data(), end_key.size()));
}

::rocksdb::Status RocksDatabase::put(Batch& batch,
                                     std::string_view column_family,
                                     std::string_view key,
                                     std::string_view value) {
    return batch.Put(column_family_handle(column_family),
                     ::rocksdb::Slice(key.data(), key.size()),
                     ::rocksdb::Slice(value.data(), value.size()));
}

::rocksdb::Status RocksDatabase::del(Batch& batch,
                                     std::string_view column_family,
                                     std::string_view key) {
    return batch.Delete(column_family_handle(column_family),
                        ::rocksdb::Slice(key.data(), key.size()));
}

RocksDatabase::Batch RocksDatabase::begin_batch() const { return Batch(); }

::rocksdb::Status RocksDatabase::commit_batch(Batch& batch) {
    return db_->Write(write_options(), &batch);
}

std::unique_ptr<::rocksdb::Iterator> RocksDatabase::new_iterator(
    std::string_view column_family) const {
    return std::unique_ptr<::rocksdb::Iterator>(
        db_->NewIterator(read_options(), column_family_handle(column_family)));
}

::rocksdb::Status RocksDatabase::compact(std::string_view column_family) {
    ::rocksdb::CompactRangeOptions opts;
    opts.max_subcompactions = 8;
    return db_->CompactRange(opts, column_family_handle(column_family), nullptr,
                             nullptr);
}

::rocksdb::Status RocksDatabase::ingest_external_files(
    std::string_view column_family,
    const std::vector<std::string>& external_files, bool ingest_behind) {
    if (external_files.empty()) {
        return ::rocksdb::Status::OK();
    }
    ::rocksdb::IngestExternalFileOptions opts;
    opts.move_files = false;
    opts.snapshot_consistency = true;
    opts.allow_global_seqno = true;
    opts.allow_blocking_flush = true;
    opts.ingest_behind = ingest_behind;
    return db_->IngestExternalFile(column_family_handle(column_family),
                                   external_files, opts);
}

}  // namespace dftracer::utils::rocksdb
