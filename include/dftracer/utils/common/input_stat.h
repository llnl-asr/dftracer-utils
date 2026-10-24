#ifndef DFTRACER_UTILS_COMMON_INPUT_STAT_H
#define DFTRACER_UTILS_COMMON_INPUT_STAT_H

#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>

#include <memory>
#include <shared_mutex>
#include <string>

namespace dftracer::utils {

/// The stat results of the paths one run has read, so an input is stat'ed once
/// however many layers ask. Owned by one run (see InputStatScope); thread safe.
class InputStatCache {
   public:
    bool find(const std::string& path, InputFileStat& out) const;
    void insert(const std::string& path, const InputFileStat& st);
    void erase(const std::string& path);

   private:
    mutable std::shared_mutex mutex_;
    // Made when the first path is kept, so a cache that kept nothing holds no
    // pool memory.
    dstd::UniquePtr<dstd::HashMap<dstd::String, InputFileStat>> map_;
};

/// Publishes a fresh InputStatCache for the lifetime of the object, so every
/// stat_input() on any thread reads and fills it. Only a CLI tool makes one, on
/// the stack of its main, after its arguments parse and before its work
/// starts; the work has joined when it ends. Each run then has a cache of its
/// own, and library calls, which never make one, stat the files every time. A
/// scope made while another is live shares that one, and the other stays
/// published when this one ends. A stat_input() that is running when the owner
/// ends keeps the cache alive until it returns.
class InputStatScope {
   public:
    InputStatScope();
    ~InputStatScope();
    InputStatScope(const InputStatScope&) = delete;
    InputStatScope& operator=(const InputStatScope&) = delete;

   private:
    dstd::SharedPtr<InputStatCache> cache_;
};

/// stat_path() through the published cache; a plain stat when none is
/// published. A failed stat is not kept.
bool stat_input(const std::string& path, InputFileStat& out);

inline bool stat_input(const fs::path& path, InputFileStat& out) {
    return stat_input(path.string(), out);
}

/// Drops `path` from the published cache; no-op without one. A caller that
/// deletes a path it may have stat'ed through stat_input() calls it.
void forget_input(const std::string& path);

/// Whether `path` exists, is a directory, or is a regular file, through the
/// published cache.
inline bool input_exists(const std::string& path) {
    InputFileStat st;
    return stat_input(path, st);
}
inline bool input_is_directory(const std::string& path) {
    InputFileStat st;
    return stat_input(path, st) && st.is_directory;
}
inline bool input_is_regular_file(const std::string& path) {
    InputFileStat st;
    return stat_input(path, st) && st.is_regular_file;
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_COMMON_INPUT_STAT_H
