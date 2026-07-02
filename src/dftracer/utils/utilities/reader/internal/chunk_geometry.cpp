#include <dftracer/utils/utilities/reader/internal/chunk_geometry.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_pruner_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::reader::internal {

namespace {

// User-supplied byte/line clip applied to every emitted work item.
struct ClipRange {
    std::size_t start_byte = 0, end_byte = 0;
    std::size_t start_line = 0, end_line = 0;
    bool has_line_clip() const { return start_line > 0 || end_line > 0; }
    bool has_byte_clip() const { return end_byte > start_byte; }
};

// Geometry of the pruner chunks for one file, derived from its gzip recovery
// points. Pruner chunk_idx is 0-indexed over uncompressed slices: recovery
// point ckpts[k] sits at the START of chunk (k+1); chunk 0 has no recovery
// point at its start (decoded from gzip stream start). Total chunks =
// ckpts.size()+1.
struct ChunkGeometry {
    const std::vector<dftracer::utils::utilities::indexer::IndexerCheckpoint>
        &ckpts;

    std::size_t total_chunks() const { return ckpts.size() + 1; }
    std::size_t start_byte(std::uint64_t cidx) const {
        if (cidx == 0) return 0;
        return ckpts[cidx - 1].uc_offset;
    }
    std::size_t end_byte(std::uint64_t cidx) const {
        if (cidx == 0) return ckpts.empty() ? 0 : ckpts[0].uc_offset;
        std::size_t k = cidx - 1;
        return ckpts[k].uc_offset + ckpts[k].uc_size;
    }
    // Chunk 0 covers everything before the first recovery point; chunk k>=1
    // spans recovery point (k-1).
    std::size_t first_line(std::uint64_t cidx) const {
        if (cidx == 0) return 1;
        return ckpts[cidx - 1].first_line_num;
    }
    std::size_t last_line(std::uint64_t cidx) const {
        if (cidx == 0) {
            if (ckpts.empty()) return SIZE_MAX;
            return ckpts[0].first_line_num > 0 ? ckpts[0].first_line_num - 1
                                               : 0;
        }
        return ckpts[cidx - 1].last_line_num;
    }
};

// Extract AND-of-EQ leaves from a Query AST. Returns nullopt if the predicate
// shape is anything else (NE, range ops, IN, NOT, OR), in which case the
// uniform-match shortcut does not apply.
std::optional<std::vector<std::pair<std::string, std::string>>>
extract_eq_leaves(
    const dftracer::utils::utilities::common::query::QueryNode &node) {
    namespace q_ns = dftracer::utils::utilities::common::query;
    using LeafVec = std::vector<std::pair<std::string, std::string>>;

    auto literal_to_string = [](const q_ns::LiteralNode &lit) -> std::string {
        return std::visit(
            [](auto &&v) -> std::string {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>)
                    return v;
                else if constexpr (std::is_same_v<T, bool>)
                    return v ? "true" : "false";
                else if constexpr (std::is_same_v<T, std::int64_t>)
                    return std::to_string(v);
                else if constexpr (std::is_same_v<T, std::uint64_t>)
                    return std::to_string(v);
                else if constexpr (std::is_same_v<T, double>)
                    return std::to_string(v);
                else
                    return {};
            },
            lit.value);
    };

    return std::visit(
        [&](const auto &n) -> std::optional<LeafVec> {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, q_ns::CompareNode>) {
                if (n.op != q_ns::CompareOp::EQ) return std::nullopt;
                return LeafVec{{n.field.path, literal_to_string(n.value)}};
            } else if constexpr (std::is_same_v<T, q_ns::AndNode>) {
                auto l = extract_eq_leaves(*n.left);
                if (!l) return std::nullopt;
                auto r = extract_eq_leaves(*n.right);
                if (!r) return std::nullopt;
                l->insert(l->end(), r->begin(), r->end());
                return l;
            } else {
                return std::nullopt;
            }
        },
        node.data);
}

