#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <simdjson.h>

namespace dftracer::utils::utilities::composites::dft::statistics {

using dftracer::utils::utilities::common::json::JsonValue;
using dftracer::utils::utilities::indexer::ChunkStatisticsResult;
using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::internal::get_logical_path;
using fileio::lines::sources::async_streaming_gz_lines;

coro::CoroTask<TraceStatistics> StatisticsAggregatorUtility::process(
    const StatisticsAggregatorInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("aggregate statistics");
    TraceStatistics result;
    result.file_path = input.file_path;

    if (!input.index_path.empty()) {
        result.index_path =
            indexer::internal::normalize_index_root(input.index_path);
    } else {
        result.index_path =
            internal::determine_index_path(input.file_path, input.index_dir);
    }

    if (!fs::exists(result.index_path)) {
        result.success = false;
        result.error_message = "Index store not found: " + result.index_path;
        co_return result;
    }

    bool needs_streaming_fallback = false;
    try {
        IndexDatabase idx_db(result.index_path);

        int fid = idx_db.get_file_info_id(get_logical_path(input.file_path));
        if (fid < 0) {
            result.success = false;
            result.error_message =
                "File not found in index: " + input.file_path;
            co_return result;
        }

        std::vector<ChunkStatisticsResult> chunks;
        try {
            chunks = idx_db.query_chunk_statistics(fid);
        } catch (const std::exception&) {
            needs_streaming_fallback = true;
        }

        if (!needs_streaming_fallback && chunks.empty()) {
            needs_streaming_fallback = true;
        }

        if (!needs_streaming_fallback) {
            result.num_chunks = chunks.size();
            result.merged = chunks[0].stats;
            for (std::size_t i = 1; i < chunks.size(); ++i) {
                result.merged.merge_from(chunks[i].stats);
            }

            auto dim_stats = idx_db.query_chunk_dimension_stats(fid);
            for (const auto& ds : dim_stats) {
                if (!ds.has_value_counts_payload()) continue;
                ds.ensure_value_counts_decoded();
                if (!ds.value_counts) continue;
                if (ds.dimension == "cat") {
                    for (const auto& [k, v] : *ds.value_counts)
                        result.merged.category_counts[k] += v;
                } else if (ds.dimension == "name") {
                    for (const auto& [k, v] : *ds.value_counts)
                        result.merged.name_counts[k] += v;
                } else if (ds.dimension == "pid_tid") {
                    for (const auto& [k, v] : *ds.value_counts)
                        result.merged.pid_tid_counts[k] += v;
                }
            }

            result.success = true;
            co_return result;
        }
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
        co_return result;
    }

    if (!needs_streaming_fallback) {
        co_return result;
    }

    if (!fs::exists(input.file_path)) {
        result.success = false;
        result.error_message = "Trace file not found: " + input.file_path;
        co_return result;
    }

    /// Sequential fallback: stream the file line-by-line and compute
    /// statistics on-the-fly when the index has no chunk_statistics.
    try {
        indexing::ChunkStatistics stats;
        simdjson::dom::parser parser;
        auto gen = async_streaming_gz_lines(input.file_path);
        while (auto line_opt = co_await gen.next()) {
            const auto& line = *line_opt;
            if (line.content.empty()) continue;

            auto parse_result =
                parser.parse(line.content.data(), line.content.size());
            if (parse_result.error()) continue;

            auto root = parse_result.value_unsafe();
            if (!root.is_object()) continue;

            try {
                JsonValue json(root);
                std::string_view ph = json["ph"].get<std::string_view>();

                if (ph != "M") {
                    std::string_view name =
                        json["name"].get<std::string_view>();
                    std::string_view cat = json["cat"].get<std::string_view>();
                    std::uint64_t pid = json["pid"].get<std::uint64_t>();
                    std::uint64_t tid = json["tid"].get<std::uint64_t>();
                    std::uint64_t ts = json["ts"].get<std::uint64_t>();
                    std::uint64_t dur = json["dur"].get<std::uint64_t>();
                    stats.update_from_event(name, cat, pid, tid, ts, dur);
                }
            } catch (const std::exception&) {
            }
        }

        result.merged = std::move(stats);
        result.num_chunks = 0;
        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
    }

    co_return result;
}

coro::CoroTask<std::vector<TraceStatistics>>
StatisticsAggregatorUtility::process_batch(
    const StatisticsAggregatorBatchInput& input) {
    if (input.file_paths.empty()) {
        co_return std::vector<TraceStatistics>{};
    }

    const auto& index_path = input.index_path;
    if (!fs::exists(index_path)) {
        std::vector<TraceStatistics> results;
        results.reserve(input.file_paths.size());
        for (const auto& fp : input.file_paths) {
            TraceStatistics r;
            r.file_path = fp;
            r.index_path = index_path;
            r.success = false;
            r.error_message = "Index store not found: " + index_path;
            results.push_back(std::move(r));
        }
        co_return results;
    }

    const auto& files = input.file_paths;
    std::vector<TraceStatistics> results;
    results.resize(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        results[i].file_path = files[i];
        results[i].index_path = index_path;
    }

    try {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);

        std::vector<int> file_ids(files.size(), -1);
        for (std::size_t i = 0; i < files.size(); ++i) {
            file_ids[i] = db.get_file_info_id(get_logical_path(files[i]));
            if (file_ids[i] < 0) {
                results[i].success = false;
                results[i].error_message =
                    "File not found in index: " + files[i];
            }
        }

        std::vector<int> valid_ids;
        valid_ids.reserve(files.size());
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (file_ids[i] >= 0) valid_ids.push_back(file_ids[i]);
        }

        auto scalar_batch = db.query_file_scalar_stats_batch(valid_ids);
        auto cat_batch = db.query_file_category_counts_batch(valid_ids);
        auto pid_tid_batch = db.query_file_pid_tid_counts_batch(valid_ids);
        auto name_batch = db.query_file_name_summaries_batch(valid_ids);

        for (std::size_t i = 0; i < files.size(); ++i) {
            if (file_ids[i] < 0) continue;
            const int fid = file_ids[i];

            auto scalar_it = scalar_batch.find(fid);
            if (scalar_it == scalar_batch.end()) {
                results[i].success = false;
                results[i].error_message =
                    "No file summary in index for: " + files[i];
                continue;
            }

            results[i].merged = scalar_it->second.stats;
            results[i].num_chunks = scalar_it->second.num_chunks;
            results[i].success = true;

            auto cat_it = cat_batch.find(fid);
            if (cat_it != cat_batch.end()) {
                results[i].merged.category_counts = std::move(cat_it->second);
            }

            auto pid_it = pid_tid_batch.find(fid);
            if (pid_it != pid_tid_batch.end()) {
                results[i].merged.pid_tid_counts = std::move(pid_it->second);
            }

            auto name_it = name_batch.find(fid);
            if (name_it != name_batch.end()) {
                results[i].merged.name_counts =
                    std::move(name_it->second.counts);
            }
        }
    } catch (const std::exception& e) {
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (!results[i].success && results[i].error_message.empty()) {
                results[i].success = false;
                results[i].error_message = e.what();
            }
        }
    }

    co_return results;
}

}  // namespace dftracer::utils::utilities::composites::dft::statistics
