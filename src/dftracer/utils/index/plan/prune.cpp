#include <ankerl/unordered_dense.h>
#include <dftracer/utils/index/extensions/kinds/time_bounds.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/plan/condition.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace dftracer::utils::index::plan {

namespace {

// Keeps a chunk unless its recorded time bounds lie wholly outside the window,
// [begin, end) when `half_open`; chunks without bounds, or with corrupt ones,
// are kept.
std::vector<std::uint64_t> filter_by_time(
    const std::vector<std::uint64_t>& chunks,
    const ankerl::unordered_dense::map<
        std::uint64_t, std::pair<std::uint64_t, std::uint64_t>>& bounds,
    double begin, double end, bool half_open) {
    std::vector<std::uint64_t> kept;
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

// Runs `fn(db, file_id)` on the request's index; false when the file is not
// in it.
template <class Fn>
bool with_file(const PruneRequest& request, Fn&& fn) {
    std::optional<store::IndexDatabase> owned;
    auto* db = request.db;
    if (!db) {
        owned.emplace(request.index_path, store::IndexOpenMode::ReadOnly);
        db = &*owned;
    }
    const int fid = db->get_file_info_id(
        store::internal::get_logical_path(request.file_path));
    if (fid < 0) return false;
    fn(*db, fid);
    return true;
}

coro::CoroTask<Result<PruneResult>> prune_data(const PruneRequest& request) {
    PruneResult result;

    if (request.query && !request.index_path.empty()) {
        ChunkPruner pruner;
        // A temporary built inside the co_await expression is destroyed
        // twice by GCC 12, so the input is a named local.
        const ChunkPrunerInput input{request.index_path, request.file_path,
                                     *request.query, request.db};
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
    ankerl::unordered_dense::map<std::uint64_t,
                                 std::pair<std::uint64_t, std::uint64_t>>
        bounds;
    ankerl::unordered_dense::set<std::uint64_t> no_starts;
    try {
        if (!with_file(request, [&](const store::IndexDatabase& db, int fid) {
                bounds =
                    request.by_start
                        ? index::extensions::kinds::chunk_start_bounds(db, fid)
                        : index::extensions::kinds::chunk_time_bounds(db, fid);
                // Whole bins round the window outward, so a zero is exact.
                if (request.by_start)
                    no_starts = index::extensions::kinds::chunks_without_starts(
                        db, fid,
                        static_cast<std::uint64_t>(std::max(0.0, begin)),
                        end > 0 ? static_cast<std::uint64_t>(std::ceil(end))
                                : std::numeric_limits<std::uint64_t>::max());
            }))
            co_return result;
    } catch (const std::exception& e) {
        co_return make_error(ErrorCode::INDEXER,
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

coro::CoroTask<Result<PruneResult>> prune_file(PruneRequest request) {
    PruneResult result;
    if (request.metadata != MetadataUse::RECORDS) {
        auto data = co_await prune_data(request);
        if (!data || request.metadata == MetadataUse::NONE || data->all_chunks)
            co_return data;
        result = std::move(*data);
    }

    // Data evidence says nothing about metadata records: add the chunks
    // whose records are asked for, or every chunk without evidence.
    std::optional<
        ankerl::unordered_dense::map<std::uint64_t, store::ChunkMetadata>>
        meta;
    if (!request.index_path.empty()) {
        try {
            with_file(request, [&](const store::IndexDatabase& db, int fid) {
                meta = db.chunk_metadata(fid);
            });
        } catch (const std::exception& e) {
            co_return make_error(ErrorCode::INDEXER,
                                 "prune_file: index read failed for " +
                                     request.file_path + ": " + e.what());
        }
    }
    if (!meta) co_return PruneResult{};

    if (request.metadata == MetadataUse::EVERY) {
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
