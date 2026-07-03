#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VISITORS_VISITOR_DOM_HELPERS_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VISITORS_VISITOR_DOM_HELPERS_H

#include <simdjson.h>

#include <string_view>

namespace dftracer::utils::utilities::composites::dft::visitors {

// Read `key` from a DOM object as a string. Returns a view into the DOM (no
// copy); empty view when absent or not a string.
inline std::string_view dom_string(simdjson::dom::element obj,
                                   std::string_view key) {
    auto r = obj[key];
    if (r.error()) return {};
    auto v = r.value_unsafe();
    if (!v.is_string()) return {};
    return v.get_string().value_unsafe();
}

}  // namespace dftracer::utils::utilities::composites::dft::visitors

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VISITORS_VISITOR_DOM_HELPERS_H
