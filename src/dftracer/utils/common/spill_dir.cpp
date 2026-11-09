// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/filesystem_info.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/scratch.h>
#include <dftracer/utils/common/spill_dir.h>
#include <dftracer/utils/common/spill_dir_internal.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <mutex>

namespace dftracer::utils::common {

namespace detail {

dstd::Result<dstd::Path> choose_spill_dir(
    dstd::StringView env, const dstd::Path& local_root,
    const dstd::Vector<dstd::Path>& fallbacks) {
    if (!env.empty()) {
        dstd::StringView reason;
        const dstd::Path env_path{env};
        const auto made = dstd::fs::create_directories(env_path.view());
        if (!made.has_value())
            reason = made.error().message();
        else if (::access(env_path.c_str(), W_OK | X_OK) != 0)
            reason = "Permission denied";
        if (!reason.empty())
            return make_error(
                ErrorCode::Io,
                str_cat("spill: cannot use spill directory '", env, "' (",
                        reason, "); set ", constants::SPILL_DIR_ENV,
                        " to a writable directory"));
        return env_path;
    }
    // The root is found once, so check it again: a cleaner may have removed it
    // since.
    if (!local_root.empty()) {
        const auto made = dstd::fs::create_directories(local_root.view());
        if (made.has_value() && ::access(local_root.c_str(), W_OK | X_OK) == 0)
            return local_root;
    }
    // Spilled bytes on a RAM-backed file system stay in memory, which is what
    // spilling is meant to avoid, so such a directory is never chosen; a
    // network file system is slower but holds them outside memory.
    for (const auto& dir : fallbacks) {
        if (dir.empty()) continue;
        const dstd::PathView parent = dir.view().parent();
        const auto present = dstd::fs::exists(dir.view());
        const dstd::Path probe =
            (present.has_value() && *present) || parent.empty()
                ? dir
                : dstd::Path(parent.str());
        if (is_memory_filesystem(filesystem_kind(probe))) continue;
        if (!dstd::fs::create_directories(dir.view()).has_value() ||
            ::access(dir.c_str(), W_OK | X_OK) != 0)
            continue;
        return dir;
    }
    return make_error(
        ErrorCode::Io,
        str_cat("spill: no spill directory off RAM (no writable local disk, "
                "and the temp and cache directories are RAM-backed or not "
                "writable); set ",
                constants::SPILL_DIR_ENV,
                " to a directory on a disk or a parallel file system"));
}

}  // namespace detail

dstd::Result<dstd::Path> spill_dir() {
    const auto env = Env::get(constants::SPILL_DIR_ENV);
    dstd::Vector<dstd::Path> fallbacks;
    const dstd::Path tmp = dstd::fs::temp_directory();
    const auto tmp_is_dir = dstd::fs::is_directory(tmp.view());
    if (tmp_is_dir.has_value() && *tmp_is_dir) fallbacks.push_back(tmp);
    if (const auto home = Env::get("HOME"); home && !home->empty()) {
        dstd::Path spill{dstd::StringView(*home)};
        spill /= ".cache";
        spill /= "dftracer-utils";
        spill /= "spill";
        fallbacks.push_back(std::move(spill));
    }
    auto dir = detail::choose_spill_dir(env ? *env : dstd::StringView(),
                                        local_spill_root(), fallbacks);
    if (dir) {
        static const bool logged = [&] {
            DFTRACER_UTILS_LOG_INFO("spill directory: %s", dir->c_str());
            return true;
        }();
        (void)logged;
        spill_dir_in_memory(*dir);
    }
    return dir;
}

namespace {

struct DirKinds {
    std::mutex mu;
    dstd::Map<dstd::String, bool> kinds;
    bool warned = false;
};

}  // namespace

bool spill_dir_in_memory(dstd::CStringView dir) {
    DirKinds& known = dstd::Singleton<DirKinds>::get();
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, [] {
            DirKinds& held = dstd::Singleton<DirKinds>::get();
            std::lock_guard<std::mutex> lock(held.mu);
            held.kinds.clear();
        });
        return true;
    }();
    std::lock_guard<std::mutex> lock(known.mu);
    const auto found = known.kinds.find(dir.view());
    if (found != known.kinds.end()) return found->second;
    const bool in_memory = is_memory_filesystem(filesystem_kind(dir));
    known.kinds.emplace(dstd::String(dir.view()), in_memory);
    if (in_memory && !known.warned) {
        known.warned = true;
        DFTRACER_UTILS_LOG_WARN(
            "spill directory '%s' is RAM-backed (tmpfs): spilled bytes stay in "
            "memory and count toward the memory budget; set %s to a directory "
            "on a disk",
            dir.c_str(), constants::SPILL_DIR_ENV);
    }
    return in_memory;
}

