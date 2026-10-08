#ifndef DFTRACER_UTILS_CORE_COMMON_SCRATCH_H
#define DFTRACER_UTILS_CORE_COMMON_SCRATCH_H

#include <cstdint>
#include <string>

namespace dftracer::utils {

/// The best node-local scratch root discovered from the OS mount table (RAM
/// tmpfs preferred over local disk, larger free space breaks ties), or "" when
/// none is usable or staging is disabled. $DFTRACER_INDEX_SCRATCH overrides the
/// choice; set it to "off"/"none"/"0" to disable staging entirely. Cached; this
/// no-argument form ignores capacity, so use it only to test availability.
const std::string& scratch_root() noexcept;

/// `<mount>/<user>/dftracer-utils/spill` on the node-local disk mount with the
/// most free space that the user can write, created if missing. When no mount
/// root is writable, the same under /var/tmp, then ~/.cache, if they are on a
/// local disk. "" when there is none. RAM-backed (tmpfs) and network mounts
/// never qualify: spill files there would count against the memory limit or be
/// slow. Cached. Independent of $DFTRACER_INDEX_SCRATCH.
const std::string& local_spill_root() noexcept;

/// Whether an artifact whose final home is `dest` should be built on local
/// scratch and copied back. True when a scratch root exists and either the
/// destination is a network/parallel filesystem (the default "smart" policy) or
/// $DFTRACER_INDEX_STAGE=always. Set $DFTRACER_INDEX_STAGE=never to force off.
bool should_stage(const std::string& dest) noexcept;

/// A per-invocation scratch working directory:
///   `<scratch_root>/<user>/dftracer-utils/<unique-id>/`
/// meant to hold all build artifacts for one run (e.g. `.dftindex` plus any
/// per-worker subdirs) so cleanup is a single directory removal. The directory
/// is removed on destruction unless release() is called.
class ScratchSession {
   public:
    /// Creates the session directory on the best scratch mount that has room
    /// for `estimated_bytes` (plus headroom); tmpfs is additionally bounded by
    /// free RAM. `valid()` is false when no mount qualifies or the directory
    /// could not be created; callers then build in place.
    explicit ScratchSession(std::uint64_t estimated_bytes = 0);
    ~ScratchSession();
    ScratchSession(const ScratchSession&) = delete;
    ScratchSession& operator=(const ScratchSession&) = delete;

    bool valid() const noexcept { return !dir_.empty(); }
    const std::string& dir() const noexcept { return dir_; }

    /// Keep the directory on destruction instead of removing it.
    void release() noexcept { keep_ = true; }

   private:
    std::string dir_;
    bool keep_ = false;
};

/// Move `src` (a directory or file built on scratch) to `dst`, replacing any
/// existing `dst`. Renames when both live on one filesystem, else recursively
/// copies then removes `src`. Throws DFTUtilsException on failure.
void publish_path(const std::string& src, const std::string& dst);

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_SCRATCH_H
