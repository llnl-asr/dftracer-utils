// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_JSON_DICT_BUILDER_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_JSON_DICT_BUILDER_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/parser.h>
#include <dftracer/utils/trace/args_map.h>
#include <dftracer/utils/trace/time_metric.h>
#include <simdjson.h>

namespace dftracer::utils::utilities::reader::internal {

using ArgsMap = dftracer::utils::trace::ArgsMap;

// One parsed JSON event split into top-level fields (`top`) and the nested
// "args" object (`args`), each held as an ArgsMap.
struct JsonDictEvent {
    ArgsMap top;
    ArgsMap args;
};

struct JsonDictBatch {
    dstd::Vector<JsonDictEvent> events;
};

// Coerce a simdjson value into `map` under `key`. Errored or non-scalar
// values are skipped.
void insert_simdjson_value(ArgsMap &map, dstd::StringView key,
                           simdjson::ondemand::value val);

// Parse one JSON row from `parser` into `ev`, routing the top-level "args"
// object into ev.args and all other fields into ev.top. When `time_scale`
// requests a target unit, top-level ts/dur are scaled from the file's native
// unit into it.
void parse_json_to_event(json::JsonParser &parser, JsonDictEvent &ev,
                         const trace::TimeScaleState &time_scale = {});

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_JSON_DICT_BUILDER_H
