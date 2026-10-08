#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/filesystem_info.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/common/scratch.h>
#include <dftracer/utils/core/env.h>
#include <pwd.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <string>
#include <system_error>

namespace dftracer::utils {

namespace {

std::string env_str(const char* name) {
    auto v = Env::get(name);
    return v ? std::string(*v) : std::string();
}

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool writable_dir(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) return false;
    const std::string probe =
        dir + "/.dftracer_probe_" + std::to_string(::getpid());
    fs::create_directory(probe, ec);
    if (ec) return false;
    fs::remove(probe, ec);
    return true;
}

// RAM-backed tmpfs (e.g. /dev/shm) is fastest and needs no fsync; node-local
// disk is the large-capacity fallback. Larger free space breaks ties.
int locality_tier(FilesystemKind kind) {
    if (kind == FilesystemKind::TMPFS) return 2;
    if (kind == FilesystemKind::LOCAL) return 1;
    return 0;
}

constexpr std::uint64_t MIN_HEADROOM_BYTES = 512ULL << 20;

// A mount can hold an index of `needed` bytes with headroom to spare. A tmpfs
// mount's statvfs free is only its size cap; its pages can spill to swap, but
// building an index through swap would be slow and risk thrashing, so bound it
// by real free RAM and never use more than half of that.
bool mount_has_room(const std::string& mount, FilesystemKind kind,
                    std::uint64_t needed) {
    if (needed == 0) return true;
    std::error_code ec;
    const auto space = fs::space(mount, ec);
    if (ec) return false;
    std::uint64_t avail = static_cast<std::uint64_t>(space.available);
    if (is_memory_filesystem(kind)) {
        const std::uint64_t ram = detect_available_memory();
        if (ram != 0 && ram < avail) avail = ram;
        if (needed * 2 > avail) return false;
    }
    return avail >= needed + MIN_HEADROOM_BYTES;
}

std::string compute_scratch_root(std::uint64_t needed) {
    const std::string forced = env_str("DFTRACER_INDEX_SCRATCH");
    if (!forced.empty()) {
        const std::string f = lower(forced);
        if (f == "off" || f == "none" || f == "0" || f == "false") return {};
        if (!writable_dir(forced)) return {};
        return mount_has_room(forced, filesystem_kind(forced), needed)
                   ? forced
                   : std::string();
    }

    std::string best;
    int best_tier = 0;
    std::uintmax_t best_avail = 0;
    for (const auto& m : list_mounts()) {
        const int tier = locality_tier(m.kind);
        if (tier == 0) continue;
        if (!writable_dir(m.mountpoint)) continue;
        if (!mount_has_room(m.mountpoint, m.kind, needed)) continue;
        std::error_code ec;
        const auto space = fs::space(m.mountpoint, ec);
        if (ec) continue;
        if (tier > best_tier ||
            (tier == best_tier && space.available > best_avail)) {
            best_tier = tier;
            best_avail = space.available;
            best = m.mountpoint;
        }
    }
    return best;
}

std::string compute_local_spill_root();

std::string current_user() {
    std::string u = env_str("USER");
    if (u.empty()) u = env_str("LOGNAME");
    if (u.empty()) {
        if (struct passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_name)
            u = pw->pw_name;
    }
    return u.empty() ? std::string("nobody") : u;
}

std::atomic<std::uint64_t> g_session_counter{0};

std::string compute_local_spill_root() {
    std::string best;
    std::uintmax_t best_avail = 0;
    for (const auto& m : list_mounts()) {
        if (m.kind != FilesystemKind::LOCAL) continue;
        if (!writable_dir(m.mountpoint)) continue;
        std::error_code ec;
        const auto space = fs::space(m.mountpoint, ec);
        if (ec || space.available <= best_avail) continue;
        best_avail = space.available;
        best = m.mountpoint;
    }
    // No mount root is writable by an ordinary user (the usual case: users
    // own directories under a mount, not the mount itself), so try the
    // per-user places that live on disk: /var/tmp, then ~/.cache.
    if (best.empty()) {
        std::vector<std::string> candidates{"/var/tmp"};
        const std::string home = env_str("HOME");
        if (!home.empty()) candidates.push_back(home + "/.cache");
        for (const auto& c : candidates) {
            if (filesystem_kind(c) == FilesystemKind::LOCAL &&
                writable_dir(c)) {
                best = c;
                break;
            }
        }
    }
    if (best.empty()) return {};
    const std::string dir =
        (fs::path(best) / current_user() / "dftracer-utils" / "spill")
            .lexically_normal()
            .string();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec)) return {};
    return dir;
}

}  // namespace

const std::string& scratch_root() noexcept {
    static const std::string root = compute_scratch_root(0);
    return root;
}

const std::string& local_spill_root() noexcept {
    static const std::string root = compute_local_spill_root();
    return root;
}

bool should_stage(const std::string& dest) noexcept {
    const std::string mode = lower(env_str("DFTRACER_INDEX_STAGE"));
    if (mode == "never" || mode == "off" || mode == "0") return false;
    // Cheap gate first: a local destination is the common case and never
    // stages, so skip the mount probing and mountpoint walks entirely.
    if (mode != "always" && !is_network_filesystem(filesystem_kind(dest)))
        return false;
    const std::string& root = scratch_root();
    if (root.empty()) return false;
    // No gain when scratch and destination share a mount.
    return find_mountpoint(root) != find_mountpoint(dest);
}

ScratchSession::ScratchSession(std::uint64_t estimated_bytes) {
    const std::string root = compute_scratch_root(estimated_bytes);
    if (root.empty()) return;
    const std::uint64_t id =
        g_session_counter.fetch_add(1, std::memory_order_relaxed);
    const std::string leaf =
        std::to_string(::getpid()) + "_" + std::to_string(id);
    const std::string dir =
        (fs::path(root) / current_user() / "dftracer-utils" / leaf)
            .lexically_normal()
            .string();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!ec && fs::is_directory(dir, ec)) dir_ = dir;
}

ScratchSession::~ScratchSession() {
    if (dir_.empty() || keep_) return;
    std::error_code ec;
    fs::remove_all(dir_, ec);
}

void publish_path(const std::string& src, const std::string& dst) {
    std::error_code ec;
    const fs::path parent = fs::path(dst).parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        ec.clear();
    }
    fs::remove_all(dst, ec);
    ec.clear();

    fs::rename(src, dst, ec);
    if (!ec) return;

    // Cross-device rename fails; fall back to copy + remove.
    ec.clear();
    fs::copy(src, dst,
             fs::copy_options::recursive | fs::copy_options::copy_symlinks |
                 fs::copy_options::overwrite_existing,
             ec);
    if (ec) {
        throw DFTUtilsException(
            ErrorCode::IO,
            "failed to publish " + src + " to " + dst + ": " + ec.message());
    }
    fs::remove_all(src, ec);
}

}  // namespace dftracer::utils
