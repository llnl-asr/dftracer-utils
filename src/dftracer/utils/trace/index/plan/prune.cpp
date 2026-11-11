// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/trace/index/extensions/kinds/time_bounds.h>
#include <dftracer/utils/trace/index/plan/chunk_pruner.h>
#include <dftracer/utils/trace/index/plan/condition.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/index/tables.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace dftracer::utils::trace::index::plan {

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

    dstd::Result<const trace::index::Snapshot*> get() {
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
    const trace::index::Snapshot* snapshot_;
    dstd::Optional<trace::index::Snapshot> owned_;
};

dstd::Unexpected<dstd::Error> read_failed(const dstd::String& file_path,
                                          const dstd::Error& error) {
    return common::make_error<"prune_file: index read failed for {}: {}">(
        common::ErrorCode::Indexer, file_path, error.message());
}

coro::CoroTask<dstd::Result<PruneResult>> prune_data(
    const PruneRequest& request, Reads& reads) {
    PruneResult result;

    if (request.query && !request.index_path.empty()) {
        ChunkPruner pruner;
        // A temporary built inside the co_await expression is destroyed
        // twice by GCC 12, so the input is a named local.
        const auto snapshot = reads.get();
        if (!snapshot) co_return snapshot.error();
        const ChunkPrunerInput input{request.index_path, request.file_path,
                                     *request.query, *snapshot};
        auto pruning = pruner(input);
        auto pruned = co_await std::move(pruning);
        if (!pruned) co_return pruned.error();
        auto& out = *pruned;
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
    {
        const auto taken = reads.get();
        if (!taken) co_return read_failed(request.file_path, taken.error());
        const trace::index::Snapshot& snapshot = **taken;
        const auto file =
            snapshot.file(fileio::get_logical_path(request.file_path));
        if (!file) co_return result;
        auto read_bounds =
            request.by_start
                ? trace::index::extensions::kinds::chunk_start_bounds(snapshot,
                                                                      *file)
                : trace::index::extensions::kinds::chunk_time_bounds(snapshot,
                                                                     *file);
        if (!read_bounds)
            co_return read_failed(request.file_path, read_bounds.error());
        bounds = std::move(*read_bounds);
        // Whole bins round the window outward, so a zero is exact.
        if (request.by_start) {
            auto read_starts =
                trace::index::extensions::kinds::chunks_without_starts(
                    snapshot, *file,
                    static_cast<std::uint64_t>(std::max(0.0, begin)),
                    end > 0 ? static_cast<std::uint64_t>(std::ceil(end))
                            : std::numeric_limits<std::uint64_t>::max());
            if (!read_starts)
                co_return read_failed(request.file_path, read_starts.error());
            no_starts = std::move(*read_starts);
        }
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

dstd::Result<trace::index::Snapshot> open_snapshot(
    dstd::StringView index_path) {
    auto opened = Index::open(normalize_index_root(index_path));
    if (!opened) return dstd::Unexpected(std::move(opened).error());
    return opened->snapshot();
}

namespace {

coro::CoroTask<dstd::Result<PruneResult>> prune_chunks(
    const PruneRequest& request, Reads& reads) {
    PruneResult result;
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
        const auto taken = reads.get();
        if (!taken) co_return read_failed(request.file_path, taken.error());
        const trace::index::Snapshot& snapshot = **taken;
        if (const auto file =
                snapshot.file(fileio::get_logical_path(request.file_path))) {
            auto read_meta = trace::index::chunk_meta(snapshot, *file);
            if (!read_meta)
                co_return read_failed(request.file_path, read_meta.error());
            meta = std::move(*read_meta);
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
                ? evaluate(duql::detail::QueryAccess::root(*request.query),
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

// The key of a line that spans chunks is in the chunk where it ends, and a byte
// range reader returns it from the chunk where it starts, so that one is read
// too. A line number reader keeps its own rows, since each row has one owner.
void add_line_starts(dstd::Vector<std::uint64_t>& candidates,
                     const dstd::Vector<gzip::ChunkSpan>& spans) {
    const std::size_t count = candidates.size();
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t chunk = candidates[i];
        while (chunk > 0 && chunk < spans.size() &&
               spans[chunk].opens_mid_line) {
            --chunk;
            candidates.push_back(chunk);
        }
    }
    if (candidates.size() == count) return;
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
}

}  // namespace

coro::CoroTask<dstd::Result<PruneResult>> prune_file(PruneRequest request) {
    Reads reads(request);
    auto pruned = co_await prune_chunks(request, reads);
    // Only a later chunk can open on a line that starts in an earlier one.
    if (!pruned || pruned->all_chunks || pruned->candidates.empty() ||
        request.index_path.empty() || pruned->candidates.back() == 0)
        co_return pruned;
    const auto taken = reads.get();
    if (!taken) co_return read_failed(request.file_path, taken.error());
    const trace::index::Snapshot& snapshot = **taken;
    const auto file =
        snapshot.file(fileio::get_logical_path(request.file_path));
    if (file) {
        const auto meta = trace::index::file_metadata(snapshot, *file);
        if (!meta) co_return read_failed(request.file_path, meta.error());
        if (*meta && (*meta)->has_mid_line_chunk) {
            const auto spans = trace::index::spans(snapshot, *file);
            if (!spans) co_return read_failed(request.file_path, spans.error());
            add_line_starts(pruned->candidates, *spans);
        }
    }
    co_return pruned;
}

}  // namespace dftracer::utils::trace::index::plan
