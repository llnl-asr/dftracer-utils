// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_TYPES_H
#define DFTRACER_UTILS_DUQL_TYPES_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/variant.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::duql {

/// Comparison operators for query expressions.
enum class CompareOp { Eq, Ne, Gt, Lt, Ge, Le };

/// Pattern-match operators. LIKE/ILIKE use SQL wildcards (% = any run, _ = one
/// UTF-8 character) and match the whole string; REGEX/IREGEX use the duql
/// regex dialect and search anywhere in the value; ICONTAINS is
/// case-insensitive substring containment (the "'sub' in field" form). The I*
/// variants are case-insensitive.
enum class MatchOp { Like, Ilike, Regex, Iregex, Icontains };

/// A typed literal value in a query expression.
using LiteralValue =
    dstd::Variant<dstd::String, int64_t, uint64_t, double, bool>;

/// A value bound to a parameter: a scalar, or a list that `x in $p` reads.
using ParamValue = dstd::Variant<LiteralValue, dstd::Vector<LiteralValue>>;

/// Values bound to `$name` parameters, by name without the `$`.
using Params = dstd::StringViewMap<ParamValue>;

/// A literal value node.
struct LiteralNode {
    LiteralValue value;
};

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_TYPES_H
