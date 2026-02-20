#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_KEY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_KEY_H

#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace dftracer::utils::utilities::composites::dft::aggregators {

struct AggregationKey {
    std::string cat;
    std::string name;
    std::uint64_t pid;
    std::uint64_t tid;
    std::string hhash;
    std::string fhash;
    std::uint64_t time_bucket;

    std::unordered_map<std::string, std::string> extra_keys;

    bool operator==(const AggregationKey& other) const {
        return cat == other.cat && name == other.name && pid == other.pid &&
               tid == other.tid && hhash == other.hhash &&
               fhash == other.fhash && time_bucket == other.time_bucket &&
               extra_keys == other.extra_keys;
    }
};

struct AggregationKeyHash {
    std::size_t operator()(const AggregationKey& key) const {
        utilities::hash::HasherUtility hasher;
        hasher.update(key.cat);
        hasher.update(key.name);
        hasher.update(key.pid);
        hasher.update(key.tid);
        hasher.update(key.hhash);
        hasher.update(key.fhash);
        hasher.update(key.time_bucket);
        for (const auto& [k, v] : key.extra_keys) {
            hasher.update(k);
            hasher.update(v);
        }
        return hasher.get_hash().value;
    }
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_KEY_H
