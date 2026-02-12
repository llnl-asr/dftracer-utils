#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_QUERY_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_QUERY_UTILITY_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing {

struct BloomQueryInput {
    std::string bidx_path;
    std::string file_path;
    // dimension -> values (OR within dimension, AND across dimensions)
    std::unordered_map<std::string, std::vector<std::string>> predicates;

    BloomQueryInput& with_bidx_path(const std::string& path) {
        bidx_path = path;
        return *this;
    }

    BloomQueryInput& with_file_path(const std::string& path) {
        file_path = path;
        return *this;
    }

    BloomQueryInput& with_predicate(const std::string& dimension,
                                    const std::vector<std::string>& values) {
        predicates[dimension] = values;
        return *this;
    }
};

struct BloomQueryOutput {
    bool file_may_match = false;
    std::vector<std::uint64_t> candidate_checkpoints;
    std::uint64_t total_checkpoints = 0;
    bool success = false;
};

class BloomQueryUtility
    : public utilities::Utility<BloomQueryInput, BloomQueryOutput,
                                utilities::tags::Parallelizable> {
   public:
    BloomQueryUtility() = default;

    BloomQueryOutput process(const BloomQueryInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_QUERY_UTILITY_H
