#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/sqlite/async.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>

namespace dftracer::utils::utilities::composites::dft::statistics {

using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::internal::get_logical_path;

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

    auto do_query = [&input, &result]() -> TraceStatistics {
        try {
            IndexDatabase idx_db(result.idx_path);

            int fid =
                idx_db.get_file_info_id(get_logical_path(input.file_path));
            if (fid < 0) {
                result.success = false;
                result.error_message =
                    "File not found in bloom index: " + input.file_path;
                return result;
            }

            auto chunks =
                indexing::queries::query_chunk_statistics(idx_db.sql_db(), fid);

            if (chunks.empty()) {
                result.success = true;
                result.num_chunks = 0;
                return result;
            }

            result.num_chunks = chunks.size();
            result.merged = chunks[0].stats;
            for (std::size_t i = 1; i < chunks.size(); ++i) {
                result.merged.merge_from(chunks[i].stats);
            }
            result.success = true;
        } catch (const std::exception& e) {
            result.success = false;
            result.error_message = e.what();
        }
        return result;
    };

    co_return co_await sqlite::run(do_query);
}

}  // namespace dftracer::utils::utilities::composites::dft::statistics
