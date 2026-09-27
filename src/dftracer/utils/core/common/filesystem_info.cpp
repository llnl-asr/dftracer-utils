#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/filesystem_info.h>
#include <sys/stat.h>

#ifdef __linux__
#include <sys/vfs.h>

#include <fstream>
#include <sstream>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
#include <sys/mount.h>
#include <sys/param.h>
#endif

#ifdef DFTRACER_UTILS_HAVE_LUSTREAPI
#include <lustre/lustreapi.h>

#include <cstdlib>
#endif

#include <string>
#include <string_view>
#include <system_error>

namespace dftracer::utils {

namespace {

#ifdef __linux__
// From linux/magic.h; inlined to avoid a hard kernel-header dep.
constexpr unsigned long TMPFS_MAGIC = 0x01021994;
constexpr unsigned long RAMFS_MAGIC = 0x858458f6;
constexpr unsigned long NFS_MAGIC = 0x6969;
constexpr unsigned long LUSTRE_MAGIC = 0x0BD00BD0;
constexpr unsigned long GPFS_MAGIC = 0x47504653;  // "GPFS"
constexpr unsigned long BEEGFS_MAGIC = 0x19830326;

FilesystemKind classify_magic(unsigned long magic) noexcept {
    switch (magic) {
        case TMPFS_MAGIC:
        case RAMFS_MAGIC:
            return FilesystemKind::TMPFS;
        case NFS_MAGIC:
            return FilesystemKind::NFS;
        case LUSTRE_MAGIC:
            return FilesystemKind::LUSTRE;
        case GPFS_MAGIC:
            return FilesystemKind::GPFS;
        case BEEGFS_MAGIC:
            return FilesystemKind::BEEGFS;
        default:
            return FilesystemKind::LOCAL;
    }
}
#endif

// Classify a mount-table filesystem type name. Recognized disk filesystems map
// to LOCAL, tmpfs/ramfs to TMPFS, known network stores to their kind, and
// pseudo/kernel filesystems (proc, sysfs, ...) to UNKNOWN so they are never
// mistaken for usable local storage.
FilesystemKind classify_fstype_name(std::string_view name) noexcept {
    if (name == "tmpfs" || name == "ramfs") return FilesystemKind::TMPFS;
    if (name == "nfs" || name == "nfs4") return FilesystemKind::NFS;
    if (name == "lustre") return FilesystemKind::LUSTRE;
    if (name == "gpfs") return FilesystemKind::GPFS;
    if (name == "beegfs") return FilesystemKind::BEEGFS;
    if (name == "ext2" || name == "ext3" || name == "ext4" || name == "xfs" ||
        name == "btrfs" || name == "f2fs" || name == "zfs" || name == "jfs" ||
        name == "reiserfs" || name == "ntfs" || name == "vfat" ||
        name == "exfat" || name == "overlay" || name == "fuseblk" ||
        name == "apfs" || name == "hfs" || name == "ufs" || name == "msdos")
        return FilesystemKind::LOCAL;
    return FilesystemKind::UNKNOWN;
}

std::string probe_path(const std::string& path) noexcept {
    std::error_code ec;
    if (fs::exists(path, ec)) return path;
    auto parent = fs::path(path).parent_path();
    if (parent.empty()) return std::string(".");
    if (fs::exists(parent, ec)) return parent.string();
    return std::string(".");
}

bool same_device(const std::string& a, const std::string& b) noexcept {
    struct stat sa{};
    struct stat sb{};
    if (::stat(a.c_str(), &sa) != 0 || ::stat(b.c_str(), &sb) != 0)
        return false;
    return sa.st_dev == sb.st_dev;
}

#ifdef __linux__
// Decode /proc/mounts octal escapes (e.g. "\040" for space).
std::string unescape_mount_field(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 3 < in.size() && in[i + 1] >= '0' &&
            in[i + 1] <= '7') {
            out.push_back(static_cast<char>((in[i + 1] - '0') * 64 +
                                            (in[i + 2] - '0') * 8 +
                                            (in[i + 3] - '0')));
            i += 3;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}
#endif

}  // namespace

FilesystemKind filesystem_kind(const std::string& path) noexcept {
    [[maybe_unused]] const std::string target = probe_path(path);
#if defined(__linux__)
    struct statfs st{};
    if (::statfs(target.c_str(), &st) != 0) return FilesystemKind::UNKNOWN;
    return classify_magic(static_cast<unsigned long>(st.f_type));
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
    struct statfs st{};
    if (::statfs(target.c_str(), &st) != 0) return FilesystemKind::UNKNOWN;
    return classify_fstype_name(st.f_fstypename);
#else
    return FilesystemKind::UNKNOWN;
#endif
}

std::string find_mountpoint(const std::string& path) noexcept {
    std::error_code ec;
    fs::path cur = fs::absolute(fs::path(probe_path(path)), ec);
    if (ec) return {};
    cur = cur.lexically_normal();
    // Climb until the parent crosses a device boundary.
    while (cur.has_relative_path() && cur != cur.root_path()) {
        fs::path parent = cur.parent_path();
        if (parent.empty() || parent == cur) break;
        if (!same_device(cur.string(), parent.string())) return cur.string();
        cur = parent;
    }
    return cur.string();
}

std::vector<MountInfo> list_mounts() noexcept {
    std::vector<MountInfo> mounts;
#if defined(__linux__)
    std::ifstream f("/proc/mounts");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string spec, mountpoint, fstype;
        if (!(ls >> spec >> mountpoint >> fstype)) continue;
        mounts.push_back(
            {unescape_mount_field(mountpoint), classify_fstype_name(fstype)});
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
    struct statfs* buf = nullptr;
    const int n = ::getmntinfo(&buf, MNT_NOWAIT);
    for (int i = 0; i < n; ++i)
        mounts.push_back(
            {buf[i].f_mntonname, classify_fstype_name(buf[i].f_fstypename)});
#endif
    return mounts;
}

StripeInfo lustre_stripe([[maybe_unused]] const std::string& path) noexcept {
    StripeInfo info;
#ifdef DFTRACER_UTILS_HAVE_LUSTREAPI
    const std::string probe = probe_path(path);
    const std::size_t lum_size =
        sizeof(struct lov_user_md) +
        LOV_MAX_STRIPE_COUNT * sizeof(struct lov_user_ost_data_v1);
    auto* raw = std::calloc(1, lum_size);
    if (!raw) return info;
    auto* lum = reinterpret_cast<struct lov_user_md*>(raw);
    lum->lmm_magic = LOV_USER_MAGIC;
    if (llapi_file_get_stripe(probe.c_str(), lum) == 0) {
        info.size = static_cast<std::size_t>(lum->lmm_stripe_size);
        info.count = static_cast<std::size_t>(lum->lmm_stripe_count);
    }
    std::free(raw);
#endif
    return info;
}

}  // namespace dftracer::utils
