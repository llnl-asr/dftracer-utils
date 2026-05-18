#ifndef DFTRACER_UTILS_CORE_ROCKSDB_DB_MANAGER_H
#define DFTRACER_UTILS_CORE_ROCKSDB_DB_MANAGER_H

#include <dftracer/utils/core/rocksdb/database.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace dftracer::utils::rocksdb {

// Process-wide registry of open RocksDB instances keyed by their normalized
// .dftindex root path. The manager owns one live instance per path so short-
// lived wrappers (IndexDatabase, ProvenanceDatabase, Python bindings, etc.)
// reuse the same DB instead of repeatedly reopening it.
class RocksDBManager {
   public:
    static RocksDBManager& instance();

    std::shared_ptr<RocksDatabase> get_or_open(
        const std::string& db_path,
        RocksDatabase::OpenMode open_mode = RocksDatabase::OpenMode::ReadWrite,
        RocksDatabase::CfOptionsOverride cf_override = nullptr);
    void reset(const std::string& db_path);
    void shutdown();

   private:
    RocksDBManager() = default;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, std::weak_ptr<RocksDatabase>> databases_;
    std::unordered_set<std::string> opening_;
};

}  // namespace dftracer::utils::rocksdb

#endif  // DFTRACER_UTILS_CORE_ROCKSDB_DB_MANAGER_H
