#ifndef DFTRACER_UTILS_TRACE_INTERNAL_INDEX_ROOT_H
#define DFTRACER_UTILS_TRACE_INTERNAL_INDEX_ROOT_H

#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/runtime/coro/async_semaphore.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace dftracer::utils::trace::internal {

// A file this process built or found current in an index root, by modification
// time and size.
struct IndexedFile {
    std::pair<std::int64_t, std::uint64_t> stamp;
    std::size_t num_lines = 0;
    std::uint64_t max_bytes = 0;
    std::uint64_t checkpoint_size = 0;
    std::size_t num_checkpoints = 0;
};

// What this process knows of the index at one root. `gate` is a FIFO
// reader-writer gate: a freshness check holds one permit, while a build or a
// first-touch bootstrap writes the root and holds them all. A read-only open
// that meets a writer fails and reads as a stale index, and the store refuses
// to upgrade a read-only handle that a reader still holds, so the two never
// overlap.
struct IndexRoot {
    static constexpr std::uint64_t ALL_PERMITS = std::uint64_t{1} << 32;
    coro::CoroSemaphore gate{ALL_PERMITS};
    std::mutex current_mutex;
    StringViewMap<IndexedFile> current;
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
std::shared_ptr<IndexRoot> index_root(const std::string& index_path);

}  // namespace dftracer::utils::trace::internal

#endif  // DFTRACER_UTILS_TRACE_INTERNAL_INDEX_ROOT_H
