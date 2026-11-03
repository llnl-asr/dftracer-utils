// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/extensions/kinds/time_bounds.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/plan/condition.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/tables.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace dftracer::utils::index::plan {

namespace {

// Keeps a chunk unless its recorded time bounds lie wholly outside the window,
// [begin, end) when `half_open`; chunks without bounds, or with corrupt ones,
// are kept.
dstd::Vector<std::uint64_t> filter_by_time(
    const dstd::Vector<std::uint64_t>& chunks,
    const dstd::HashMap<std::uint64_t,
                        dstd::Pair<std::uint64_t, std::uint64_t>>& bounds,
    double begin, double end, bool half_open) {
    dstd::Vector<std::uint64_t> kept;
    kept.reserve(chunks.size());
    for (auto ckpt : chunks) {
        auto it = bounds.find(ckpt);
        if (it != bounds.end()) {
            const auto lo = static_cast<double>(it->second.first);
            const auto hi = static_cast<double>(it->second.second);
            const bool after = end > 0 && (half_open ? lo >= end : lo > end);
            if (lo <= hi && (hi < begin || after)) continue;
        }
        kept.push_back(ckpt);
    }
    return kept;
}

// The request's snapshot, or one taken on first use and kept for the call.
class Reads {
   public:
    explicit Reads(const PruneRequest& request)
        : request_(request), snapshot_(request.snapshot) {}

    dstd::Result<const index::Snapshot*> get() {
        if (!snapshot_) {
            auto taken = open_snapshot(request_.index_path);
            if (!taken) return dstd::Unexpected(std::move(taken).error());
            owned_.emplace(std::move(*taken));
            snapshot_ = &*owned_;
        }
        return snapshot_;
    }

   private:
    const PruneRequest& request_;
    const index::Snapshot* snapshot_;
    dstd::Optional<index::Snapshot> owned_;
};

coro::CoroTask<dstd::Result<PruneResult>> prune_data(
    const PruneRequest& request, Reads& reads) {
    PruneResult result;

    if (request.query && !request.index_path.empty()) {
        ChunkPruner pruner;
        // A temporary built inside the co_await expression is destroyed
        // twice by GCC 12, so the input is a named local.
        const ChunkPrunerInput input{request.index_path, request.file_path,
                                     *request.query,
                                     common::unwrap(reads.get())};
        auto out = co_await pruner(input);
        if (out.success) {
            result.total_chunks = out.total_checkpoints;
            if (!out.file_may_match && out.candidate_checkpoints.empty()) {
                result.file_may_match = false;
                result.all_chunks = false;
                co_return result;
            }
            if (!out.candidate_checkpoints.empty()) {
                result.all_chunks = false;
                result.candidates = std::move(out.candidate_checkpoints);
            }
        }
    }

    if (!request.time_range) co_return result;
    const auto [begin, end] = *request.time_range;
    if (begin <= 0 && end <= 0) co_return result;

    if (request.index_path.empty()) co_return result;
    dstd::HashMap<std::uint64_t, dstd::Pair<std::uint64_t, std::uint64_t>>
        bounds;
    dstd::HashSet<std::uint64_t> no_starts;
    try {
        const index::Snapshot& snapshot = *common::unwrap(reads.get());
        const auto file =
            snapshot.file(store::internal::get_logical_path(request.file_path));
        if (!file) co_return result;
        bounds =
            request.by_start
                ? index::extensions::kinds::chunk_start_bounds(snapshot, *file)
                : index::extensions::kinds::chunk_time_bounds(snapshot, *file);
        // Whole bins round the window outward, so a zero is exact.
        if (request.by_start)
            no_starts = index::extensions::kinds::chunks_without_starts(
                snapshot, *file,
                static_cast<std::uint64_t>(std::max(0.0, begin)),
                end > 0 ? static_cast<std::uint64_t>(std::ceil(end))
                        : std::numeric_limits<std::uint64_t>::max());
    } catch (const std::exception& e) {
        co_return common::make_error(common::ErrorCode::Indexer,
                                     "prune_file: index read failed for " +
                                         request.file_path + ": " + e.what());
    }

    if (result.all_chunks) {
        const auto count =
            result.total_chunks > 0 ? result.total_chunks : request.chunk_count;
        for (std::uint64_t i = 0; i < count; ++i)
            result.candidates.push_back(i);
        result.all_chunks = false;
    }
    result.candidates =
        filter_by_time(result.candidates, bounds, begin, end, request.by_start);
    if (!no_starts.empty())
        std::erase_if(result.candidates,
                      [&](std::uint64_t c) { return no_starts.contains(c); });
    co_return result;
}

}  // namespace

dstd::Result<index::Snapshot> open_snapshot(dstd::StringView index_path) {
    auto opened =
        Index::open(store::internal::normalize_index_root(index_path));
    if (!opened) return dstd::Unexpected(std::move(opened).error());
    return opened->snapshot();
}

coro::CoroTask<dstd::Result<PruneResult>> prune_file(PruneRequest request) {
    PruneResult result;
    Reads reads(request);
    if (request.metadata != MetadataUse::Records) {
        auto data = co_await prune_data(request, reads);
        if (!data || request.metadata == MetadataUse::None || data->all_chunks)
            co_return data;
        result = std::move(*data);
    }

    // Data evidence says nothing about metadata records: add the chunks
    // whose records are asked for, or every chunk without evidence.
    dstd::Optional<dstd::HashMap<std::uint64_t, store::ChunkMetadata>> meta;
    if (!request.index_path.empty()) {
        try {
            const index::Snapshot& snapshot = *common::unwrap(reads.get());
            if (const auto file = snapshot.file(
                    store::internal::get_logical_path(request.file_path)))
                meta = common::unwrap(index::chunk_meta(snapshot, *file));
        } catch (const std::exception& e) {
            co_return common::make_error(common::ErrorCode::Indexer,
                                         "prune_file: index read failed for " +
                                             request.file_path + ": " +
                                             e.what());
        }
    }
    if (!meta) co_return PruneResult{};

    if (request.metadata == MetadataUse::Every) {
        for (const auto& [chunk, m] : *meta)
            if (m.records > 0) result.candidates.push_back(chunk);
    } else {
        ChunkSet with_records;
        for (const auto& [chunk, m] : *meta)
            if (m.records > 0) with_records.insert(chunk);
        const ChunkSet matched =
            request.query
                ? evaluate(request.query->root(),
                           make_metadata_conditions(*meta), with_records)
                : with_records;
        result.candidates.insert(result.candidates.end(), matched.begin(),
                                 matched.end());
    }
    std::sort(result.candidates.begin(), result.candidates.end());
    result.candidates.erase(
        std::unique(result.candidates.begin(), result.candidates.end()),
        result.candidates.end());
    result.all_chunks = false;
    result.file_may_match = !result.candidates.empty();
    if (result.total_chunks == 0) result.total_chunks = meta->size();
    co_return result;
}

}  // namespace dftracer::utils::index::plan
