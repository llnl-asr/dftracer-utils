#include <dftracer/utils/core/io/io_backend.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/sqlite/vfs.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <new>

namespace dftracer::utils::sqlite {

// Forward declarations of all VFS methods
static int dftracer_sqlite_vfs_open(sqlite3_vfs *pVfs, const char *zName,
                                    sqlite3_file *pFile, int flags,
                                    int *pOutFlags);
static int dftracer_sqlite_vfs_delete(sqlite3_vfs *pVfs, const char *zPath,
                                      int dirSync);
static int dftracer_sqlite_vfs_access(sqlite3_vfs *pVfs, const char *zPath,
                                      int flags, int *pResOut);
static int dftracer_sqlite_vfs_fullpathname(sqlite3_vfs *pVfs,
                                            const char *zName, int nOut,
                                            char *zOut);
static int dftracer_sqlite_vfs_get_last_error(sqlite3_vfs *pVfs, int nBuf,
                                              char *zBuf);

// Forward declarations of all io_methods
static int dftracer_sqlite_vfs_close(sqlite3_file *pFile);
static int dftracer_sqlite_vfs_read(sqlite3_file *pFile, void *buf, int amt,
                                    sqlite3_int64 offset);
static int dftracer_sqlite_vfs_write(sqlite3_file *pFile, const void *buf,
                                     int amt, sqlite3_int64 offset);
static int dftracer_sqlite_vfs_truncate(sqlite3_file *pFile,
                                        sqlite3_int64 size);
static int dftracer_sqlite_vfs_sync(sqlite3_file *pFile, int flags);
static int dftracer_sqlite_vfs_file_size(sqlite3_file *pFile,
                                         sqlite3_int64 *pSize);
static int dftracer_sqlite_vfs_lock(sqlite3_file *pFile, int eLock);
static int dftracer_sqlite_vfs_unlock(sqlite3_file *pFile, int eLock);
static int dftracer_sqlite_vfs_check_reserved_lock(sqlite3_file *pFile,
                                                   int *pResOut);
static int dftracer_sqlite_vfs_file_control(sqlite3_file *pFile, int op,
                                            void *pArg);
static int dftracer_sqlite_vfs_sector_size(sqlite3_file *pFile);
static int dftracer_sqlite_vfs_device_characteristics(sqlite3_file *pFile);
static int dftracer_sqlite_vfs_shm_map(sqlite3_file *pFile, int iRegion,
                                       int szRegion, int bExtend,
                                       void volatile **pp);
static int dftracer_sqlite_vfs_shm_lock(sqlite3_file *pFile, int offset, int n,
                                        int flags);
static void dftracer_sqlite_vfs_shm_barrier(sqlite3_file *pFile);
static int dftracer_sqlite_vfs_shm_unmap(sqlite3_file *pFile, int deleteFlag);
static int dftracer_sqlite_vfs_fetch(sqlite3_file *pFile, sqlite3_int64 offset,
                                     int amt, void **pp);
static int dftracer_sqlite_vfs_unfetch(sqlite3_file *pFile,
                                       sqlite3_int64 offset, void *p);

// Static io_methods struct (iVersion=3 for WAL + mmap)
static sqlite3_io_methods dftracer_sqlite_vfs_io_methods = {
    3,                                           // iVersion
    dftracer_sqlite_vfs_close,                   // xClose
    dftracer_sqlite_vfs_read,                    // xRead
    dftracer_sqlite_vfs_write,                   // xWrite
    dftracer_sqlite_vfs_truncate,                // xTruncate
    dftracer_sqlite_vfs_sync,                    // xSync
    dftracer_sqlite_vfs_file_size,               // xFileSize
    dftracer_sqlite_vfs_lock,                    // xLock
    dftracer_sqlite_vfs_unlock,                  // xUnlock
    dftracer_sqlite_vfs_check_reserved_lock,     // xCheckReservedLock
    dftracer_sqlite_vfs_file_control,            // xFileControl
    dftracer_sqlite_vfs_sector_size,             // xSectorSize
    dftracer_sqlite_vfs_device_characteristics,  // xDeviceCharacteristics
    dftracer_sqlite_vfs_shm_map,                 // xShmMap
    dftracer_sqlite_vfs_shm_lock,                // xShmLock
    dftracer_sqlite_vfs_shm_barrier,             // xShmBarrier
    dftracer_sqlite_vfs_shm_unmap,               // xShmUnmap
    dftracer_sqlite_vfs_fetch,                   // xFetch
    dftracer_sqlite_vfs_unfetch,                 // xUnfetch
};

// Static VFS instance and app data
static sqlite3_vfs dftracer_vfs_instance;
static DfTracerSqliteVfsAppData *dftracer_vfs_app_data = nullptr;
static bool dftracer_vfs_registered = false;

// ============================================================================
// sqlite3_io_methods implementations
// ============================================================================

static int dftracer_sqlite_vfs_close(sqlite3_file *pFile) {
    auto *vf = reinterpret_cast<DfTracerSqliteVfsFile *>(pFile);

    // Clean up SHM resources
    for (int i = 0; i < vf->n_shm_region; ++i) {
        if (vf->shm_regions[i] != nullptr) {
            ::munmap(vf->shm_regions[i], 32768);
            vf->shm_regions[i] = nullptr;
        }
    }
    if (vf->shm_fd >= 0) {
        ::close(vf->shm_fd);
        vf->shm_fd = -1;
    }

    if (vf->fd >= 0) {
        ::close(vf->fd);
        vf->fd = -1;
    }

    // Destroy placement-new'd string
    vf->path.~basic_string();

    return SQLITE_OK;
}

static int dftracer_sqlite_vfs_read(sqlite3_file *pFile, void *buf, int amt,
                                    sqlite3_int64 offset) {
    auto *vf = reinterpret_cast<DfTracerSqliteVfsFile *>(pFile);

    if (vf->backend == nullptr) {
        ssize_t n = ::pread(vf->fd, buf, static_cast<std::size_t>(amt),
                            static_cast<off_t>(offset));
        if (n == amt) return SQLITE_OK;
        if (n >= 0) {
            std::memset(static_cast<char *>(buf) + n, 0, amt - n);
            return SQLITE_IOERR_SHORT_READ;
        }
        return SQLITE_IOERR_READ;
    }

    ssize_t result = vf->backend->submit_read_sync(
        vf->fd, buf, static_cast<std::size_t>(amt), static_cast<off_t>(offset));

    if (result == amt) return SQLITE_OK;
    if (result >= 0) {
        std::memset(static_cast<char *>(buf) + result, 0, amt - result);
        return SQLITE_IOERR_SHORT_READ;
    }
    return SQLITE_IOERR_READ;
}

static int dftracer_sqlite_vfs_write(sqlite3_file *pFile, const void *buf,
                                     int amt, sqlite3_int64 offset) {
    auto *vf = reinterpret_cast<DfTracerSqliteVfsFile *>(pFile);

    if (vf->backend == nullptr) {
        ssize_t n = ::pwrite(vf->fd, buf, static_cast<std::size_t>(amt),
                             static_cast<off_t>(offset));
        if (n == amt) return SQLITE_OK;
        return SQLITE_IOERR_WRITE;
    }

    ssize_t result = vf->backend->submit_write_sync(
        vf->fd, buf, static_cast<std::size_t>(amt), static_cast<off_t>(offset));

    if (result == amt) return SQLITE_OK;
    return SQLITE_IOERR_WRITE;
}

static int dftracer_sqlite_vfs_truncate(sqlite3_file *pFile,
                                        sqlite3_int64 size) {
    auto *vf = reinterpret_cast<DfTracerSqliteVfsFile *>(pFile);

    if (vf->backend == nullptr) {
        if (::ftruncate(vf->fd, static_cast<off_t>(size)) != 0) {
            return SQLITE_IOERR_TRUNCATE;
        }
        return SQLITE_OK;
    }

    int rc =
        vf->backend->submit_ftruncate_sync(vf->fd, static_cast<off_t>(size));
    if (rc != 0) return SQLITE_IOERR_TRUNCATE;
    return SQLITE_OK;
}

static int dftracer_sqlite_vfs_sync(sqlite3_file *pFile, int /*flags*/) {
    auto *vf = reinterpret_cast<DfTracerSqliteVfsFile *>(pFile);

    if (vf->backend == nullptr) {
        if (::fsync(vf->fd) != 0) return SQLITE_IOERR_FSYNC;
        return SQLITE_OK;
    }

    int rc = vf->backend->submit_fsync_sync(vf->fd);
    if (rc != 0) return SQLITE_IOERR_FSYNC;
    return SQLITE_OK;
}

static int dftracer_sqlite_vfs_file_size(sqlite3_file *pFile,
                                         sqlite3_int64 *pSize) {
    auto *vf = reinterpret_cast<DfTracerSqliteVfsFile *>(pFile);
    struct stat st;

    if (vf->backend == nullptr) {
        if (::fstat(vf->fd, &st) != 0) return SQLITE_IOERR_FSTAT;
        *pSize = st.st_size;
        return SQLITE_OK;
    }

    int rc = vf->backend->submit_fstat_sync(vf->fd, &st);
    if (rc != 0) return SQLITE_IOERR_FSTAT;
    *pSize = st.st_size;
    return SQLITE_OK;
}

static int dftracer_sqlite_vfs_lock(sqlite3_file *pFile, int eLock) {
    auto *vf = reinterpret_cast<DfTracerSqliteVfsFile *>(pFile);

    struct flock fl;
    std::memset(&fl, 0, sizeof(fl));

    if (eLock == SQLITE_LOCK_NONE) {
        return SQLITE_OK;
    }

    if (eLock == SQLITE_LOCK_SHARED) {
        fl.l_type = F_RDLCK;
    } else {
        fl.l_type = F_WRLCK;
    }
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (::fcntl(vf->fd, F_SETLK, &fl) == -1) {
        if (errno == EACCES || errno == EAGAIN) {
            return SQLITE_BUSY;
        }
        return SQLITE_IOERR_LOCK;
    }
    return SQLITE_OK;
}

static int dftracer_sqlite_vfs_unlock(sqlite3_file *pFile, int /*eLock*/) {
    auto *vf = reinterpret_cast<DfTracerSqliteVfsFile *>(pFile);

    struct flock fl;
    std::memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (::fcntl(vf->fd, F_SETLK, &fl) == -1) {
        return SQLITE_IOERR_UNLOCK;
    }
    return SQLITE_OK;
}

static int dftracer_sqlite_vfs_check_reserved_lock(sqlite3_file *pFile,
                                                   int *pResOut) {
    auto *vf = reinterpret_cast<DfTracerSqliteVfsFile *>(pFile);

    struct flock fl;
    std::memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 1;

    if (::fcntl(vf->fd, F_GETLK, &fl) == -1) {
        *pResOut = 0;
        return SQLITE_IOERR_CHECKRESERVEDLOCK;
    }

    *pResOut = (fl.l_type != F_UNLCK) ? 1 : 0;
    return SQLITE_OK;
}

static int dftracer_sqlite_vfs_file_control(sqlite3_file * /*pFile*/, int op,
                                            void * /*pArg*/) {
    if (op == SQLITE_FCNTL_LOCKSTATE) {
        return SQLITE_OK;
    }
    return SQLITE_NOTFOUND;
}

static int dftracer_sqlite_vfs_sector_size(sqlite3_file * /*pFile*/) {
    return 4096;
}

static int dftracer_sqlite_vfs_device_characteristics(
    sqlite3_file * /*pFile*/) {
    return SQLITE_IOCAP_ATOMIC512 | SQLITE_IOCAP_SAFE_APPEND;
}

// ============================================================================
// SHM methods (WAL shared memory)
// ============================================================================

static int dftracer_sqlite_vfs_shm_map(sqlite3_file *pFile, int iRegion,
                                       int szRegion, int bExtend,
                                       void volatile **pp) {
    auto *vf = reinterpret_cast<DfTracerSqliteVfsFile *>(pFile);

    if (iRegion >= 32) {
        *pp = nullptr;
        return SQLITE_IOERR;
    }

    // Open SHM file if not yet opened
    if (vf->shm_fd < 0) {
        std::string shm_path = vf->path + "-shm";
        int oflags = O_RDWR | O_CREAT;
        vf->shm_fd = ::open(shm_path.c_str(), oflags, 0644);
        if (vf->shm_fd < 0) {
            *pp = nullptr;
            return SQLITE_IOERR;
        }
    }

    // Extend file if needed
    off_t required_size =
        static_cast<off_t>(iRegion + 1) * static_cast<off_t>(szRegion);
    struct stat st;
    if (::fstat(vf->shm_fd, &st) != 0) {
        *pp = nullptr;
        return SQLITE_IOERR;
    }
    if (st.st_size < required_size) {
        if (!bExtend) {
            *pp = nullptr;
            return SQLITE_OK;
        }
        if (::ftruncate(vf->shm_fd, required_size) != 0) {
            *pp = nullptr;
            return SQLITE_IOERR;
        }
    }

    // Map the region if not already mapped
    if (iRegion >= vf->n_shm_region || vf->shm_regions[iRegion] == nullptr) {
        off_t map_offset =
            static_cast<off_t>(iRegion) * static_cast<off_t>(szRegion);
        void *mapped =
            ::mmap(nullptr, static_cast<std::size_t>(szRegion),
                   PROT_READ | PROT_WRITE, MAP_SHARED, vf->shm_fd, map_offset);
        if (mapped == MAP_FAILED) {
            *pp = nullptr;
            return SQLITE_IOERR;
        }
        vf->shm_regions[iRegion] = mapped;
        if (iRegion >= vf->n_shm_region) {
            vf->n_shm_region = iRegion + 1;
        }
    }

    *pp = vf->shm_regions[iRegion];
    return SQLITE_OK;
}

static int dftracer_sqlite_vfs_shm_lock(sqlite3_file *pFile, int offset, int n,
                                        int flags) {
    auto *vf = reinterpret_cast<DfTracerSqliteVfsFile *>(pFile);

    if (vf->shm_fd < 0) return SQLITE_IOERR;

    struct flock fl;
    std::memset(&fl, 0, sizeof(fl));

    if (flags & SQLITE_SHM_UNLOCK) {
        fl.l_type = F_UNLCK;
    } else if (flags & SQLITE_SHM_EXCLUSIVE) {
        fl.l_type = F_WRLCK;
    } else {
        fl.l_type = F_RDLCK;
    }
    fl.l_whence = SEEK_SET;
    fl.l_start = offset;
    fl.l_len = n;

    if (::fcntl(vf->shm_fd, F_SETLK, &fl) == -1) {
        if (errno == EACCES || errno == EAGAIN) {
            return SQLITE_BUSY;
        }
        return SQLITE_IOERR;
    }
    return SQLITE_OK;
}

static void dftracer_sqlite_vfs_shm_barrier(sqlite3_file * /*pFile*/) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static int dftracer_sqlite_vfs_shm_unmap(sqlite3_file *pFile, int deleteFlag) {
    auto *vf = reinterpret_cast<DfTracerSqliteVfsFile *>(pFile);

    for (int i = 0; i < vf->n_shm_region; ++i) {
        if (vf->shm_regions[i] != nullptr) {
            ::munmap(vf->shm_regions[i], 32768);
            vf->shm_regions[i] = nullptr;
        }
    }
    vf->n_shm_region = 0;

    if (vf->shm_fd >= 0) {
        ::close(vf->shm_fd);
        if (deleteFlag) {
            std::string shm_path = vf->path + "-shm";
            ::unlink(shm_path.c_str());
        }
        vf->shm_fd = -1;
    }

    return SQLITE_OK;
}

// ============================================================================
// mmap methods (version 3)
// ============================================================================

static int dftracer_sqlite_vfs_fetch(sqlite3_file * /*pFile*/,
                                     sqlite3_int64 /*offset*/, int /*amt*/,
                                     void **pp) {
    // Disable mmap — returning nullptr tells SQLite to use
    // xRead instead. This avoids tracking mmap sizes for munmap.
    *pp = nullptr;
    return SQLITE_OK;
}

static int dftracer_sqlite_vfs_unfetch(sqlite3_file * /*pFile*/,
                                       sqlite3_int64 /*offset*/, void *p) {
    (void)p;
    return SQLITE_OK;
}

// ============================================================================
// sqlite3_vfs implementations
// ============================================================================

static int dftracer_sqlite_vfs_open(sqlite3_vfs *pVfs, const char *zName,
                                    sqlite3_file *pFile, int flags,
                                    int *pOutFlags) {
    auto *app = static_cast<DfTracerSqliteVfsAppData *>(pVfs->pAppData);
    auto *vf = reinterpret_cast<DfTracerSqliteVfsFile *>(pFile);

    // Zero the sqlite3_file base (C struct, safe to memset)
    std::memset(&vf->base, 0, sizeof(vf->base));
    vf->backend = nullptr;
    vf->executor = nullptr;
    vf->fd = -1;
    vf->read_only = false;
    vf->shm_fd = -1;
    vf->n_shm_region = 0;
    for (int i = 0; i < 32; ++i) {
        vf->shm_regions[i] = nullptr;
    }

    // Placement-new the std::string (SQLite allocates raw memory)
    new (&vf->path) std::string(zName ? zName : "");

    vf->backend = app ? app->backend : nullptr;
    vf->executor = app ? app->executor : nullptr;
    vf->read_only = (flags & SQLITE_OPEN_READONLY) != 0;

    // Build open flags
    int oflags = 0;
    if (flags & SQLITE_OPEN_EXCLUSIVE) {
        oflags |= O_EXCL;
    }
    if (flags & SQLITE_OPEN_CREATE) {
        oflags |= O_CREAT;
    }
    if (flags & SQLITE_OPEN_READONLY) {
        oflags = O_RDONLY;
    } else if (flags & SQLITE_OPEN_READWRITE) {
        oflags |= O_RDWR;
    }

    // Handle temp/journal files without a name
    if (zName == nullptr) {
        char tmp_path[] = "/tmp/dftracer_sqlite_XXXXXX";
        vf->fd = ::mkstemp(tmp_path);
        if (vf->fd < 0) {
            vf->path.~basic_string();
            return SQLITE_CANTOPEN;
        }
        ::unlink(tmp_path);
        vf->path = tmp_path;
    } else {
        vf->fd = ::open(zName, oflags, 0644);
        if (vf->fd < 0) {
            vf->path.~basic_string();
            return SQLITE_CANTOPEN;
        }
    }

    if (pOutFlags != nullptr) {
        *pOutFlags = flags;
    }

    pFile->pMethods = &dftracer_sqlite_vfs_io_methods;
    return SQLITE_OK;
}

static int dftracer_sqlite_vfs_delete(sqlite3_vfs * /*pVfs*/, const char *zPath,
                                      int dirSync) {
    if (::unlink(zPath) != 0) {
        if (errno == ENOENT) return SQLITE_OK;
        return SQLITE_IOERR_DELETE;
    }

    if (dirSync) {
        // Sync the parent directory
        std::string dir(zPath);
        auto pos = dir.rfind('/');
        if (pos != std::string::npos) {
            dir.resize(pos);
            if (dir.empty()) dir = "/";
        } else {
            dir = ".";
        }
        int dfd = ::open(dir.c_str(), O_RDONLY);
        if (dfd >= 0) {
            ::fsync(dfd);
            ::close(dfd);
        }
    }

    return SQLITE_OK;
}

static int dftracer_sqlite_vfs_access(sqlite3_vfs * /*pVfs*/, const char *zPath,
                                      int flags, int *pResOut) {
    int mode = F_OK;
    if (flags == SQLITE_ACCESS_READWRITE) {
        mode = R_OK | W_OK;
    } else if (flags == SQLITE_ACCESS_READ) {
        mode = R_OK;
    }

    *pResOut = (::access(zPath, mode) == 0) ? 1 : 0;
    return SQLITE_OK;
}

static int dftracer_sqlite_vfs_fullpathname(sqlite3_vfs * /*pVfs*/,
                                            const char *zName, int nOut,
                                            char *zOut) {
    char *resolved = ::realpath(zName, nullptr);
    if (resolved != nullptr) {
        std::strncpy(zOut, resolved, static_cast<std::size_t>(nOut));
        zOut[nOut - 1] = '\0';
        ::free(resolved);
    } else {
        // If realpath fails (file doesn't exist yet), copy as-is
        std::strncpy(zOut, zName, static_cast<std::size_t>(nOut));
        zOut[nOut - 1] = '\0';
    }
    return SQLITE_OK;
}

static int dftracer_sqlite_vfs_get_last_error(sqlite3_vfs * /*pVfs*/, int nBuf,
                                              char *zBuf) {
    if (nBuf > 0 && zBuf != nullptr) {
        std::strncpy(zBuf, std::strerror(errno),
                     static_cast<std::size_t>(nBuf));
        zBuf[nBuf - 1] = '\0';
    }
    return errno;
}

// ============================================================================
// VFS Registration
// ============================================================================

void register_dftracer_sqlite_vfs(io::IoBackend *backend, Executor *executor) {
    if (dftracer_vfs_registered) return;

    sqlite3_vfs *default_vfs = sqlite3_vfs_find(nullptr);

    dftracer_vfs_app_data = new DfTracerSqliteVfsAppData{backend, executor};

    std::memset(&dftracer_vfs_instance, 0, sizeof(dftracer_vfs_instance));
    dftracer_vfs_instance.iVersion = 3;
    dftracer_vfs_instance.szOsFile =
        static_cast<int>(sizeof(DfTracerSqliteVfsFile));
    dftracer_vfs_instance.mxPathname = 512;
    dftracer_vfs_instance.pNext = nullptr;
    dftracer_vfs_instance.zName = "dftracer_sqlite";
    dftracer_vfs_instance.pAppData = dftracer_vfs_app_data;
    dftracer_vfs_instance.xOpen = dftracer_sqlite_vfs_open;
    dftracer_vfs_instance.xDelete = dftracer_sqlite_vfs_delete;
    dftracer_vfs_instance.xAccess = dftracer_sqlite_vfs_access;
    dftracer_vfs_instance.xFullPathname = dftracer_sqlite_vfs_fullpathname;
    dftracer_vfs_instance.xGetLastError = dftracer_sqlite_vfs_get_last_error;

    // Delegate time/random/sleep to default VFS
    if (default_vfs != nullptr) {
        dftracer_vfs_instance.xRandomness = default_vfs->xRandomness;
        dftracer_vfs_instance.xSleep = default_vfs->xSleep;
        dftracer_vfs_instance.xCurrentTime = default_vfs->xCurrentTime;
        dftracer_vfs_instance.xCurrentTimeInt64 =
            default_vfs->xCurrentTimeInt64;
    }

    sqlite3_vfs_register(&dftracer_vfs_instance, 0);
    dftracer_vfs_registered = true;
}

void unregister_dftracer_sqlite_vfs() {
    if (!dftracer_vfs_registered) return;

    sqlite3_vfs_unregister(&dftracer_vfs_instance);

    delete dftracer_vfs_app_data;
    dftracer_vfs_app_data = nullptr;
    dftracer_vfs_registered = false;
}

}  // namespace dftracer::utils::sqlite