namespace {

struct SpillSlot {
    memory::detail::ExternalSlot slot{memory::detail::attach_external("spill")};
};

memory::detail::ExternalSlot& spill_slot() {
    return dstd::Singleton<SpillSlot>::get().slot;
}

// The live subdirectories by path, so a writer that has only a file path finds
// the charge of the directory holding it.
struct SubdirRegistry {
    std::mutex mu;
    dstd::Map<dstd::String, dstd::WeakPtr<SpillCharge>> by_path;
};

SubdirRegistry& subdirs() { return dstd::Singleton<SubdirRegistry>::get(); }

}  // namespace

void SpillCharge::add(std::uint64_t bytes) noexcept {
    if (bytes == 0) return;
    bytes_.fetch_add(bytes, std::memory_order_relaxed);
    spill_slot().add(static_cast<std::ptrdiff_t>(bytes),
                     in_memory_ ? memory::detail::ExternalSlot::Kind::Counted
                                : memory::detail::ExternalSlot::Kind::Reported);
}

void SpillCharge::release(std::uint64_t bytes) noexcept {
    std::uint64_t cur = bytes_.load(std::memory_order_relaxed);
    std::uint64_t take = 0;
    do {
        take = std::min(cur, bytes);
    } while (!bytes_.compare_exchange_weak(cur, cur - take,
                                           std::memory_order_relaxed));
    if (take == 0) return;
    spill_slot().add(-static_cast<std::ptrdiff_t>(take),
                     in_memory_ ? memory::detail::ExternalSlot::Kind::Counted
                                : memory::detail::ExternalSlot::Kind::Reported);
}

SpillCharge::~SpillCharge() {
    const std::uint64_t bytes = bytes_.load(std::memory_order_relaxed);
    if (bytes == 0) return;
    spill_slot().add(-static_cast<std::ptrdiff_t>(bytes),
                     in_memory_ ? memory::detail::ExternalSlot::Kind::Counted
                                : memory::detail::ExternalSlot::Kind::Reported);
}

namespace {

struct SweptPrefixes {
    std::mutex mu;
    dstd::Set<dstd::String> prefixes;
};

// A spill root on a shared file system holds the directories of other hosts,
// whose pids mean nothing here.
dstd::String host_tag() {
    char host[256] = {};
    ::gethostname(host, sizeof(host) - 1);
    dstd::String tag(host);
    for (char& c : tag)
        if (c == '/' || c == '_') c = '-';
    return tag;
}

// Removes the `<prefix>_<host>_<pid>_<n>` directories of this host whose pid is
// no longer a process.
void sweep_stale(dstd::PathView root, dstd::StringView prefix) {
    const auto dir = dstd::fs::Dir::open(root);
    if (!dir) return;
    auto listing = dir->entries();
    if (!listing) return;
    const dstd::String tag = host_tag() + "_";
    for (const dstd::fs::DirEntry& entry : *listing) {
        const dstd::StringView name = entry.name;
        if (name.size() <= prefix.size() + 1 ||
            name.compare(0, prefix.size(), prefix) != 0 ||
            name[prefix.size()] != '_')
            continue;
        if (name.compare(prefix.size() + 1, tag.size(), tag) != 0) continue;
        const char* first = name.data() + prefix.size() + 1 + tag.size();
        const char* last = name.data() + name.size();
        long pid = 0;
        const auto [stop, perr] = dstd::from_chars(first, last, pid);
        if (perr != std::errc{} || stop == last || *stop != '_' || pid <= 0)
            continue;
        if (::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH) continue;
        dstd::fs::remove_all((root / name).view()).discard();
    }
}

}  // namespace

