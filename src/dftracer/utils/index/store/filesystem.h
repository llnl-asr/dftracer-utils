#ifndef DFTRACER_UTILS_INDEX_STORE_FILESYSTEM_H
#define DFTRACER_UTILS_INDEX_STORE_FILESYSTEM_H

#include <cstdint>
#include <memory>

namespace rocksdb {
class Env;
class FileSystem;
}  // namespace rocksdb

namespace dftracer::utils::index::store {

/// Staged SSTs an ingest linked into the database without the reopen and fsync
/// RocksDB adds, in this process.
std::uint64_t ingest_reopens_skipped() noexcept;

/// While alive, `file_system` (made by make_dftracer_file_system) refuses to
/// reopen a file for writing, which RocksDB reads as "no reopen needed" and
/// skips the per-file fsync of an ingest. Only for ingesting SSTs that
/// SstFileWriter already synced: a hard link shares the synced inode.
class IngestReopenSkip {
   public:
    explicit IngestReopenSkip(::rocksdb::FileSystem& file_system);
    ~IngestReopenSkip();
    IngestReopenSkip(const IngestReopenSkip&) = delete;
    IngestReopenSkip& operator=(const IngestReopenSkip&) = delete;

   private:
    ::rocksdb::FileSystem& file_system_;
};

std::shared_ptr<::rocksdb::FileSystem> make_dftracer_file_system();
std::unique_ptr<::rocksdb::Env> make_dftracer_env(
    const std::shared_ptr<::rocksdb::FileSystem>& file_system);

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_FILESYSTEM_H
