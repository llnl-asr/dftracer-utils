#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_RESERVED_ARGS_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_RESERVED_ARGS_H

#include <string_view>

namespace dftracer::utils::utilities::composites::dft::aggregators {

// Reserved event-arg keys that the aggregator handles specially (built-in
// metrics + hashes) and therefore must not treat as custom user fields. Shared
// by the aggregation visitor and the aggregation-logic updater so the set
// cannot drift between them.
inline bool is_reserved_arg(std::string_view k) {
    if (k.empty()) return false;
    switch (k[0]) {
        case 'h':
            return k == "hhash";
        case 'f':
            return k == "fhash";
        case 'd':
            return k == "dur" || k == "dur_sum" || k == "dur_min" ||
                   k == "dur_max" || k == "dft_cnt";
        case 'r':
            return k == "ret" || k == "ret_sum" || k == "ret_min" ||
                   k == "ret_max";
        case 'o':
            return k == "offset" || k == "offset_sum" || k == "offset_min" ||
                   k == "offset_max";
    }
    return false;
}

// True for a pre-aggregated metric field name (ends in _sum / _min / _max).
inline bool is_preagg_suffix(std::string_view k) {
    if (k.size() <= 4) return false;
    const std::string_view tail = k.substr(k.size() - 4);
    return tail == "_sum" || tail == "_min" || tail == "_max";
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_RESERVED_ARGS_H
