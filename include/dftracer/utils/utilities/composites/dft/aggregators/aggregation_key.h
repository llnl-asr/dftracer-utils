#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_KEY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_KEY_H

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
        std::size_t h = 0;

        auto hash_combine = [](std::size_t& seed, std::size_t value) {
            seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        };

        hash_combine(h, std::hash<std::string>{}(key.cat));
        hash_combine(h, std::hash<std::string>{}(key.name));
        hash_combine(h, std::hash<std::uint64_t>{}(key.pid));
        hash_combine(h, std::hash<std::uint64_t>{}(key.tid));
        hash_combine(h, std::hash<std::string>{}(key.hhash));
        hash_combine(h, std::hash<std::string>{}(key.fhash));
        hash_combine(h, std::hash<std::uint64_t>{}(key.time_bucket));

        for (const auto& [k, v] : key.extra_keys) {
            hash_combine(h, std::hash<std::string>{}(k));
            hash_combine(h, std::hash<std::string>{}(v));
        }

        return h;
    }
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_KEY_H
