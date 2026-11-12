// SPDX-License-Identifier: MIT
#include <dftracer/utils/json/json_value.h>

#include <cstddef>

namespace dftracer::utils::json {

JsonValue JsonValue::at(const char* path) const {
    if (!valid_ || !path) return JsonValue();

    JsonValue current = *this;
    const char* p = path;

    while (*p) {
        if (*p == '.') {
            ++p;
            continue;
        }
        if (*p == '[') {
            ++p;
            std::size_t idx = 0;
            bool any = false;
            while (*p >= '0' && *p <= '9') {
                idx = idx * 10 + static_cast<std::size_t>(*p - '0');
                ++p;
                any = true;
            }
            if (*p == ']') ++p;
            if (!any) return JsonValue();
            current = current[idx];
            if (!current.exists()) return JsonValue();
            continue;
        }

        const char* start = p;
        while (*p && *p != '.' && *p != '[') ++p;
        dstd::StringView key_sv(start, static_cast<std::size_t>(p - start));

        JsonValue next = current[key_sv];
        // A numeric segment addresses an array element when the object-key
        // lookup found nothing and the current node is an array (e.g.
        // "tags.0.name").
        if (!next.exists() && current.is_array()) {
            bool all_digits = !key_sv.empty();
            std::size_t idx = 0;
            for (char c : key_sv) {
                if (c < '0' || c > '9') {
                    all_digits = false;
                    break;
                }
                idx = idx * 10 + static_cast<std::size_t>(c - '0');
            }
            if (all_digits) next = current[idx];
        }
        // Flat-key fallback: an object member name may itself contain dots
        // (e.g. the arg key "cqe.raw_ns" is one flat member, not a nested
        // object). The single-segment lookup above is the nested attempt and
        // wins when it exists; only on its miss do we try progressively longer
        // dot-joined keys, longest first, then continue the walk from the end
        // of the matched key so a value nested under a flat key still resolves.
        if (!next.exists() && current.is_object() && *p == '.') {
            const char* run_end = p;
            while (*run_end && *run_end != '[') ++run_end;
            for (const char* end = run_end; end > p;) {
                dstd::StringView cand(start,
                                      static_cast<std::size_t>(end - start));
                JsonValue c = current[cand];
                if (c.exists()) {
                    next = c;
                    p = end;
                    break;
                }
                --end;
                while (end > p && *end != '.') --end;
            }
        }
        current = next;
        if (!current.exists()) return JsonValue();
    }

    return current;
}

JsonValue JsonValue::at(dstd::StringView path) const {
    const dstd::String path_str(path);
    return at(path_str.c_str());
}

}  // namespace dftracer::utils::json
