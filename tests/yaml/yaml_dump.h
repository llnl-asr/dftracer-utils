// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_YAML_DUMP_H
#define DFTRACER_UTILS_TESTS_YAML_DUMP_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/yaml/document.h>

namespace dftu_yaml_test {

// A compact text of a value tree: objects as {k:v,}, arrays as [v,], strings
// as 'text' and a null as ~. Two equal trees give equal text.
inline void dump(dftracer::utils::yaml::Value value,
                 dftracer::utils::dstd::String& out, int depth = 0) {
    using dftracer::utils::dstd::StringView;
    using dftracer::utils::yaml::Value;
    if (depth > 100) {
        out += "<deep>";
    } else if (value.is_object()) {
        out += '{';
        value.for_each_member([&](StringView key, Value member) {
            out += key;
            out += ':';
            dump(member, out, depth + 1);
            out += ',';
        });
        out += '}';
    } else if (value.is_array()) {
        out += '[';
        value.for_each_element([&](Value element) {
            dump(element, out, depth + 1);
            out += ',';
        });
        out += ']';
    } else if (value.is_string()) {
        out += '\'';
        out += value.raw();
        out += '\'';
    } else {
        out += '~';
    }
}

inline dftracer::utils::dstd::String dump(dftracer::utils::yaml::Value value) {
    dftracer::utils::dstd::String out;
    dump(value, out);
    return out;
}

}  // namespace dftu_yaml_test

#endif  // DFTRACER_UTILS_TESTS_YAML_DUMP_H
