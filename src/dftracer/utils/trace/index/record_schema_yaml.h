// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_RECORD_SCHEMA_YAML_H
#define DFTRACER_UTILS_TRACE_INDEX_RECORD_SCHEMA_YAML_H

#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/record_schema.h>

#include <array>
#include <cstddef>

namespace dftracer::utils::trace::index {

/// The spellings of the schema types, roles and units in a spec.
inline constexpr std::array<dstd::Pair<const char*, FieldType>, 5> TYPE_NAMES =
    {{
        {"bool", FieldType::Bool},
        {"int", FieldType::Int},
        {"float", FieldType::Float},
        {"string", FieldType::String},
        {"json", FieldType::Json},
    }};
inline constexpr std::array<dstd::Pair<const char*, Role>, 5> ROLE_NAMES = {{
    {"time", Role::Time},
    {"duration", Role::Duration},
    {"entity", Role::Entity},
    {"lane", Role::Lane},
    {"name", Role::Name},
}};
inline constexpr std::array<dstd::Pair<const char*, TimeUnit>, 4> UNIT_NAMES = {
    {
        {"ns", TimeUnit::Ns},
        {"us", TimeUnit::Us},
        {"ms", TimeUnit::Ms},
        {"s", TimeUnit::S},
    }};

/// Parses the YAML or JSON spec `text` into a @ref SchemaSpec; `source` names
/// it in errors. Returns an InvalidArgument error for text that is not YAML,
/// an unknown key, or a value of the wrong type. This is the one function that
/// reads YAML: it is built with exceptions and no YAML exception leaves it.
dstd::Result<SchemaSpec> parse_schema_spec(dstd::StringView text,
                                           const dstd::String& source);

}  // namespace dftracer::utils::trace::index

#endif  // DFTRACER_UTILS_TRACE_INDEX_RECORD_SCHEMA_YAML_H
