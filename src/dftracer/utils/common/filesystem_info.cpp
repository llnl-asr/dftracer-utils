// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/filesystem_info.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/vector.h>
#include <sys/stat.h>

#ifdef __linux__
#include <sys/vfs.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
#include <sys/mount.h>
#include <sys/param.h>
#endif

#ifdef DFTRACER_UTILS_HAVE_LUSTREAPI
#include <lustre/lustreapi.h>

#include <cstdint>
#endif

namespace dftracer::utils::common {

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
            return FilesystemKind::Tmpfs;
        case NFS_MAGIC:
            return FilesystemKind::Nfs;
        case LUSTRE_MAGIC:
            return FilesystemKind::Lustre;
        case GPFS_MAGIC:
            return FilesystemKind::Gpfs;
        case BEEGFS_MAGIC:
            return FilesystemKind::Beegfs;
        default:
            return FilesystemKind::Local;
    }
}
#endif

// Classify a mount-table filesystem type name. Recognized disk filesystems map
// to LOCAL, tmpfs/ramfs to TMPFS, known network stores to their kind, and
// pseudo/kernel filesystems (proc, sysfs, ...) to UNKNOWN so they are never
// mistaken for usable local storage.
FilesystemKind classify_fstype_name(dstd::StringView name) noexcept {
    if (name == "tmpfs" || name == "ramfs") return FilesystemKind::Tmpfs;
    if (name == "nfs" || name == "nfs4") return FilesystemKind::Nfs;
    if (name == "lustre") return FilesystemKind::Lustre;
    if (name == "gpfs") return FilesystemKind::Gpfs;
    if (name == "beegfs") return FilesystemKind::Beegfs;
    if (name == "ext2" || name == "ext3" || name == "ext4" || name == "xfs" ||
        name == "btrfs" || name == "f2fs" || name == "zfs" || name == "jfs" ||
        name == "reiserfs" || name == "ntfs" || name == "vfat" ||
        name == "exfat" || name == "overlay" || name == "fuseblk" ||
        name == "apfs" || name == "hfs" || name == "ufs" || name == "msdos")
        return FilesystemKind::Local;
    return FilesystemKind::Unknown;
}

dstd::Path probe_path(dstd::StringView path) noexcept {
    const auto found = dstd::fs::exists(dstd::PathView(path));
    if (found && *found) return dstd::Path(path);
    const dstd::PathView parent = dstd::PathView(path).parent();
    if (parent.empty()) return dstd::Path(".");
    const auto parent_found = dstd::fs::exists(parent);
    if (parent_found && *parent_found) return dstd::Path(parent.str());
    return dstd::Path(".");
}

bool same_device(dstd::CStringView a, dstd::CStringView b) noexcept {
    const auto sa = dstd::fs::stat(dstd::PathView(a));
    const auto sb = dstd::fs::stat(dstd::PathView(b));
    if (!sa || !sb) return false;
    return sa->dev == sb->dev;
}

// Decode /proc/mounts octal escapes (e.g. "\040" for space).
[[maybe_unused]] dstd::String unescape_mount_field(dstd::StringView in) {
    dstd::String out;
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

constexpr bool is_field_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
           c == '\r';
}

// The next whitespace-delimited field of `line`, consumed from its front.
dstd::StringView next_field(dstd::StringView& line) noexcept {
    std::size_t begin = 0;
    while (begin < line.size() && is_field_space(line[begin])) ++begin;
    std::size_t end = begin;
    while (end < line.size() && !is_field_space(line[end])) ++end;
    const dstd::StringView field = line.substr(begin, end - begin);
    line.remove_prefix(end);
    return field;
}

