#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/sqlite/async.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <yyjson.h>

namespace dftracer::utils::utilities::composites::dft::statistics {

using dftracer::utils::utilities::common::json::JsonValue;
using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::internal::get_logical_path;
using fileio::lines::sources::async_streaming_gz_lines;

coro::CoroTask<TraceStatistics> StatisticsAggregatorUtility::process(
    const StatisticsAggregatorInput& input) {
    TraceStatistics result;
    result.file_path = input.file_path;

    if (!input.idx_path.empty()) {
        result.idx_path = input.idx_path;
    } else {
        result.idx_path =
            internal::determine_index_path(input.file_path, input.index_dir);
    }

    if (!fs::exists(result.idx_path)) {
        result.success = false;
        result.error_message = "Index file not found: " + result.idx_path;
        co_return result;
    }

    bool needs_streaming_fallback = false;

    auto do_query = [&input, &result,
                     &needs_streaming_fallback]() -> TraceStatistics {
        try {
            IndexDatabase idx_db(result.idx_path);

            int fid =
                idx_db.get_file_info_id(get_logical_path(input.file_path));
            if (fid < 0) {
                result.success = false;
                result.error_message =
                    "File not found in index: " + input.file_path;
                return result;
            }

            std::vector<indexing::queries::ChunkStatisticsResult> chunks;
            try {
                chunks = indexing::queries::query_chunk_statistics(
                    idx_db.sql_db(), fid);
            } catch (const std::exception&) {
                needs_streaming_fallback = true;
                return result;
            }

            if (chunks.empty()) {
                needs_streaming_fallback = true;
                return result;
            }

            result.num_chunks = chunks.size();
            result.merged = chunks[0].stats;
            for (std::size_t i = 1; i < chunks.size(); ++i) {
                result.merged.merge_from(chunks[i].stats);
            }

            auto dim_stats = indexing::queries::query_chunk_dimension_stats(
                idx_db.sql_db(), fid);
            for (const auto& ds : dim_stats) {
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
        } catch (const std::exception& e) {
            result.success = false;
            result.error_message = e.what();
        }
        return result;
    };

    result = co_await sqlite::run(do_query);

    if (!needs_streaming_fallback) {
        co_return result;
    }

    if (!fs::exists(input.file_path)) {
        result.success = false;
        result.error_message = "Trace file not found: " + input.file_path;
        co_return result;
    }

    /// Sequential fallback: stream the file line-by-line and compute
    /// statistics on-the-fly when the index has no chunk_statistics
    /// (e.g. file was below the index_threshold).
    try {
        indexing::ChunkStatistics stats;
        auto gen = async_streaming_gz_lines(input.file_path);
        while (auto line_opt = co_await gen.next()) {
            const auto& line = *line_opt;
            if (line.content.empty()) continue;

            yyjson_doc* doc = yyjson_read(
                line.content.data(), line.content.size(), YYJSON_READ_NOFLAG);
            if (!doc) continue;

            yyjson_val* root = yyjson_doc_get_root(doc);
            if (!root || !yyjson_is_obj(root)) {
                yyjson_doc_free(doc);
                continue;
            }

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
                // Skip malformed or partial events without
                // aborting the entire aggregation.
            }

            yyjson_doc_free(doc);
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

}  // namespace dftracer::utils::utilities::composites::dft::statistics
