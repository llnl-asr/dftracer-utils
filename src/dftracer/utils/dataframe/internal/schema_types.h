#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_SCHEMA_TYPES_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_SCHEMA_TYPES_H

#include <dftracer/utils/dataframe/lazyframe.h>

#include <cstdint>
#include <vector>

// Definition of the opaque dftu_schema (dataframe/abi.h) the dftu_schema_add_*
// API builds into. `top` mirrors dataframe::Schema::fields exactly (either can
// be read as the other), so a producer holding a typed Schema (LazyFrame's
// plan) or a consumer wanting one back (a node's declared output) can copy
// straight into/out of it. `paths` resolves a returned field index back into
// `top`/nested DataType::fields for dftu_schema_add_child_field; recomputed
// from the root on every lookup, since a sibling insertion earlier in the
// schema may have reallocated some vector along the path.
struct dftu_schema {
    std::vector<dftracer::utils::dataframe::Field> top;
    std::vector<std::vector<std::int32_t>> paths;
};

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_SCHEMA_TYPES_H
