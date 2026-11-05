// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_INPUT_STAT_H
#define DFTRACER_UTILS_COMMON_INPUT_STAT_H

#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <sys/stat.h>

#include <cstdint>
#include <ctime>
#include <shared_mutex>

namespace dftracer::utils::common {

/// Type, size and mtime of one path from a single stat that follows symlinks.
struct InputFileStat {
    std::uint64_t size = 0;
    /// Unix seconds.
    std::int64_t mtime = 0;
    /// Nanoseconds since the epoch, to tell two writes within one second.
    std::int64_t mtime_ns = 0;
    bool is_directory = false;
    bool is_regular_file = false;
};

/// The modification time of `st` in nanoseconds since the epoch.
inline std::int64_t stat_mtime_ns(const struct stat& st) {
#ifdef __APPLE__
    const struct timespec& ts = st.st_mtimespec;
#else
    const struct timespec& ts = st.st_mtim;
#endif
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000000 +
           static_cast<std::int64_t>(ts.tv_nsec);
}

inline InputFileStat input_file_stat_from(const struct stat& st) {
    InputFileStat out;
    out.size = static_cast<std::uint64_t>(st.st_size);
    out.mtime = static_cast<std::int64_t>(st.st_mtime);
    out.mtime_ns = stat_mtime_ns(st);
    out.is_directory = S_ISDIR(st.st_mode);
    out.is_regular_file = S_ISREG(st.st_mode);
    return out;
}

/// One stat of `path`. Returns false when the path cannot be stat'ed.
inline bool stat_path(const char* path, InputFileStat& out) {
    struct stat st{};
    if (::stat(path, &st) != 0) return false;
    out = input_file_stat_from(st);
    return true;
}

/// The stat results of the paths one run has read, so an input is stat'ed once
/// however many layers ask. Owned by one run (see @ref InputStatScope). Thread
/// safe.
class InputStatCache {
   public:
    bool find(dstd::StringView path, InputFileStat& out) const;
    void insert(dstd::StringView path, const InputFileStat& st);
    void erase(dstd::StringView path);

   private:
    mutable std::shared_mutex mutex_;
    // Made when the first path is kept, so a cache that kept nothing holds no
    // pool memory.
    dstd::UniquePtr<dstd::HashMap<dstd::String, InputFileStat>> map_;
};

/// Publishes a fresh @ref InputStatCache for its lifetime, so every
/// stat_input() uses it. Only a CLI tool makes one, on the stack of its main,
/// and the work must have joined when it ends. A scope made while another is
/// live shares that one. A running stat_input() keeps the cache alive until it
/// returns.
class InputStatScope {
   public:
    InputStatScope();
    ~InputStatScope();
    InputStatScope(const InputStatScope&) = delete;
    InputStatScope& operator=(const InputStatScope&) = delete;

   private:
    dstd::SharedPtr<InputStatCache> cache_;
};

/// stat_path() through the published cache. A plain stat when none is
/// published. A failed stat is not kept.
bool stat_input(dstd::CStringView path, InputFileStat& out);

/// The stat kept for `path` in the published cache, without a syscall. False
/// when there is none or no cache is published.
bool cached_input(dstd::StringView path, InputFileStat& out);

/// Keeps a stat the caller read, so a later stat_input() of `path` makes no
/// syscall. No-op without a published cache.
void remember_input(dstd::StringView path, const InputFileStat& st);

/// Drops `path` from the published cache. No-op without one. A caller that
/// deletes a path it may have stat'ed through stat_input() calls it.
void forget_input(dstd::StringView path);

/// Whether `path` exists, is a directory, or is a regular file, through the
/// published cache.
inline bool input_exists(dstd::CStringView path) {
    InputFileStat st;
    return stat_input(path, st);
}
inline bool input_is_directory(dstd::CStringView path) {
    InputFileStat st;
    return stat_input(path, st) && st.is_directory;
}
inline bool input_is_regular_file(dstd::CStringView path) {
    InputFileStat st;
    return stat_input(path, st) && st.is_regular_file;
}

}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_INPUT_STAT_H
