// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/parallel.h>
#include <dftracer/utils/dstd/fatal.h>

#include <algorithm>

namespace dftracer::utils::coro::detail {

ChunkPlan ChunkPlan::make(std::int64_t size, const ParallelForOptions& options,
                          const Executor* executor) {
    if (options.grain < 1) {
        dstd::fatal("coro: a parallel range needs a grain of at least 1");
    }
    ChunkPlan plan;
    plan.partition = options.partition;
    plan.grain = options.grain;
    if (size <= 0) return plan;
    plan.size = size;
    const std::int64_t grains =
        size / options.grain + (size % options.grain != 0 ? 1 : 0);
    std::size_t limit = options.max_in_flight;
    if (limit == 0) limit = executor != nullptr ? executor->concurrency() : 1;
    limit = std::max<std::size_t>(limit, 1);
    const auto grain_chunks = static_cast<std::size_t>(grains);
    plan.workers = std::min(limit, grain_chunks);
    plan.chunks =
        options.partition == Partition::Static ? plan.workers : grain_chunks;
    return plan;
}

std::int64_t ChunkPlan::begin(std::size_t chunk) const noexcept {
    const auto at = static_cast<std::int64_t>(chunk);
    if (partition == Partition::Dynamic) return at * grain;
    const auto count = static_cast<std::int64_t>(chunks);
    return size / count * at + std::min(size % count, at);
}

std::int64_t ChunkPlan::end(std::size_t chunk) const noexcept {
    if (chunk + 1 == chunks) return size;
    return begin(chunk + 1);
}

}  // namespace dftracer::utils::coro::detail
