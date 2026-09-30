#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_STRING_OPS_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_STRING_OPS_H

#include <dftracer/utils/dataframe/dataframe.h>

#include <string_view>

namespace dftracer::utils::duql {
struct CompiledPattern;
struct Substitution;
}  // namespace dftracer::utils::duql

namespace dftracer::utils::dataframe {

/// String predicates over a String/Binary column, each returning a Bool column.
/// A DICTIONARY input evaluates the predicate once per dictionary entry then
/// maps codes (O(dict + n)). Invalid (empty) for non-string inputs.
Series str_eq(const Series& v, std::string_view rhs);
Series str_contains(const Series& v, std::string_view needle);
Series str_starts_with(const Series& v, std::string_view prefix);

/// Bool mask of the duql pattern `p` over a String/Binary column; a row whose
/// match reaches the pattern's work limit is null.
Series str_pattern(const Series& v, const duql::CompiledPattern& p);

/// String column of `s` with every match of `p` replaced per `sub`; null rows
/// and rows whose match reaches the work limit are null.
Series str_regex_replace(const Series& v, const duql::CompiledPattern& p,
                         const duql::Substitution& sub);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_STRING_OPS_H
