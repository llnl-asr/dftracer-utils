#ifndef DFTRACER_UTILS_JSON_RECORD_PARSER_H
#define DFTRACER_UTILS_JSON_RECORD_PARSER_H

#include <simdjson.h>

namespace dftracer::utils::json {

/// A DOM parser for trace records: an integer wider than 64 bits is read as
/// a BIGINT element holding its digits instead of failing the whole record.
class RecordParser : public simdjson::dom::parser {
   public:
    RecordParser() { number_as_string(true); }
};

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_RECORD_PARSER_H
