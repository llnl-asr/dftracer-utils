#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_JSON_DICT_BUILDER_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_JSON_DICT_BUILDER_H

#include <dftracer/utils/utilities/common/json/parser.h>
#include <dftracer/utils/utilities/composites/dft/args_map.h>
#include <simdjson.h>

#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::reader::internal {

using ArgsMap = dftracer::utils::utilities::composites::dft::ArgsMap;

// One parsed JSON event split into top-level fields (`top`) and the nested
// "args" object (`args`), each held as an ArgsMap.
struct JsonDictEvent {
    ArgsMap top;
    ArgsMap args;
};

struct JsonDictBatch {
    std::vector<JsonDictEvent> events;
};

// Coerce a simdjson value into `map` under `key`. Errored or non-scalar
// values are skipped.
void insert_simdjson_value(ArgsMap &map, std::string_view key,
                           simdjson::ondemand::value val);

// Parse one JSON row from `parser` into `ev`, routing the top-level "args"
// object into ev.args and all other fields into ev.top.
void parse_json_to_event(common::json::JsonParser &parser, JsonDictEvent &ev);

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_JSON_DICT_BUILDER_H
