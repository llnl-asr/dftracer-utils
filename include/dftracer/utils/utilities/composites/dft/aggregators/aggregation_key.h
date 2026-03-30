#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_KEY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_KEY_H

#include <dftracer/utils/core/common/string_intern.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::aggregators {

inline StringIntern& aggregation_intern() {
    static StringIntern intern;
    return intern;
}

struct AggregationKey {
    std::uint32_t cat_id = 0;
    std::uint32_t name_id = 0;
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    std::uint32_t hhash_id = 0;
    std::uint32_t fhash_id = 0;
    std::uint64_t time_bucket = 0;

    std::unique_ptr<std::vector<std::pair<std::uint32_t, std::uint32_t>>>
        extra_keys;

    bool operator==(const AggregationKey& other) const {
        if (cat_id != other.cat_id || name_id != other.name_id ||
            pid != other.pid || tid != other.tid ||
            hhash_id != other.hhash_id || fhash_id != other.fhash_id ||
            time_bucket != other.time_bucket) {
            return false;
        }
        bool has_extra = extra_keys && !extra_keys->empty();
        bool other_has_extra = other.extra_keys && !other.extra_keys->empty();
        if (has_extra != other_has_extra) return false;
        if (!has_extra) return true;
        if (extra_keys->size() != other.extra_keys->size()) return false;
        for (const auto& [k, v] : *extra_keys) {
            bool found = false;
            for (const auto& [ok, ov] : *other.extra_keys) {
                if (k == ok && v == ov) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
        return true;
    }

    AggregationKey() = default;

    AggregationKey(const AggregationKey& other)
        : cat_id(other.cat_id),
          name_id(other.name_id),
          pid(other.pid),
          tid(other.tid),
          hhash_id(other.hhash_id),
          fhash_id(other.fhash_id),
          time_bucket(other.time_bucket),
          extra_keys(
              other.extra_keys
                  ? std::make_unique<
                        std::vector<std::pair<std::uint32_t, std::uint32_t>>>(
                        *other.extra_keys)
                  : nullptr) {}

    AggregationKey& operator=(const AggregationKey& other) {
        if (this != &other) {
            cat_id = other.cat_id;
            name_id = other.name_id;
            pid = other.pid;
            tid = other.tid;
            hhash_id = other.hhash_id;
            fhash_id = other.fhash_id;
            time_bucket = other.time_bucket;
            extra_keys =
                other.extra_keys
                    ? std::make_unique<
                          std::vector<std::pair<std::uint32_t, std::uint32_t>>>(
                          *other.extra_keys)
                    : nullptr;
        }
        return *this;
    }

    AggregationKey(AggregationKey&&) = default;
    AggregationKey& operator=(AggregationKey&&) = default;

    std::string_view cat() const {
        return aggregation_intern().resolve(cat_id);
    }
    std::string_view name() const {
        return aggregation_intern().resolve(name_id);
    }
    std::string_view hhash() const {
        return hhash_id ? aggregation_intern().resolve(hhash_id)
                        : std::string_view{};
    }
    std::string_view fhash() const {
        return fhash_id ? aggregation_intern().resolve(fhash_id)
                        : std::string_view{};
    }
};

struct AggregationKeyHash {
    using is_avalanching = void;

    std::size_t operator()(const AggregationKey& key) const {
        constexpr std::uint64_t BASIS = 0xcbf29ce484222325ULL;
        constexpr std::uint64_t PRIME = 0x00000100000001B3ULL;
        std::uint64_t h = BASIS;
        auto mix = [&](std::uint64_t v) {
            h ^= v;
            h *= PRIME;
        };
        mix(key.cat_id);
        mix(key.name_id);
        mix(key.pid);
        mix(key.tid);
        mix(key.hhash_id);
        mix(key.fhash_id);
        mix(key.time_bucket);
        if (key.extra_keys) {
            for (const auto& [k, v] : *key.extra_keys) {
                mix(k);
                mix(v);
            }
        }
        return static_cast<std::size_t>(h);
    }
};

struct AggregationKeyEqual {
    bool operator()(const AggregationKey& a, const AggregationKey& b) const {
        return a == b;
    }
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_KEY_H
