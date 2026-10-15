#ifndef DFTRACER_UTILS_COMMON_FILESYSTEM_INFO_H
#define DFTRACER_UTILS_COMMON_FILESYSTEM_INFO_H

#include <cstddef>
#include <string>
#include <vector>

namespace dftracer::utils {

enum class FilesystemKind {
    UNKNOWN,
    LOCAL,  ///< disk-backed local FS: ext4, xfs, btrfs, apfs, ...
    TMPFS,  ///< RAM-backed: tmpfs / ramfs (e.g. /dev/shm)
    NFS,
    LUSTRE,
    GPFS,
    BEEGFS,
};

/// True for networked or parallel stores (NFS/Lustre/GPFS/BeeGFS), where
/// metadata- and fsync-heavy work such as index construction is slow.
inline bool is_network_filesystem(FilesystemKind kind) noexcept {
    return kind == FilesystemKind::NFS || kind == FilesystemKind::LUSTRE ||
           kind == FilesystemKind::GPFS || kind == FilesystemKind::BEEGFS;
}

/// True for node-local filesystems (disk or RAM) that make good scratch space.
inline bool is_local_filesystem(FilesystemKind kind) noexcept {
    return kind == FilesystemKind::LOCAL || kind == FilesystemKind::TMPFS;
}

/// True for RAM-backed filesystems (tmpfs/ramfs). Fastest scratch, but its
/// capacity competes with process memory, so callers should size accordingly.
inline bool is_memory_filesystem(FilesystemKind kind) noexcept {
    return kind == FilesystemKind::TMPFS;
}

/// Classify the filesystem backing `path` via statfs. When `path` does not
/// exist yet its nearest existing parent is probed instead (a file inherits its
/// directory's filesystem on creation). Never throws; returns UNKNOWN only when
/// statfs itself fails or the platform is unsupported.
FilesystemKind filesystem_kind(const std::string& path) noexcept;

/// The mount point that backs `path` (the longest mounted prefix), or "" when
/// it cannot be determined. `path` need not exist; its nearest existing parent
/// is used. Never throws.
std::string find_mountpoint(const std::string& path) noexcept;

struct MountInfo {
    std::string mountpoint;
    FilesystemKind kind;
};

/// Every current mount and its filesystem kind, read from the OS mount table
/// (Linux /proc/mounts, BSD getmntinfo) without a statfs per entry so a stale
/// network mount cannot stall. Empty when the table is unavailable.
std::vector<MountInfo> list_mounts() noexcept;

struct StripeInfo {
    std::size_t size = 0;   ///< bytes per stripe, 0 if unknown/not striped
    std::size_t count = 0;  ///< number of stripes, 0 if unknown/not striped
};

/// Lustre stripe geometry for `path` (or its parent when absent). Returns an
/// all-zero StripeInfo off Lustre or when the query is unavailable.
StripeInfo lustre_stripe(const std::string& path) noexcept;

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_COMMON_FILESYSTEM_INFO_H
