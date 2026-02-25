#ifndef DFTRACER_UTILS_CORE_SQLITE_VFS_H
#define DFTRACER_UTILS_CORE_SQLITE_VFS_H

#include <sqlite3.h>

#include <string>

namespace dftracer::utils::io {
class IoBackend;
}  // namespace dftracer::utils::io

namespace dftracer::utils {
class Executor;
}  // namespace dftracer::utils

namespace dftracer::utils::sqlite {

struct DfTracerSqliteVfsAppData {
    io::IoBackend* backend;
    Executor* executor;
};

struct DfTracerSqliteVfsFile {
    sqlite3_file base;  // Must be first — SQLite casts to this
    io::IoBackend* backend;
    Executor* executor;
    int fd;
    bool read_only;
    std::string path;
    int shm_fd;
    int n_shm_region;
    void* shm_regions[32];
};

void register_dftracer_sqlite_vfs(io::IoBackend* backend, Executor* executor);
void unregister_dftracer_sqlite_vfs();

}  // namespace dftracer::utils::sqlite

#endif  // DFTRACER_UTILS_CORE_SQLITE_VFS_H
