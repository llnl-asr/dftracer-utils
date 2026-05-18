#ifndef DFTRACER_UTILS_CORE_ROCKSDB_DATABASE_H
#define DFTRACER_UTILS_CORE_ROCKSDB_DATABASE_H

#include <dftracer/utils/core/rocksdb/column_families.h>
#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/file_system.h>
#include <rocksdb/merge_operator.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::rocksdb {

void mark_process_exiting_for_rocksdb();

class RocksDatabase {
   public:
    using Batch = ::rocksdb::WriteBatch;
    enum class OpenMode { ReadWrite, ReadOnly };

    RocksDatabase();
    explicit RocksDatabase(const std::string& db_path,
                           OpenMode open_mode = OpenMode::ReadWrite);
    ~RocksDatabase();

    RocksDatabase(const RocksDatabase&) = delete;
    RocksDatabase& operator=(const RocksDatabase&) = delete;

    RocksDatabase(RocksDatabase&& other) noexcept;
    RocksDatabase& operator=(RocksDatabase&& other) noexcept;

    bool open(const std::string& db_path,
              OpenMode open_mode = OpenMode::ReadWrite);
    void close();

    bool is_open() const noexcept;
    bool is_read_only() const noexcept;
    const std::string& path() const noexcept;
    ::rocksdb::DB* get() const noexcept;

    ::rocksdb::Status put(std::string_view key, std::string_view value,
                          std::string_view column_family = cf::DEFAULT);
    ::rocksdb::Status get(std::string_view key, std::string* value,
                          std::string_view column_family = cf::DEFAULT) const;
    ::rocksdb::Status del(std::string_view key,
                          std::string_view column_family = cf::DEFAULT);
    ::rocksdb::Status delete_range(
        std::string_view begin_key, std::string_view end_key,
        std::string_view column_family = cf::DEFAULT);

    ::rocksdb::Status put(Batch& batch, std::string_view column_family,
                          std::string_view key, std::string_view value);
    ::rocksdb::Status del(Batch& batch, std::string_view column_family,
                          std::string_view key);

    ::rocksdb::Status merge(std::string_view key, std::string_view value,
                            std::string_view column_family = cf::DEFAULT);
    ::rocksdb::Status merge(Batch& batch, std::string_view column_family,
                            std::string_view key, std::string_view value);

    Batch begin_batch() const;
    ::rocksdb::Status commit_batch(Batch& batch);

    std::unique_ptr<::rocksdb::Iterator> new_iterator(
        std::string_view column_family = cf::DEFAULT) const;

    ::rocksdb::Status compact(std::string_view column_family = cf::DEFAULT);

    /// Bulk-ingest externally built SST files into the named column family.
    /// Keys across the SSTs must be sorted and non-overlapping unless the
    /// caller requests `ingest_behind`, which pushes entries to the bottom
    /// level and silently drops duplicate keys (for content-addressed CFs).
    ::rocksdb::Status ingest_external_files(
        std::string_view column_family,
        const std::vector<std::string>& external_files,
        bool ingest_behind = false);

    using CfOptionsOverride = std::function<void(
        const std::string&, ::rocksdb::ColumnFamilyOptions&)>;
    void set_cf_options_override(CfOptionsOverride override);

    static const decltype(cf::ALL)& default_column_families();
    static ::rocksdb::Options default_options();
    static ::rocksdb::ColumnFamilyOptions default_column_family_options();

   private:
    ::rocksdb::ColumnFamilyHandle* column_family_handle(
        std::string_view column_family) const;

    std::string db_path_;
    OpenMode open_mode_ = OpenMode::ReadWrite;
    std::shared_ptr<::rocksdb::FileSystem> file_system_;
    std::unique_ptr<::rocksdb::Env> env_;
    ::rocksdb::DB* db_ = nullptr;
    std::unordered_map<std::string, ::rocksdb::ColumnFamilyHandle*>
        column_families_;
    CfOptionsOverride cf_options_override_;
};

}  // namespace dftracer::utils::rocksdb

#endif  // DFTRACER_UTILS_CORE_ROCKSDB_DATABASE_H
