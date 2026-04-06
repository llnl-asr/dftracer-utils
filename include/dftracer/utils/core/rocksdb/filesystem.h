#ifndef DFTRACER_UTILS_CORE_ROCKSDB_FILESYSTEM_H
#define DFTRACER_UTILS_CORE_ROCKSDB_FILESYSTEM_H

#include <memory>

namespace rocksdb {
class Env;
class FileSystem;
}  // namespace rocksdb

namespace dftracer::utils::rocksdb {

std::shared_ptr<::rocksdb::FileSystem> make_dftracer_file_system();
std::unique_ptr<::rocksdb::Env> make_dftracer_env(
    const std::shared_ptr<::rocksdb::FileSystem>& file_system);

}  // namespace dftracer::utils::rocksdb

#endif  // DFTRACER_UTILS_CORE_ROCKSDB_FILESYSTEM_H
