// SPDX-License-Identifier: MIT
// The dftu_schema builders: how a provider or a node declares column types
// across the C ABI.

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/schema_types.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace dftracer::utils::dataframe {
namespace {

// A type this ABI lets a provider nest a child field under: DataType::fields
// holds one entry for List/LargeList/FixedSizeList/Map (the element or
// key/value entry type) and any number for Struct.
bool is_nestable(TypeId id) {
    switch (id) {
        case TypeId::List:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Struct:
        case TypeId::Map:
            return true;
        default:
            return false;
    }
}

/// How many children `id` accepts: a Struct takes any number, every other
/// nestable takes exactly one (the element, or a Map's entries).
bool accepts_another_child(const DataType& dt) {
    if (!is_nestable(dt.id)) return false;
    return dt.id == TypeId::Struct || dt.fields.empty();
}

Field to_field(const char* name, ::dftu_dtype type, std::int32_t nullable,
               ::dftu_time_unit time_unit, const char* tz,
               std::int32_t decimal_precision, std::int32_t decimal_scale,
               std::int32_t fixed_size) {
    Field f;
    f.name = name ? name : "";
    f.nullable = nullable != 0;
    f.type.id = static_cast<TypeId>(type);
    f.type.set_time_unit(static_cast<TimeUnit>(time_unit));
    if (tz) f.type.set_timezone(tz);
    f.type.set_decimal(decimal_precision, decimal_scale);
    f.type.set_fixed_size(fixed_size);
    return f;
}

// Resolves `schema->paths[index]` against `schema->top`, or NULL if `index`
// is out of range. Recomputed from the root on every call rather than
// cached, since a sibling insertion earlier in the same schema may have
// reallocated any vector along the path.
Field* resolve_field(::dftu_schema* schema, std::int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= schema->paths.size())
        return nullptr;
    const dstd::Vector<std::int32_t>& path =
        schema->paths[static_cast<std::size_t>(index)];
    Field* f = &schema->top[static_cast<std::size_t>(path[0])];
    for (std::size_t i = 1; i < path.size(); ++i)
        f = &f->type.fields[static_cast<std::size_t>(path[i])];
    return f;
}

// A schema is well-formed only if every nested field count matches what its
// type allows: exactly one for List/LargeList/FixedSizeList/Map, any number
// for Struct, none for a scalar. A provider that gets this wrong (e.g. two
// children under a List) gets the whole declared schema discarded rather
// than a silently truncated or misinterpreted one.
bool is_well_formed(const DataType& dt) {
    if (dt.id == TypeId::Struct) {
        for (const Field& f : dt.fields)
            if (!is_well_formed(f.type)) return false;
        return true;
    }
    if (is_nestable(dt.id))
        return dt.fields.size() == 1 && is_well_formed(dt.fields[0].type);
    return dt.fields.empty();
}

}  // namespace

// Converts a provider's declared dftu_schema into a Schema, but only if it
// names exactly `names`, in order, and every field is well-formed; otherwise
// returns nullopt so the caller falls back to Unknown for every column, the
// same as a provider that declared nothing.
dstd::Optional<Schema> to_schema(const ::dftu_schema& built,
                                 const dstd::Vector<dstd::String>& names) {
    if (built.top.size() != names.size()) return dftracer::utils::dstd::nullopt;
    Schema s;
    s.fields.reserve(built.top.size());
    for (std::size_t i = 0; i < built.top.size(); ++i) {
        const Field& f = built.top[i];
        if (f.name != names[i] || !is_well_formed(f.type))
            return dftracer::utils::dstd::nullopt;
        s.fields.push_back(f);
    }
    return s;
}

}  // namespace dftracer::utils::dataframe

extern "C" {

int32_t dftu_schema_add_field(dftu_schema* schema, const char* name,
                              dftu_dtype type, int32_t nullable,
                              dftu_time_unit time_unit, const char* tz,
                              int32_t decimal_precision, int32_t decimal_scale,
                              int32_t fixed_size) {
    if (!schema || !name) return -1;
    if (type < DFTU_TYPE_UNKNOWN || type > DFTU_TYPE_MAP) return -1;
    schema->top.push_back(dftracer::utils::dataframe::to_field(
        name, type, nullable, time_unit, tz, decimal_precision, decimal_scale,
        fixed_size));
    const int32_t idx = static_cast<int32_t>(schema->paths.size());
    schema->paths.push_back({static_cast<int32_t>(schema->top.size() - 1)});
    return idx;
}

int32_t dftu_schema_add_child_field(dftu_schema* schema, int32_t parent_index,
                                    const char* name, dftu_dtype type,
                                    int32_t nullable, dftu_time_unit time_unit,
                                    const char* tz, int32_t decimal_precision,
                                    int32_t decimal_scale, int32_t fixed_size) {
    if (!schema || !name) return -1;
    if (type < DFTU_TYPE_UNKNOWN || type > DFTU_TYPE_MAP) return -1;
    dftracer::utils::dataframe::Field* parent =
        dftracer::utils::dataframe::resolve_field(schema, parent_index);
    // Refused at the call that would break the shape, so a schema under
    // construction is never in a state to_schema has to throw away: a second
    // element under a List is rejected here rather than discovered at the end.
    if (!parent ||
        !dftracer::utils::dataframe::accepts_another_child(parent->type))
        return -1;

    parent->type.fields.push_back(dftracer::utils::dataframe::to_field(
        name, type, nullable, time_unit, tz, decimal_precision, decimal_scale,
        fixed_size));

    dftracer::utils::dstd::Vector<int32_t> path =
        schema->paths[static_cast<std::size_t>(parent_index)];
    path.push_back(static_cast<int32_t>(parent->type.fields.size() - 1));
    const int32_t idx = static_cast<int32_t>(schema->paths.size());
    schema->paths.push_back(std::move(path));
    return idx;
}

int32_t dftu_schema_field_count(const dftu_schema* schema) {
    if (!schema) return -1;
    return static_cast<int32_t>(schema->top.size());
}

int32_t dftu_schema_copy_field(dftu_schema* out, const dftu_schema* schema,
                               int32_t i) {
    if (!out || !schema) return -1;
    if (i < 0 || static_cast<std::size_t>(i) >= schema->top.size()) return -1;
    out->top.push_back(schema->top[static_cast<std::size_t>(i)]);
    const int32_t idx = static_cast<int32_t>(out->paths.size());
    out->paths.push_back({static_cast<int32_t>(out->top.size() - 1)});
    return idx;
}

}  // extern "C"
