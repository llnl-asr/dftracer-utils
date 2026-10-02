#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_DICTIONARY_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_DICTIONARY_H

#include <dftracer/utils/dataframe/dataframe.h>

#include <cstdint>
#include <span>

namespace dftracer::utils::dataframe {

/// Dictionary-encode a FLAT String/Binary column: deduplicate the values into a
/// dictionary and return a DICTIONARY column of int32 codes over it. The trace
/// fast path (repeated names/paths). Invalid (empty) for other inputs.
Series dictionary_encode(const Series& v);

/// A DICTIONARY String column of `codes` over `values` (a flat String column,
/// moved in). `validity` is an Arrow bitmap of codes.size() bits, or null for
/// no nulls; a null row's code is ignored but must be in range.
Series dictionary_from_codes(std::span<const std::int32_t> codes,
                             Series&& values, const std::uint8_t* validity);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_DICTIONARY_H
