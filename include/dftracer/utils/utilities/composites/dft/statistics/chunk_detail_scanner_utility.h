#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_CHUNK_DETAIL_SCANNER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_CHUNK_DETAIL_SCANNER_UTILITY_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/detailed_statistics.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::statistics {

struct ChunkDetailScanInput {
    std::string file_path;
    std::string index_path;
    std::size_t checkpoint_size = 0;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
    std::uint64_t checkpoint_idx = 0;
    std::size_t batch_size = 4 * 1024 * 1024;
    const std::vector<std::string>* filter_names = nullptr;
    const std::vector<std::string>* filter_categories = nullptr;
    const std::vector<std::string>* group_by = nullptr;
};

// Success payload; failures are reported via Result<ChunkDetailScanOutput>.
struct ChunkDetailScanOutput {
    DetailedStatistics stats;
};

class ChunkDetailScannerUtility
    : public utilities::Utility<ChunkDetailScanInput,
                                Result<ChunkDetailScanOutput>> {
   public:
    ChunkDetailScannerUtility() = default;

    coro::CoroTask<Result<ChunkDetailScanOutput>> process(
        const ChunkDetailScanInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::statistics

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_CHUNK_DETAIL_SCANNER_UTILITY_H
