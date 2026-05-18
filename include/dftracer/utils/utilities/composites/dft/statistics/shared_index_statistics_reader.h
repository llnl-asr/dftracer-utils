#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_SHARED_INDEX_STATISTICS_READER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_SHARED_INDEX_STATISTICS_READER_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/composites/dft/indexing/index_resolver_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_query_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/trace_statistics.h>
#include <dftracer/utils/utilities/indexer/index_database.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::statistics {

struct EntrySnapshot {
    std::size_t file_index;
    int file_id;
    std::string file_path;
};

struct SharedIndexBatchRows {
    std::unordered_map<int, std::uint64_t> num_chunks;
    std::unordered_map<int, utilities::indexer::MergedStatisticsResult>
        fallback_merged_stats;
    std::unordered_map<int, utilities::indexer::ChunkStatistics> merged_stats;
    std::vector<EntrySnapshot> entries_snapshot;
};

inline SharedIndexBatchRows query_shared_index_batch(
    std::string index_path, std::vector<indexing::ResolvedFile> entries,
    StatisticsQueryType query_type) {
    SharedIndexBatchRows rows;
    std::vector<int> file_ids;
    file_ids.reserve(entries.size());
    rows.entries_snapshot.reserve(entries.size());
    for (auto& entry : entries) {
        file_ids.push_back(entry.file_id);
        rows.entries_snapshot.push_back(EntrySnapshot{
            entry.file_index, entry.file_id, std::move(entry.file_path)});
    }

    utilities::indexer::IndexDatabase idx_db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
    auto scalar_rows = idx_db.query_file_scalar_stats_batch(file_ids);
    std::unordered_map<int, utilities::indexer::ChunkStatistics*> merge_targets;
    merge_targets.reserve(file_ids.size());

    std::vector<int> missing_ids;
    missing_ids.reserve(file_ids.size());
    rows.num_chunks.reserve(scalar_rows.size());
    rows.merged_stats.reserve(scalar_rows.size());
    for (auto& [file_id, merged] : scalar_rows) {
        rows.num_chunks.emplace(file_id, merged.num_chunks);
        auto merged_entry =
            rows.merged_stats.emplace(file_id, std::move(merged.stats));
        merge_targets.emplace(file_id, &merged_entry.first->second);
    }
    for (const auto file_id : file_ids) {
        if (rows.num_chunks.find(file_id) == rows.num_chunks.end()) {
            missing_ids.push_back(file_id);
        }
    }
    const bool needs_categories =
        query_type == StatisticsQueryType::SUMMARY ||
        query_type == StatisticsQueryType::CATEGORIES ||
        query_type == StatisticsQueryType::TOP_N_CATEGORIES;
    const bool needs_names = query_type == StatisticsQueryType::NAMES ||
                             query_type == StatisticsQueryType::TOP_N_NAMES;
    const bool needs_pid_tids = query_type == StatisticsQueryType::SUMMARY ||
                                query_type == StatisticsQueryType::PID_TIDS;

    if (needs_categories) {
        idx_db.merge_file_category_counts_batch_into(file_ids, merge_targets);
    }
    if (needs_names) {
        idx_db.merge_file_name_counts_batch_into(file_ids, merge_targets);
    }
    if (needs_pid_tids) {
        idx_db.merge_file_pid_tid_counts_batch_into(file_ids, merge_targets);
    }
    if (!missing_ids.empty()) {
        rows.fallback_merged_stats =
            idx_db.query_merged_statistics_batch(missing_ids);
    }
    return rows;
}

class SharedIndexStatisticsReader {
   public:
    SharedIndexStatisticsReader() = default;

    coro::CoroTask<SharedIndexBatchRows> query(
        std::string index_path, std::vector<indexing::ResolvedFile> entries,
        StatisticsQueryType query_type) const {
        co_return query_shared_index_batch(std::move(index_path),
                                           std::move(entries), query_type);
    }

    template <typename Callback>
    static void process_batch_results(SharedIndexBatchRows& batch_rows,
                                      Callback& callback) {
        for (const auto& [file_index, file_id, file_path] :
             batch_rows.entries_snapshot) {
            const auto chunks_it = batch_rows.num_chunks.find(file_id);
            if (chunks_it != batch_rows.num_chunks.end()) {
                TraceStatistics stats;
                stats.file_path = file_path;
                stats.num_chunks = chunks_it->second;
                stats.merged = std::move(batch_rows.merged_stats[file_id]);
                stats.success = stats.num_chunks > 0;
                if (!stats.success) {
                    stats.error_message =
                        "No chunk statistics in index for " + file_path;
                }
                callback(file_index, std::move(stats));
                continue;
            }

            auto merged_it = batch_rows.fallback_merged_stats.find(file_id);
            callback(file_index,
                     build_trace_statistics_from_index(
                         file_path,
                         merged_it == batch_rows.fallback_merged_stats.end()
                             ? nullptr
                             : &merged_it->second));
        }
    }

   private:
    static TraceStatistics build_trace_statistics_from_index(
        const std::string& file_path,
        utilities::indexer::MergedStatisticsResult* merged) {
        TraceStatistics result;
        result.file_path = file_path;

        if (merged == nullptr || merged->num_chunks == 0) {
            result.success = false;
            result.error_message =
                "No chunk statistics in index for " + file_path;
            return result;
        }

        result.num_chunks = merged->num_chunks;
        result.merged = std::move(merged->stats);
        result.success = true;
        return result;
    }
};

}  // namespace dftracer::utils::utilities::composites::dft::statistics

#endif
