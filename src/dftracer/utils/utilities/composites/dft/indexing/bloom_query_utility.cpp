#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/sqlite/async.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_query_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>

#include <algorithm>
#include <set>
#include <unordered_set>

namespace dftracer::utils::utilities::composites::dft::indexing {

namespace {

// Hash dimension names that support resolution
static const std::unordered_set<std::string> HASH_DIMENSIONS = {
    "hhash", "fhash", "shash"};

// Simple heuristic: hex strings of 16 chars look like hashes
bool looks_like_hash(const std::string& value) {
    if (value.size() != 16) return false;
    for (char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

}  // namespace

coro::CoroTask<BloomQueryOutput> BloomQueryUtility::process(
    const BloomQueryInput& input) {
    if (input.predicates.empty()) {
        co_return BloomQueryOutput{true, {}, 0, true};
    }

    auto do_query = [&input]() -> BloomQueryOutput {
        BloomQueryOutput out;
        out.success = false;
        out.file_may_match = false;

        try {
            BloomIndexDatabase bidx(input.bidx_path);
            int file_info_id = bidx.get_file_info_id(input.file_path);
            if (file_info_id < 0) {
                DFTRACER_UTILS_LOG_WARN(
                    "BloomQuery: file not found in bloom index: %s",
                    input.file_path.c_str());
                out.success = true;
                out.file_may_match = true;
                return out;
            }

            auto indexed_dims =
                queries::query_index_dimensions(bidx.db(), file_info_id);
            std::unordered_set<std::string> indexed_set(indexed_dims.begin(),
                                                        indexed_dims.end());

            std::unordered_map<std::string, std::vector<std::string>>
                effective_predicates;

            for (const auto& [dimension, values] : input.predicates) {
                if (indexed_set.find(dimension) == indexed_set.end()) {
                    DFTRACER_UTILS_LOG_WARN(
                        "BloomQuery: dimension '%s' not indexed, "
                        "skipping",
                        dimension.c_str());
                    continue;
                }

                std::vector<std::string> resolved_values;
                for (const auto& val : values) {
                    if (HASH_DIMENSIONS.count(dimension) &&
                        !looks_like_hash(val)) {
                        auto hashes = queries::query_hash_by_resolved(
                            bidx.db(), dimension, val);
                        for (auto& h : hashes) {
                            resolved_values.push_back(std::move(h));
                        }
                    } else {
                        resolved_values.push_back(val);
                    }
                }

                if (!resolved_values.empty()) {
                    effective_predicates[dimension] =
                        std::move(resolved_values);
                }
            }

            if (effective_predicates.empty()) {
                out.file_may_match = true;
                out.success = true;
                return out;
            }

            // Step 1: File-level bloom check (fast skip)
            for (const auto& [dimension, values] : effective_predicates) {
                auto file_bloom = queries::query_file_bloom_filter(
                    bidx.db(), file_info_id, dimension);
                if (!file_bloom) {
                    continue;
                }

                auto bloom =
                    BloomFilter::from_blob(file_bloom->bloom_data.data(),
                                           file_bloom->bloom_data.size());

                bool any_match = false;
                for (const auto& val : values) {
                    if (bloom.possibly_contains(val)) {
                        any_match = true;
                        break;
                    }
                }

                if (!any_match) {
                    out.file_may_match = false;
                    out.success = true;
                    return out;
                }
            }

            // Step 2: Chunk-level bloom check
            std::set<std::uint64_t>* candidate_set = nullptr;
            std::set<std::uint64_t> current_candidates;

            for (const auto& [dimension, values] : effective_predicates) {
                auto chunk_blooms = queries::query_chunk_bloom_filters(
                    bidx.db(), file_info_id, dimension);

                if (chunk_blooms.empty()) {
                    continue;
                }

                if (out.total_checkpoints == 0) {
                    out.total_checkpoints = chunk_blooms.size();
                }

                std::set<std::uint64_t> dim_candidates;
                for (const auto& cb : chunk_blooms) {
                    auto bloom = BloomFilter::from_blob(cb.bloom_data.data(),
                                                        cb.bloom_data.size());

                    bool any_match = false;
                    for (const auto& val : values) {
                        if (bloom.possibly_contains(val)) {
                            any_match = true;
                            break;
                        }
                    }

                    if (any_match) {
                        dim_candidates.insert(cb.checkpoint_idx);
                    }
                }

                if (candidate_set == nullptr) {
                    current_candidates = std::move(dim_candidates);
                    candidate_set = &current_candidates;
                } else {
                    std::set<std::uint64_t> intersection;
                    std::set_intersection(
                        candidate_set->begin(), candidate_set->end(),
                        dim_candidates.begin(), dim_candidates.end(),
                        std::inserter(intersection, intersection.begin()));
                    current_candidates = std::move(intersection);
                }
            }

            if (candidate_set != nullptr) {
                out.candidate_checkpoints.assign(candidate_set->begin(),
                                                 candidate_set->end());
            }

            out.file_may_match = !out.candidate_checkpoints.empty();
            out.success = true;
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN(
                "BloomQuery: database error for %s: %s, "
                "assuming match",
                input.file_path.c_str(), e.what());
            out.file_may_match = true;
            out.success = true;
        }

        return out;
    };

    co_return co_await sqlite::run(do_query);
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing
