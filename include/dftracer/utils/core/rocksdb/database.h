#ifndef DFTRACER_UTILS_CORE_ROCKSDB_DATABASE_H
#define DFTRACER_UTILS_CORE_ROCKSDB_DATABASE_H

#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/file_system.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

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
                          std::string_view column_family = "default");
    ::rocksdb::Status get(std::string_view key, std::string* value,
                          std::string_view column_family = "default") const;
    ::rocksdb::Status del(std::string_view key,
                          std::string_view column_family = "default");

    ::rocksdb::Status put(Batch& batch, std::string_view column_family,
                          std::string_view key, std::string_view value);
    ::rocksdb::Status del(Batch& batch, std::string_view column_family,
                          std::string_view key);

    Batch begin_batch() const;
    ::rocksdb::Status commit_batch(Batch& batch);

    std::unique_ptr<::rocksdb::Iterator> new_iterator(
        std::string_view column_family = "default") const;

    static std::vector<std::string> default_column_families();
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
};

}  // namespace dftracer::utils::rocksdb

#endif  // DFTRACER_UTILS_CORE_ROCKSDB_DATABASE_H