// True iff every checkpoint in `chunk_idxs` has dim_stats min == max == literal
// for every leaf. Empty leaves -> false (no shortcut). Missing dim_stats for
// any (chunk, leaf) -> false (we don't know, play safe).
bool all_chunks_uniform_match(
    const dftracer::utils::utilities::indexer::IndexDatabase &db, int fid,
    const std::vector<std::pair<std::string, std::string>> &leaves,
    const std::vector<std::uint64_t> &chunk_idxs) {
    if (leaves.empty() || chunk_idxs.empty()) return false;
    namespace indexing = dftracer::utils::utilities::composites::dft::indexing;

    for (const auto &[dim, val] : leaves) {
        auto rows = db.query_chunk_dimension_stats_for_dimension(fid, dim);
        if (rows.empty()) return false;
        std::unordered_map<std::uint64_t,
                           const indexing::ChunkDimensionStatsResult *>
            by_ckpt;
        by_ckpt.reserve(rows.size());
        for (const auto &r : rows) by_ckpt.emplace(r.checkpoint_idx, &r);
        for (auto cidx : chunk_idxs) {
            auto it = by_ckpt.find(cidx);
            if (it == by_ckpt.end()) return false;
            const auto &ds = *it->second;
            if (ds.min_value != val || ds.max_value != val) return false;
        }
    }
    return true;
}

// Select the chunk indices to scan for one file. Returns empty when the file
// is fully pruned or no chunk overlaps the line clip (caller skips the file).
std::vector<std::uint64_t> select_kept_chunks(
    const ChunkGeometry &geo, bool has_query,
    const dftracer::utils::utilities::composites::dft::indexing::
        ChunkPrunerOutput &pr,
    const ClipRange &clip) {
    const std::size_t total_chunks = geo.total_chunks();
    std::vector<std::uint64_t> keep_chunks;
    keep_chunks.reserve(total_chunks);
    if (has_query) {
        if (pr.success && !pr.file_may_match) {
            return keep_chunks;  // whole file pruned
        }
        if (pr.success && !pr.candidate_checkpoints.empty() &&
            pr.candidate_checkpoints.size() < pr.total_checkpoints) {
            for (auto cidx : pr.candidate_checkpoints) {
                if (cidx < total_chunks) keep_chunks.push_back(cidx);
            }
            std::sort(keep_chunks.begin(), keep_chunks.end());
            keep_chunks.erase(
                std::unique(keep_chunks.begin(), keep_chunks.end()),
                keep_chunks.end());
        } else {
            for (std::uint64_t c = 0; c < total_chunks; ++c)
                keep_chunks.push_back(c);
        }
    } else {
        for (std::uint64_t c = 0; c < total_chunks; ++c)
            keep_chunks.push_back(c);
    }

    // Intersect with the user's line range so workers only touch chunks that
    // actually overlap it. Each work item carries the sub-line-range;
    // LINE_RANGE on the read maps it back to bytes via the same checkpoint
    // table the gzip stream uses.
    if (clip.has_line_clip()) {
        std::size_t lo = clip.start_line > 0 ? clip.start_line : 1;
        std::size_t hi = clip.end_line > 0 ? clip.end_line : SIZE_MAX;
        std::vector<std::uint64_t> filtered;
        filtered.reserve(keep_chunks.size());
        for (auto c : keep_chunks) {
            std::size_t cf = geo.first_line(c);
            std::size_t cl = geo.last_line(c);
            if (cl < lo || cf > hi) continue;
            filtered.push_back(c);
        }
        keep_chunks = std::move(filtered);
    }
    return keep_chunks;
}

