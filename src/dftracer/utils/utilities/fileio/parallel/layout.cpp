#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/fileio/parallel/layout.h>

#ifdef __linux__
#include <sys/vfs.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
#include <sys/mount.h>
#include <sys/param.h>

#include <string_view>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#ifdef DFTRACER_UTILS_HAVE_LUSTREAPI
#include <lustre/lustreapi.h>
#endif

namespace dftracer::utils::utilities::fileio::parallel {

namespace {

#ifdef __linux__
// From linux/magic.h; inlined to avoid a hard kernel-header dep.
constexpr unsigned long NFS_MAGIC = 0x6969;
constexpr unsigned long LUSTRE_MAGIC = 0x0BD00BD0;
constexpr unsigned long GPFS_MAGIC = 0x47504653;  // "GPFS"
constexpr unsigned long BEEGFS_MAGIC = 0x19830326;

FilesystemKind classify_magic(unsigned long magic) noexcept {
    switch (magic) {
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
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
FilesystemKind classify_fstype(const char* fstype) noexcept {
    if (fstype == nullptr) return FilesystemKind::LOCAL;
    const std::string_view name(fstype);
    if (name == "nfs") return FilesystemKind::NFS;
    if (name == "lustre") return FilesystemKind::LUSTRE;
    if (name == "gpfs") return FilesystemKind::GPFS;
    if (name == "beegfs") return FilesystemKind::BEEGFS;
    return FilesystemKind::LOCAL;
}
#endif

std::string probe_path(const std::string& path) noexcept {
    std::error_code ec;
    if (fs::exists(path, ec)) return path;
    auto parent = fs::path(path).parent_path();
    if (parent.empty()) return std::string(".");
    if (fs::exists(parent, ec)) return parent.string();
    return std::string(".");
}

void query_lustre_stripe(const std::string& probe, LayoutInfo& info) noexcept {
#ifdef DFTRACER_UTILS_HAVE_LUSTREAPI
    // When the target file does not exist yet we fall back to the parent dir;
    // the file inherits the directory's default stripe on creation.
    const std::size_t lum_size =
        sizeof(struct lov_user_md) +
        LOV_MAX_STRIPE_COUNT * sizeof(struct lov_user_ost_data_v1);
    auto* raw = std::calloc(1, lum_size);
    if (!raw) return;
    auto* lum = reinterpret_cast<struct lov_user_md*>(raw);
    lum->lmm_magic = LOV_USER_MAGIC;
    if (llapi_file_get_stripe(probe.c_str(), lum) == 0) {
        info.stripe_size = static_cast<std::size_t>(lum->lmm_stripe_size);
        info.stripe_count = static_cast<std::size_t>(lum->lmm_stripe_count);
    }
    std::free(raw);
#else
    (void)probe;
    (void)info;
#endif
}

}  // namespace

LayoutInfo detect_layout(const std::string& path) noexcept {
    LayoutInfo info{};
    info.layout = FileLayout::STRIPED;
    info.fs = FilesystemKind::UNKNOWN;
    info.stripe_size = 0;
    info.stripe_count = 0;

    const auto target = probe_path(path);
#if defined(__linux__)
    struct statfs st{};
    if (::statfs(target.c_str(), &st) != 0) {
        return info;
    }
    info.fs = classify_magic(static_cast<unsigned long>(st.f_type));
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
    struct statfs st{};
    if (::statfs(target.c_str(), &st) != 0) {
        return info;
    }
    info.fs = classify_fstype(st.f_fstypename);
#else
    (void)target;
#endif
    if (info.fs == FilesystemKind::NFS) {
        info.layout = FileLayout::SHARDED;
    }
    if (info.fs == FilesystemKind::LUSTRE) {
        query_lustre_stripe(target, info);
    }
    return info;
}

WriterSizing compute_writer_sizing(const LayoutInfo& info,
                                   std::size_t baseline_workers,
                                   std::size_t default_flush_bytes,
                                   std::size_t buffer_headroom_bytes,
                                   bool padded_layout) noexcept {
    WriterSizing s{};
    s.num_workers = baseline_workers == 0 ? 1 : baseline_workers;
    if (!padded_layout && info.stripe_count > 0) {
        s.num_workers = std::min(s.num_workers, info.stripe_count);
    }
    if (padded_layout && info.stripe_size > 0) {
        // Uncompressed flush sized to one stripe; compressed fits easily.
        s.flush_threshold = info.stripe_size;
    } else {
        s.flush_threshold = std::max(default_flush_bytes, info.stripe_size);
    }
    s.buffer_capacity = s.flush_threshold + buffer_headroom_bytes;
    return s;
}

}  // namespace dftracer::utils::utilities::fileio::parallel
