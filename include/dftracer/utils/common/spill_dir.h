// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_SPILL_DIR_H
#define DFTRACER_UTILS_COMMON_SPILL_DIR_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/path.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace dftracer::utils::common {

/// The directory every spill file goes to: DFTRACER_UTILS_SPILL_DIR when set;
/// else the node-local disk mount with the most free space that the user can
/// write (local_spill_root(), never RAM-backed or network); else the system
/// temp directory, then ~/.cache/dftracer-utils/spill, whichever is first
/// writable and not RAM-backed (a network file system qualifies). Created if
/// missing. A set variable that cannot be used is an IO error naming the
/// directory and the variable, with no fallback to another directory; when no
/// directory off RAM exists, an IO error names the variable.
Result<dstd::Path> spill_dir();

/// True when `dir` is on a RAM-backed file system (tmpfs). Decided once per
/// directory. The first such directory logs one warning per process naming it
/// and DFTRACER_UTILS_SPILL_DIR. Thread safe.
bool spill_dir_in_memory(dstd::CStringView dir);

/// Live bytes of spill files, added to the process-wide @ref
/// memory::ExternalSlot named "spill" and taken back on destruction. They count
/// toward the memory budget only when `in_memory` (see spill_dir_in_memory()).
/// add() is thread safe and takes no lock.
class SpillCharge {
   public:
    explicit SpillCharge(bool in_memory) noexcept : in_memory_(in_memory) {}
    ~SpillCharge();
    SpillCharge(const SpillCharge&) = delete;
    SpillCharge& operator=(const SpillCharge&) = delete;

    void add(std::uint64_t bytes) noexcept;
    /// Take back `bytes`, or what is charged when that is less.
    void release(std::uint64_t bytes) noexcept;
    std::uint64_t bytes() const noexcept {
        return bytes_.load(std::memory_order_relaxed);
    }

   private:
    std::atomic<std::uint64_t> bytes_{0};
    bool in_memory_;
};

/// A directory under spill_dir() that one owner fills with its spill files,
/// named `<prefix>_<host>_<pid>_<n>` and removed with everything in it on
/// destruction. The first one a process makes for a prefix also removes the
/// directories of that prefix left by processes that are gone (a crash or a
/// kill never runs a destructor), so they do not pile up. The bytes charged to
/// it are released when it is removed, not when one of its files is.
class ScopedSpillSubdir {
   public:
    static Result<ScopedSpillSubdir> create(dstd::StringView prefix);

    ScopedSpillSubdir(ScopedSpillSubdir&& other) noexcept;
    ScopedSpillSubdir& operator=(ScopedSpillSubdir&& other) noexcept;
    ScopedSpillSubdir(const ScopedSpillSubdir&) = delete;
    ScopedSpillSubdir& operator=(const ScopedSpillSubdir&) = delete;
    ~ScopedSpillSubdir();

    const dstd::Path& path() const noexcept { return path_; }

    SpillCharge& charge() const noexcept { return *charge_; }

    /// The charge of the live @ref ScopedSpillSubdir that holds `file`, for a
    /// writer that knows only the path. Null when no such directory is open.
    /// Takes a lock, so call it once per file, not per write.
    static std::shared_ptr<SpillCharge> charge_for(dstd::PathView file);

   private:
    ScopedSpillSubdir(dstd::Path path, std::shared_ptr<SpillCharge> charge)
        : path_(std::move(path)), charge_(std::move(charge)) {}
    void remove() noexcept;

    dstd::Path path_;
    std::shared_ptr<SpillCharge> charge_;
};

}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_SPILL_DIR_H