// Group the kept chunks into contiguous work ranges (one per worker slot) and
// append the resulting work items.
void emit_work_items(std::vector<ArrowWorkItem> &items, const std::string &fp,
                     const ChunkGeometry &geo,
                     const std::vector<std::uint64_t> &keep_chunks,
                     bool file_pure_match, std::size_t max_workers,
                     const ClipRange &clip) {
    std::size_t target_ranges = std::max<std::size_t>(1, max_workers);
    std::size_t per_range = std::max<std::size_t>(
        1, (keep_chunks.size() + target_ranges - 1) / target_ranges);

    std::size_t group_start = 0;
    while (group_start < keep_chunks.size()) {
        std::size_t group_end = group_start;
        std::size_t emitted = 0;
        while (group_end < keep_chunks.size() && emitted < per_range) {
            if (group_end > group_start &&
                keep_chunks[group_end] != keep_chunks[group_end - 1] + 1) {
                break;
            }
            ++group_end;
            ++emitted;
        }
        std::uint64_t scidx = keep_chunks[group_start];
        std::uint64_t ecidx = keep_chunks[group_end - 1];
        std::size_t start_byte = geo.start_byte(scidx);
        std::size_t end_byte = geo.end_byte(ecidx);
        // start_at_checkpoint: a gzip recovery point sits at start_byte (true
        // for any cidx>=1; false for the implicit chunk 0 which decodes from
        // stream start).
        bool start_at_checkpoint = (scidx >= 1);
        bool end_at_checkpoint = (group_end < keep_chunks.size());
        if (clip.has_line_clip()) {
            std::size_t lo = clip.start_line > 0 ? clip.start_line : 1;
            std::size_t hi = clip.end_line > 0 ? clip.end_line : SIZE_MAX;
            std::size_t cluster_first = geo.first_line(scidx);
            std::size_t cluster_last = geo.last_line(ecidx);
            std::size_t item_start = std::max<std::size_t>(lo, cluster_first);
            std::size_t item_end = std::min<std::size_t>(hi, cluster_last);
            if (item_start > item_end) {
                group_start = group_end;
                continue;
            }
            ArrowWorkItem item;
            item.file_path = fp;
            item.chunk_prune_only = file_pure_match;
            item.start_line = item_start;
            item.end_line = item_end;
            items.push_back(std::move(item));
            group_start = group_end;
            continue;
        }
        if (clip.has_byte_clip()) {
            if (start_byte < clip.start_byte) {
                start_byte = clip.start_byte;
                start_at_checkpoint = false;
            }
            if (end_byte > clip.end_byte) {
                end_byte = clip.end_byte;
                end_at_checkpoint = false;
            }
            if (start_byte >= end_byte) {
                group_start = group_end;
                continue;
            }
        }
        items.push_back({fp, start_byte, end_byte, start_at_checkpoint,
                         end_at_checkpoint, file_pure_match});
        group_start = group_end;
    }
}

}  // namespace

