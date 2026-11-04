// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INTERNAL_INDEX_ROOT_H
#define DFTRACER_UTILS_TRACE_INTERNAL_INDEX_ROOT_H

#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace dftracer::utils::trace::internal {

// A file this process built or found current in an index root, by modification
// time and size.
struct IndexedFile {
    dstd::Pair<std::int64_t, std::uint64_t> stamp;
    std::size_t line_count = 0;
    std::uint64_t max_bytes = 0;
    std::uint64_t checkpoint_size = 0;
    std::size_t checkpoint_count = 0;
};

// `gate` is a FIFO reader-writer gate. A freshness check holds one permit, and
// a build or a first-touch bootstrap holds them all. A read-only open that
// meets a writer fails and reads as a stale index.
struct IndexRoot {
    static constexpr std::uint64_t ALL_PERMITS = std::uint64_t{1} << 32;
    coro::CoroSemaphore gate{ALL_PERMITS};
    std::mutex current_mutex;
    dstd::StringViewMap<IndexedFile> current;
};

// Permits of a root's gate, returned when it goes out of scope.
class RootPermit {
   public:
    RootPermit(IndexRoot& root, std::uint64_t permits) noexcept
        : root_(&root), permits_(permits) {}
    RootPermit(RootPermit&& other) noexcept
        : root_(other.root_), permits_(other.permits_) {
        other.root_ = nullptr;
    }
    RootPermit(const RootPermit&) = delete;
    RootPermit& operator=(const RootPermit&) = delete;
    RootPermit& operator=(RootPermit&&) = delete;
    ~RootPermit() {
        if (root_) root_->gate.release(permits_);
    }

   private:
    IndexRoot* root_;
    std::uint64_t permits_;
};

// The shared state of the root at `index_path`, keyed by canonical path.
dstd::SharedPtr<IndexRoot> index_root(const dstd::String& index_path);

}  // namespace dftracer::utils::trace::internal

#endif  // DFTRACER_UTILS_TRACE_INTERNAL_INDEX_ROOT_H
