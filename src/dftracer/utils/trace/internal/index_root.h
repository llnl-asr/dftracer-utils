#ifndef DFTRACER_UTILS_TRACE_INTERNAL_INDEX_ROOT_H
#define DFTRACER_UTILS_TRACE_INTERNAL_INDEX_ROOT_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/coro/async_mutex.h>

#include <cstddef>
#include <cstdint>
#include <memory>
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

// What this process knows of the index at one root. `building` serializes
// every first build of a file in the root; `current` is held under it.
struct IndexRoot {
    coro::AsyncMutex building;
    StringViewMap<IndexedFile> current;
};

// The shared state of the root at `index_path`, keyed by canonical path.
std::shared_ptr<IndexRoot> index_root(const std::string& index_path);

}  // namespace dftracer::utils::trace::internal

#endif  // DFTRACER_UTILS_TRACE_INTERNAL_INDEX_ROOT_H
