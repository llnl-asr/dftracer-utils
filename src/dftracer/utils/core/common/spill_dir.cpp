#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/filesystem_info.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/scratch.h>
#include <dftracer/utils/core/common/spill_dir.h>
#include <dftracer/utils/core/common/spill_dir_internal.h>
#include <dftracer/utils/core/env.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace dftracer::utils {

namespace detail {

Result<std::string> choose_spill_dir(
    const std::string& env, const std::string& local_root,
    const std::vector<std::string>& fallbacks) {
    std::error_code ec;
    if (!env.empty()) {
        fs::create_directories(env, ec);
        if (!ec && ::access(env.c_str(), W_OK | X_OK) != 0)
            ec = std::make_error_code(std::errc::permission_denied);
        if (ec)
            return make_error(
                ErrorCode::IO,
                str_cat("spill: cannot use spill directory '", env, "' (",
                        ec.message(), "); set ", constants::SPILL_DIR_ENV,
                        " to a writable directory"));
        return env;
    }
    // The root is found once, so check it again: a cleaner may have removed it
    // since.
    if (!local_root.empty()) {
        fs::create_directories(local_root, ec);
        if (!ec && ::access(local_root.c_str(), W_OK | X_OK) == 0)
            return local_root;
    }
    // Spilled bytes on a RAM-backed file system stay in memory, which is what
    // spilling is meant to avoid, so such a directory is never chosen; a
    // network file system is slower but holds them outside memory.
    for (const auto& dir : fallbacks) {
        if (dir.empty()) continue;
        const fs::path parent = fs::path(dir).parent_path();
        const std::string probe =
            fs::exists(dir, ec) ? dir
                                : (parent.empty() ? dir : parent.string());
        if (is_memory_filesystem(filesystem_kind(probe))) continue;
        ec.clear();
        fs::create_directories(dir, ec);
        if (ec || ::access(dir.c_str(), W_OK | X_OK) != 0) continue;
        return dir;
    }
    return make_error(
        ErrorCode::IO,
        str_cat("spill: no spill directory off RAM (no writable local disk, "
                "and the temp and cache directories are RAM-backed or not "
                "writable); set ",
                constants::SPILL_DIR_ENV,
                " to a directory on a disk or a parallel file system"));
}

}  // namespace detail

Result<std::string> spill_dir() {
    const auto env = Env::get(constants::SPILL_DIR_ENV);
    std::vector<std::string> fallbacks;
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec);
    if (!ec) fallbacks.push_back(tmp.string());
    if (const auto home = Env::get("HOME"); home && !home->empty())
        fallbacks.push_back((fs::path(std::string(*home)) / ".cache" /
                             "dftracer-utils" / "spill")
                                .string());
    auto dir = detail::choose_spill_dir(env ? std::string(*env) : std::string(),
                                        local_spill_root(), fallbacks);
    if (dir) {
        static const bool logged = [&] {
            DFTRACER_UTILS_LOG_INFO("spill directory: %s", dir->c_str());
            return true;
        }();
        (void)logged;
    }
    return dir;
}

namespace {

// A spill root on a shared file system holds the directories of other hosts,
// whose pids mean nothing here.
std::string host_tag() {
    char host[256] = {};
    ::gethostname(host, sizeof(host) - 1);
    std::string tag(host);
    for (char& c : tag)
        if (c == '/' || c == '_') c = '-';
    return tag;
}

// Removes the `<prefix>_<host>_<pid>_<n>` directories of this host whose pid is
// no longer a process.
void sweep_stale(const fs::path& root, std::string_view prefix) {
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() <= prefix.size() + 1 ||
            name.compare(0, prefix.size(), prefix) != 0 ||
            name[prefix.size()] != '_')
            continue;
        const std::string tag = host_tag() + "_";
        if (name.compare(prefix.size() + 1, tag.size(), tag) != 0) continue;
        const char* first = name.data() + prefix.size() + 1 + tag.size();
        const char* last = name.data() + name.size();
        long pid = 0;
        const auto [stop, perr] = std::from_chars(first, last, pid);
        if (perr != std::errc{} || stop == last || *stop != '_' || pid <= 0)
            continue;
        if (::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH) continue;
        std::error_code rm;
        fs::remove_all(it->path(), rm);
    }
}

}  // namespace

Result<ScopedSpillSubdir> ScopedSpillSubdir::create(std::string_view prefix) {
    auto root = spill_dir();
    if (!root) return unexpected<DFTUtilsError>(root.error());
    static std::mutex mu;
    static std::set<std::string> swept;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (swept.insert(std::string(prefix)).second)
            sweep_stale(*root, prefix);
    }
    static std::atomic<std::uint64_t> counter{0};
    const fs::path dir =
        fs::path(*root) / str_cat(prefix, "_", host_tag(), "_", ::getpid(), "_",
                                  counter.fetch_add(1));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        return make_error(
            ErrorCode::IO,
            str_cat("spill: cannot create '", dir.string(), "' (", ec.message(),
                    "); set ", constants::SPILL_DIR_ENV,
                    " to a writable directory"));
    return ScopedSpillSubdir(dir.string());
}

ScopedSpillSubdir::ScopedSpillSubdir(ScopedSpillSubdir&& other) noexcept
    : path_(std::exchange(other.path_, std::string())) {}

ScopedSpillSubdir& ScopedSpillSubdir::operator=(
    ScopedSpillSubdir&& other) noexcept {
    if (this != &other) {
        std::error_code ec;
        if (!path_.empty()) fs::remove_all(path_, ec);
        path_ = std::exchange(other.path_, std::string());
    }
    return *this;
}

ScopedSpillSubdir::~ScopedSpillSubdir() {
    if (path_.empty()) return;
    std::error_code ec;
    fs::remove_all(path_, ec);
}

}  // namespace dftracer::utils
