#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_QUERY_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_QUERY_UTILITY_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/predicate_parser_utility.h>

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

    BloomQueryInput& with_predicate_string(const std::string& pred_str) {
        PredicateParserInput parser_input;
        parser_input.existing_predicates = predicates;
        parser_input.with_predicate_string(pred_str);
        auto result = PredicateParserUtility{}.process(parser_input);
        if (result.success) {
            predicates = std::move(result.predicates);
        }
        return *this;
    }

    BloomQueryInput& with_predicates(const PredicateMap& preds) {
        for (const auto& [dim, vals] : preds) {
            auto& existing = predicates[dim];
            existing.insert(existing.end(), vals.begin(), vals.end());
        }
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

    coro::CoroTask<BloomQueryOutput> process(
        const BloomQueryInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_QUERY_UTILITY_H