// Appends one MountInfo per `<spec> <mountpoint> <fstype> ...` line of a
// /proc/mounts style table. A line with fewer than three fields is skipped.
[[maybe_unused]] void parse_mount_table(dstd::StringView text,
                                        dstd::Vector<MountInfo>& mounts) {
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        dstd::StringView line = text.substr(0, newline);
        text.remove_prefix(newline == dstd::StringView::npos ? text.size()
                                                             : newline + 1);
        const dstd::StringView spec = next_field(line);
        const dstd::StringView mountpoint = next_field(line);
        const dstd::StringView fstype = next_field(line);
        if (spec.empty() || mountpoint.empty() || fstype.empty()) continue;
        mounts.push_back({dstd::Path(unescape_mount_field(mountpoint)),
                          classify_fstype_name(fstype)});
    }
}

}  // namespace

FilesystemKind filesystem_kind(dstd::CStringView path) noexcept {
    [[maybe_unused]] const dstd::Path target = probe_path(path.view());
#if defined(__linux__)
    struct statfs st{};
    if (::statfs(target.c_str(), &st) != 0) return FilesystemKind::Unknown;
    return classify_magic(static_cast<unsigned long>(st.f_type));
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
    struct statfs st{};
    if (::statfs(target.c_str(), &st) != 0) return FilesystemKind::Unknown;
    return classify_fstype_name(st.f_fstypename);
#else
    return FilesystemKind::Unknown;
#endif
}

dstd::Path find_mountpoint(dstd::CStringView path) noexcept {
    const auto absolute =
        dstd::fs::absolute(dstd::PathView(probe_path(path.view()).str()));
    if (!absolute) return {};
    dstd::Path cur = dstd::normalize(absolute->view());
    // Climb until the parent crosses a device boundary.
    while (cur.str() != cur.view().root()) {
        const dstd::PathView parent = cur.view().parent();
        if (parent.empty() || parent.str() == cur.str()) break;
        const dstd::Path parent_path(parent.str());
        if (!same_device(cur, parent_path)) return cur;
        cur = parent_path;
    }
    return cur;
}

dstd::Vector<MountInfo> list_mounts() noexcept {
    dstd::Vector<MountInfo> mounts;
#if defined(__linux__)
    const auto table = dstd::fs::read_file(dstd::PathView("/proc/mounts"));
    if (table) parse_mount_table(*table, mounts);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
    struct statfs* buf = nullptr;
    const int n = ::getmntinfo(&buf, MNT_NOWAIT);
    for (int i = 0; i < n; ++i)
        mounts.push_back({dstd::Path(buf[i].f_mntonname),
                          classify_fstype_name(buf[i].f_fstypename)});
#endif
    return mounts;
}

StripeInfo lustre_stripe([[maybe_unused]] dstd::CStringView path) noexcept {
    StripeInfo info;
#ifdef DFTRACER_UTILS_HAVE_LUSTREAPI
    // Every value above this is a layout sentinel (LLAPI_LAYOUT_DEFAULT, the
    // raid0 or wide pattern markers), never a byte count.
    constexpr std::uint64_t MAX_STRIPE_BYTES = std::uint64_t{1} << 40;

    const dstd::Path probe = probe_path(path.view());
    // The layout, not a single getstripe ioctl: a progressive file layout
    // (PFL) has several components with a stripe size each.
    struct llapi_layout* layout =
        llapi_layout_get_by_path(probe.c_str(), LLAPI_LAYOUT_GET_EXPECTED);
    if (!layout) return info;
    for (int rc = llapi_layout_comp_use(layout, LLAPI_LAYOUT_COMP_USE_FIRST);
         rc == 0;
         rc = llapi_layout_comp_use(layout, LLAPI_LAYOUT_COMP_USE_NEXT)) {
        std::uint64_t stripe_size = 0;
        if (llapi_layout_stripe_size_get(layout, &stripe_size) != 0) continue;
        if (stripe_size == 0 || stripe_size >= MAX_STRIPE_BYTES) continue;
        if (stripe_size <= info.size) continue;
        std::uint64_t stripe_count = 0;
        if (llapi_layout_stripe_count_get(layout, &stripe_count) != 0)
            stripe_count = 0;
        info.size = static_cast<std::size_t>(stripe_size);
        info.count = static_cast<std::size_t>(stripe_count);
    }
    llapi_layout_free(layout);
#endif
    return info;
}

}  // namespace dftracer::utils::common