std::vector<ArrowWorkItem> enumerate_work_items(
    const std::vector<std::string> &files, const std::string &index_dir,
    const std::string &query_str, std::size_t max_workers,
    std::size_t clip_start_byte, std::size_t clip_end_byte,
    std::size_t clip_start_line, std::size_t clip_end_line) {
    namespace dft_internal =
        dftracer::utils::utilities::composites::dft::internal;
    namespace indexer_ns = dftracer::utils::utilities::indexer;
    namespace indexing = dftracer::utils::utilities::composites::dft::indexing;

    const ClipRange clip{clip_start_byte, clip_end_byte, clip_start_line,
                         clip_end_line};

    std::vector<ArrowWorkItem> items;
    items.reserve(files.size() * 4);

    auto push_unsplit = [&](const std::string &fp) {
        ArrowWorkItem item;
        item.file_path = fp;
        item.start_line = clip.start_line;
        item.end_line = clip.end_line;
        items.push_back(std::move(item));
    };

    // Parse the query once. Pruner input copies a Query, so we keep the
    // parsed form around to feed each ChunkPrunerInput without re-parsing.
    std::optional<dftracer::utils::utilities::common::query::Query> parsed;
    if (!query_str.empty()) {
        auto r = dftracer::utils::utilities::common::query::Query::from_string(
            query_str);
        if (r) parsed = std::move(*r);
    }

    // All files in a directory-mode scan share the same `.dftindex` root.
    // Group files by their resolved index path so we can open the RocksDB
    // once per index and reuse it to prune every file against that handle.
    std::unordered_map<std::string, std::vector<std::size_t>> by_index;
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::string index_path =
            dft_internal::determine_index_path(files[i], index_dir);
        by_index[index_path].push_back(i);
    }

    for (auto &entry : by_index) {
        const auto &index_path = entry.first;
        const auto &file_idxs = entry.second;
        if (!fs::exists(index_path)) {
            for (auto i : file_idxs) push_unsplit(files[i]);
            continue;
        }
        std::unique_ptr<indexer_ns::IndexDatabase> idx_db;
        try {
            idx_db = std::make_unique<indexer_ns::IndexDatabase>(
                index_path,
                dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        } catch (...) {
            for (auto i : file_idxs) push_unsplit(files[i]);
            continue;
        }

        // Resolve fid + checkpoints per file (cheap queries).
        struct FileCtx {
            std::size_t file_idx;
            int fid;
            std::vector<indexer_ns::IndexerCheckpoint> ckpts;
        };
        std::vector<FileCtx> file_ctxs;
        file_ctxs.reserve(file_idxs.size());
        for (auto i : file_idxs) {
            FileCtx fc;
            fc.file_idx = i;
            fc.fid = idx_db->get_file_info_id(
                indexer_ns::internal::get_logical_path(files[i]));
            if (fc.fid < 0) {
                push_unsplit(files[i]);
                continue;
            }
            fc.ckpts = idx_db->query_checkpoints(fc.fid);
            if (fc.ckpts.empty()) {
                push_unsplit(files[i]);
                continue;
            }
            std::sort(fc.ckpts.begin(), fc.ckpts.end(),
                      [](const auto &a, const auto &b) {
                          return a.first_line_num < b.first_line_num;
                      });
            file_ctxs.push_back(std::move(fc));
        }

        // Batch-prune all files against the shared index: dim_stats and
        // chunk_statistics are loaded in one RocksDB scan each instead of
        // one scan per file.
        std::vector<indexing::ChunkPrunerOutput> pruner_outs(file_ctxs.size());
        if (parsed && !file_ctxs.empty()) {
            indexing::ChunkPrunerBatchInput batch_in;
            batch_in.index_path = index_path;
            batch_in.external_db = idx_db.get();
            batch_in.items.reserve(file_ctxs.size());
            for (auto &fc : file_ctxs) {
                batch_in.items.push_back({files[fc.file_idx], *parsed});
            }
            indexing::ChunkPrunerUtility pruner;
            auto batch_out = pruner.process_batch(batch_in);
            if (batch_out) {
                pruner_outs = std::move(batch_out->outputs);
            }
        }

        // For AND-of-EQ predicates, precompute uniform-match leaves once.
        // Per-file pure_match is checked inline below and lets workers skip
        // per-event predicate eval on chunks where dim_stats min == max ==
        // literal for every leaf.
        std::optional<std::vector<std::pair<std::string, std::string>>>
            eq_leaves;
        if (parsed) eq_leaves = extract_eq_leaves(parsed->root());

        for (std::size_t fc_idx = 0; fc_idx < file_ctxs.size(); ++fc_idx) {
            auto &fc = file_ctxs[fc_idx];
            const auto &fp = files[fc.file_idx];

            ChunkGeometry geo{fc.ckpts};
            std::vector<std::uint64_t> keep_chunks = select_kept_chunks(
                geo, parsed.has_value(), pruner_outs[fc_idx], clip);
            if (keep_chunks.empty()) continue;

            // All-or-nothing per file: if every kept chunk is uniform-matching
            // for every leaf, every work item from this file gets the
            // chunk_prune_only fast path. Mixed files fall back to per-event
            // eval to stay safe.
            bool file_pure_match = false;
            if (eq_leaves && !eq_leaves->empty() && idx_db) {
                file_pure_match = all_chunks_uniform_match(
                    *idx_db, fc.fid, *eq_leaves, keep_chunks);
            }

            emit_work_items(items, fp, geo, keep_chunks, file_pure_match,
                            max_workers, clip);
        }
    }
    return items;
}

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_ENABLE_ARROW
