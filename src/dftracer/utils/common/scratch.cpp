// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem_info.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/scratch.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/memory/budget.h>
#include <pwd.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <utility>

namespace dftracer::utils::common {

namespace {

dstd::String env_str(const char* name) {
    auto v = Env::get(name);
    return v ? dstd::String(*v) : dstd::String();
}

dstd::String lower(dstd::String s) {
    detail::lower_ascii(s);
    return s;
}

bool writable_dir(dstd::PathView dir) {
    // A failed create_directories shows up as a missing directory below.
    dstd::fs::create_directories(dir).discard();
    const auto is_dir = dstd::fs::is_directory(dir);
    if (!is_dir || !*is_dir) return false;
    const dstd::Path probe =
        dir / str_cat(".dftracer_probe_", static_cast<long>(::getpid()));
    if (!dstd::fs::create_directory(probe.view())) return false;
    dstd::fs::remove(probe.view()).discard();
    return true;
}

// RAM-backed tmpfs (e.g. /dev/shm) is fastest and needs no fsync; node-local
// disk is the large-capacity fallback. Larger free space breaks ties.
int locality_tier(FilesystemKind kind) {
    if (kind == FilesystemKind::Tmpfs) return 2;
    if (kind == FilesystemKind::Local) return 1;
    return 0;
}

constexpr std::uint64_t MIN_HEADROOM_BYTES = 512ULL << 20;

// A mount can hold an index of `needed` bytes with headroom to spare. A tmpfs
// mount's statvfs free is only its size cap; its pages can spill to swap, but
// building an index through swap would be slow and risk thrashing, so bound it
// by real free RAM and never use more than half of that.
bool mount_has_room(dstd::PathView mount, FilesystemKind kind,
                    std::uint64_t needed) {
    if (needed == 0) return true;
    const auto space = dstd::fs::space(mount);
    if (!space) return false;
    std::uint64_t avail = space->available;
    if (is_memory_filesystem(kind)) {
        const std::uint64_t ram = memory::detect_available_memory();
        if (ram != 0 && ram < avail) avail = ram;
        if (needed * 2 > avail) return false;
    }
    return avail >= needed + MIN_HEADROOM_BYTES;
}

dstd::Path compute_scratch_root(std::uint64_t needed) {
    const dstd::String forced = env_str("DFTRACER_UTILS_INDEX_SCRATCH");
    if (!forced.empty()) {
        const dstd::String f = lower(forced);
        if (f == "off" || f == "none" || f == "0" || f == "false") return {};
        const dstd::Path forced_path{dstd::StringView(forced)};
        if (!writable_dir(forced_path.view())) return {};
        return mount_has_room(forced_path.view(), filesystem_kind(forced_path),
                              needed)
                   ? forced_path
                   : dstd::Path();
    }

    dstd::Path best;
    int best_tier = 0;
    std::uint64_t best_avail = 0;
    for (const auto& m : list_mounts()) {
        const int tier = locality_tier(m.kind);
        if (tier == 0) continue;
        if (!writable_dir(m.mountpoint.view())) continue;
        if (!mount_has_room(m.mountpoint.view(), m.kind, needed)) continue;
        const auto space = dstd::fs::space(m.mountpoint.view());
        if (!space) continue;
        if (tier > best_tier ||
            (tier == best_tier && space->available > best_avail)) {
            best_tier = tier;
            best_avail = space->available;
            best = m.mountpoint;
        }
    }
    return best;
}

dstd::String current_user() {
    dstd::String u = env_str("USER");
    if (u.empty()) u = env_str("LOGNAME");
    if (u.empty()) {
        if (struct passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_name)
            u = pw->pw_name;
    }
    return u.empty() ? dstd::String("nobody") : u;
}

std::atomic<std::uint64_t> g_session_counter{0};

dstd::Path compute_local_spill_root() {
    dstd::Path best;
    std::uint64_t best_avail = 0;
    for (const auto& m : list_mounts()) {
        if (m.kind != FilesystemKind::Local) continue;
        if (!writable_dir(m.mountpoint.view())) continue;
        const auto space = dstd::fs::space(m.mountpoint.view());
        if (!space || space->available <= best_avail) continue;
        best_avail = space->available;
        best = m.mountpoint;
    }
    // No mount root is writable by an ordinary user (the usual case: users
    // own directories under a mount, not the mount itself), so try the
    // per-user places that live on disk: /var/tmp, then ~/.cache.
    if (best.empty()) {
        dstd::Vector<dstd::Path> candidates;
        candidates.emplace_back("/var/tmp");
        const dstd::String home = env_str("HOME");
        if (!home.empty())
            candidates.push_back(dstd::PathView(home) / ".cache");
        for (const auto& c : candidates) {
            if (filesystem_kind(c) == FilesystemKind::Local &&
                writable_dir(c.view())) {
                best = c;
                break;
            }
        }
    }
    if (best.empty()) return {};
    dstd::Path joined = best;
    joined /= current_user();
    joined /= "dftracer-utils";
    joined /= "spill";
    const dstd::Path dir = dstd::normalize(joined.view());
    if (!dstd::fs::create_directories(dir.view())) return {};
    const auto is_dir = dstd::fs::is_directory(dir.view());
    if (!is_dir || !*is_dir) return {};
    return dir;
}

struct Roots {
    dstd::Path scratch;
    dstd::Path local_spill;
};

Roots& roots() {
    Roots& held = dstd::Singleton<Roots>::get();
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, [] {
            Roots& dropped = dstd::Singleton<Roots>::get();
            // Swapped out so the old buffers are destroyed, since assigning
            // an empty value keeps the heap buffer in libstdc++.
            (void)std::exchange(dropped.scratch, dstd::Path());
            (void)std::exchange(dropped.local_spill, dstd::Path());
        });
        return true;
    }();
    return held;
}

}  // namespace

