#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>

namespace dftracer::utils::utilities::composites::dft::statistics {

TraceStatistics StatisticsAggregatorUtility::process(
    const StatisticsAggregatorInput& input) {
    TraceStatistics result;
    result.file_path = input.file_path;

    // Resolve bidx path
    if (!input.bidx_path.empty()) {
        result.bidx_path = input.bidx_path;
    } else {
        result.bidx_path = indexing::determine_bloom_index_path(
            input.file_path, input.index_dir);
    }

    // Check if bidx exists
    if (!fs::exists(result.bidx_path)) {
        result.success = false;
        result.error_message =
            "Bloom index file not found: " + result.bidx_path;
        return result;
    }

    try {
        indexing::BloomIndexDatabase bidx(result.bidx_path);
        bidx.init_schema();

        int fid = bidx.get_file_info_id(input.file_path);
        if (fid < 0) {
            result.success = false;
            result.error_message =
                "File not found in bloom index: " + input.file_path;
            return result;
        }

        auto chunks = indexing::queries::query_chunk_statistics(bidx.db(), fid);

        if (chunks.empty()) {
            result.success = true;
            result.num_chunks = 0;
            return result;
        }

        result.num_chunks = chunks.size();

        // Merge all chunks
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
}

}  // namespace dftracer::utils::utilities::composites::dft::statistics