dstd::Result<ScopedSpillSubdir> ScopedSpillSubdir::create(
    dstd::StringView prefix) {
    auto root = spill_dir();
    if (!root) return dstd::Unexpected(std::move(root).error());
    SweptPrefixes& swept = dstd::Singleton<SweptPrefixes>::get();
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, [] {
            SweptPrefixes& held = dstd::Singleton<SweptPrefixes>::get();
            std::lock_guard<std::mutex> lock(held.mu);
            held.prefixes.clear();
        });
        return true;
    }();
    {
        std::lock_guard<std::mutex> lock(swept.mu);
        if (swept.prefixes.insert(dstd::String(prefix)).second)
            sweep_stale(root->view(), prefix);
    }
    static std::atomic<std::uint64_t> counter{0};
    const dstd::Path dir =
        root->view() / str_cat(prefix, "_", host_tag(), "_", ::getpid(), "_",
                               counter.fetch_add(1));
    const auto made = dstd::fs::create_directories(dir.view());
    if (!made.has_value())
        return make_error(
            ErrorCode::Io,
            str_cat("spill: cannot create '", dir.str(), "' (",
                    made.error().message(), "); set ", constants::SPILL_DIR_ENV,
                    " to a writable directory"));
    auto charge = dstd::make_shared<SpillCharge>(spill_dir_in_memory(*root));
    SubdirRegistry& reg = subdirs();
    {
        std::lock_guard<std::mutex> lock(reg.mu);
        reg.by_path[dstd::String(dir.str())] = charge;
    }
    return ScopedSpillSubdir(dir, std::move(charge));
}

dstd::SharedPtr<SpillCharge> ScopedSpillSubdir::charge_for(
    dstd::PathView file) {
    const dstd::PathView parent = file.parent();
    SubdirRegistry& reg = subdirs();
    std::lock_guard<std::mutex> lock(reg.mu);
    const auto found = reg.by_path.find(parent.str());
    return found == reg.by_path.end() ? nullptr : found->second.lock();
}

ScopedSpillSubdir::ScopedSpillSubdir(ScopedSpillSubdir&& other) noexcept
    : path_(std::exchange(other.path_, dstd::Path())),
      charge_(std::move(other.charge_)) {}

ScopedSpillSubdir& ScopedSpillSubdir::operator=(
    ScopedSpillSubdir&& other) noexcept {
    if (this != &other) {
        remove();
        path_ = std::exchange(other.path_, dstd::Path());
        charge_ = std::move(other.charge_);
    }
    return *this;
}

ScopedSpillSubdir::~ScopedSpillSubdir() { remove(); }

// A writer that still holds the charge keeps it until it lets go, so the bytes
// leave the gauge once the files are gone and no writer adds more.
void ScopedSpillSubdir::remove() noexcept {
    if (path_.empty()) return;
    dstd::fs::remove_all(path_.view()).discard();
    SubdirRegistry& reg = subdirs();
    {
        std::lock_guard<std::mutex> lock(reg.mu);
        const auto found = reg.by_path.find(path_.str());
        if (found != reg.by_path.end()) reg.by_path.erase(found);
    }
    path_ = dstd::Path();
    charge_.reset();
}

}  // namespace dftracer::utils::common