const dstd::Path& scratch_root() noexcept {
    [[maybe_unused]] static const bool computed = [] {
        roots().scratch = compute_scratch_root(0);
        return true;
    }();
    return roots().scratch;
}

const dstd::Path& local_spill_root() noexcept {
    [[maybe_unused]] static const bool computed = [] {
        roots().local_spill = compute_local_spill_root();
        return true;
    }();
    return roots().local_spill;
}

bool should_stage(dstd::CStringView dest) noexcept {
    const dstd::String mode = lower(env_str("DFTRACER_UTILS_INDEX_STAGE"));
    if (mode == "never" || mode == "off" || mode == "0") return false;
    // Cheap gate first: a local destination is the common case and never
    // stages, so skip the mount probing and mountpoint walks entirely.
    if (mode != "always" && !is_network_filesystem(filesystem_kind(dest)))
        return false;
    const dstd::Path& root = scratch_root();
    if (root.empty()) return false;
    // No gain when scratch and destination share a mount.
    return find_mountpoint(root).str() != find_mountpoint(dest).str();
}

ScratchSession::ScratchSession(std::uint64_t estimated_bytes) {
    const dstd::Path root = compute_scratch_root(estimated_bytes);
    if (root.empty()) return;
    const std::uint64_t id =
        g_session_counter.fetch_add(1, std::memory_order_relaxed);
    dstd::Path joined = root;
    joined /= current_user();
    joined /= "dftracer-utils";
    joined /= str_cat(static_cast<long>(::getpid()), "_", id);
    const dstd::Path dir = dstd::normalize(joined.view());
    if (!dstd::fs::create_directories(dir.view())) return;
    const auto is_dir = dstd::fs::is_directory(dir.view());
    if (is_dir && *is_dir) dir_ = dir;
}

ScratchSession::ScratchSession(ScratchSession&& other) noexcept
    : dir_(std::move(other.dir_)), keep_(other.keep_) {
    other.dir_ = dstd::Path();
}

ScratchSession::~ScratchSession() {
    if (dir_.empty() || keep_) return;
    dstd::fs::remove_all(dir_.view()).discard();
}

dstd::Optional<ScratchSession> staged_build_scratch(
    const dstd::Vector<dstd::String>& files) {
    std::uint64_t input_bytes = 0;
    for (const auto& file : files) {
        InputFileStat st;
        if (stat_input(file, st)) input_bytes += st.size;
    }
    const int factor =
        Env::get<int>("DFTRACER_UTILS_INDEX_SIZE_FACTOR").value_or(3);
    dstd::Optional<ScratchSession> scratch(
        std::in_place,
        input_bytes * static_cast<std::uint64_t>(factor > 0 ? factor : 1));
    if (!scratch->valid()) scratch.reset();
    return scratch;
}

void publish_path(dstd::PathView src, dstd::PathView dst) {
    const dstd::PathView parent = dst.parent();
    if (!parent.empty()) dstd::fs::create_directories(parent).discard();
    dstd::fs::remove_all(dst).discard();

    if (dstd::fs::rename(src, dst)) return;

    // Cross-device rename fails; fall back to copy + remove.
    const auto copied =
        dstd::fs::copy_tree(src, dst, dstd::fs::CopyMode::Overwrite);
    if (!copied) {
        throw DFTUtilsException(
            ErrorCode::Io, str_cat("failed to publish ", src.str(), " to ",
                                   dst.str(), ": ", copied.error().message()));
    }
    dstd::fs::remove_all(src).discard();
}

}  // namespace dftracer::utils::common
