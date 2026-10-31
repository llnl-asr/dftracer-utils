#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/schemas/dft/merged_statistics.h>

namespace dftracer::utils::index::schemas::dft {

void add_path_counts(const index::store::IndexDatabase& db, int file_id,
                     std::string_view path,
                     dstd::StringViewMap<std::uint64_t>& into) {
    for (const auto& [granule, bytes] : db.path_granules(
             file_id, index::store::IndexExtension::Counts, path)) {
        auto counts = index::extensions::kinds::decode_counts(bytes);
        if (!counts || !counts->compressed) continue;
        const auto values =
            index::extensions::ChunkDimensionStats::decompress_value_counts(
                counts->compressed->data(), counts->compressed->size());
        for (const auto& [k, v] : values) into[k] += v;
    }
}

ankerl::unordered_dense::map<int, MergedStatisticsResult> merged_statistics(
    const index::store::IndexDatabase& db, const std::vector<int>& file_ids) {
    ankerl::unordered_dense::map<int, MergedStatisticsResult> results;
    for (const int fid : file_ids) {
        auto rows = db.query_chunk_statistics(fid);
        if (rows.empty()) continue;
        auto& merged = results[fid];
        for (auto& row : rows) {
            if (merged.num_chunks == 0)
                merged.stats = std::move(row.stats);
            else
                merged.stats.merge_from(row.stats);
            ++merged.num_chunks;
        }
        if (!db.extension_current(fid, index::store::IndexExtension::Counts))
            continue;
        add_path_counts(db, fid, "cat", merged.stats.category_counts);
        add_path_counts(db, fid, "name", merged.stats.name_counts);
        add_path_counts(db, fid, "pid_tid", merged.stats.pid_tid_counts);
    }
    return results;
}

}  // namespace dftracer::utils::index::schemas::dft
