#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/env.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/core/rocksdb/filesystem.h>
#include <rocksdb/slice.h>

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
    static const ::rocksdb::WriteOptions options;
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

std::vector<std::string> RocksDatabase::default_column_families() {
    return {"default",    "checkpoints", "metadata",   "chunk_bloom",
            "file_bloom", "chunk_stats", "dimensions", "chunk_dim_stats",
            "manifest",   "provenance",  "archives",   "tar_files"};
}

::rocksdb::Options RocksDatabase::default_options() {
    ::rocksdb::Options options;
    options.create_if_missing = true;
    options.create_missing_column_families = true;
    options.allow_concurrent_memtable_write = true;
    options.enable_pipelined_write = true;
    options.max_open_files = Env::rocksdb_max_open_files();
    return options;
}

::rocksdb::ColumnFamilyOptions RocksDatabase::default_column_family_options() {
    ::rocksdb::ColumnFamilyOptions options;
    options.compression = ::rocksdb::kLZ4Compression;
    options.bottommost_compression = ::rocksdb::kZlibCompression;
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
            throw std::runtime_error(
                "Failed to list RocksDB column families at '" + db_path_ +
                "': " + list_status.ToString());
        }
        column_family_names = default_column_families();
    } else {
        if (open_mode_ == OpenMode::ReadWrite) {
            for (const auto& name : default_column_families()) {
                if (std::find(column_family_names.begin(),
                              column_family_names.end(),
                              name) == column_family_names.end()) {
                    column_family_names.push_back(name);
                }
            }
        }
    }

    std::vector<::rocksdb::ColumnFamilyDescriptor> descriptors;
    descriptors.reserve(column_family_names.size());
    for (const auto& name : column_family_names) {
        descriptors.emplace_back(name, cf_options);
    }

    std::vector<::rocksdb::ColumnFamilyHandle*> handles;
    auto status =
        open_mode_ == OpenMode::ReadOnly
            ? ::rocksdb::DB::OpenForReadOnly(db_options, db_path_, descriptors,
                                             &handles, &db_, false)
            : ::rocksdb::DB::Open(db_options, db_path_, descriptors, &handles,
                                  &db_);
    if (!status.ok()) {
        cleanup_failed_open(db_, handles);
        throw std::runtime_error("Failed to open RocksDB at '" + db_path_ +
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
    const auto name = column_family.empty() ? std::string("default")
                                            : std::string(column_family);
    const auto it = column_families_.find(name);
    if (it == column_families_.end() || it->second == nullptr) {
        throw std::invalid_argument("Unknown RocksDB column family: " + name);
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

::rocksdb::Status RocksDatabase::del(std::string_view key,
                                     std::string_view column_family) {
    return db_->Delete(write_options(), column_family_handle(column_family),
                       ::rocksdb::Slice(key.data(), key.size()));
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

}  // namespace dftracer::utils::rocksdb
