// SPDX-License-Identifier: MIT
#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/schemas/dft/merged_statistics.h>

#include <utility>

namespace dftracer::utils::index::schemas::dft {

common::Result<void> add_path_counts(const Snapshot& snapshot,
                                     const FileRef& file, dstd::StringView path,
                                     dstd::StringViewMap<std::uint64_t>& into) {
    auto found =
        granules(snapshot, file, index::store::IndexExtension::Counts, path);
    if (!found) return dstd::Unexpected<dstd::Error>(found.error());
    for (const auto& [granule, bytes] : *found) {
        auto counts = index::extensions::kinds::decode_counts(bytes);
        if (!counts || !counts->compressed) continue;
        const auto values =
            index::extensions::ChunkDimensionStats::decompress_value_counts(
                counts->compressed->data(), counts->compressed->size());
        for (const auto& [k, v] : values) into[k] += v;
    }
    return {};
}

common::Result<dstd::Optional<MergedStatisticsResult>> merged_statistics(
    const Snapshot& snapshot, const FileRef& file) {
    auto rows = chunk_stats(snapshot, file);
    if (!rows) return dstd::Unexpected<dstd::Error>(rows.error());
    if (rows->empty()) return dstd::Optional<MergedStatisticsResult>{};
    MergedStatisticsResult merged;
    for (auto& row : *rows) {
        if (merged.num_chunks == 0)
            merged.stats = std::move(row.stats);
        else
            merged.stats.merge_from(row.stats);
        ++merged.num_chunks;
    }
    if (extension_current(snapshot, file,
                          index::store::IndexExtension::Counts)) {
        auto cats = add_path_counts(snapshot, file, "cat",
                                    merged.stats.category_counts);
        if (!cats) return dstd::Unexpected<dstd::Error>(cats.error());
        auto names =
            add_path_counts(snapshot, file, "name", merged.stats.name_counts);
        if (!names) return dstd::Unexpected<dstd::Error>(names.error());
        auto pid_tids = add_path_counts(snapshot, file, "pid_tid",
                                        merged.stats.pid_tid_counts);
        if (!pid_tids) return dstd::Unexpected<dstd::Error>(pid_tids.error());
    }
    return dstd::Optional<MergedStatisticsResult>(std::move(merged));
}

}  // namespace dftracer::utils::index::schemas::dft
