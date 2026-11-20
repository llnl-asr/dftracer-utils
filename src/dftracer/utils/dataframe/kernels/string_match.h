// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_STRING_MATCH_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_STRING_MATCH_H

#include <dftracer/utils/common/cpu_features.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdint>

namespace dftracer::utils::duql {
struct CompiledPattern;
}  // namespace dftracer::utils::duql

namespace dftracer::utils::dataframe::kernels {

/// The string-match kernels of one build. Each takes a whole column and runs
/// the per-row loop inside, so a call through the table costs once per column
/// or chunk, never per row. Results and nulls match the dftu_series_str_*
/// function of the same name.
struct StringMatch {
    dftu_series* (*pattern)(const dftu_series* v,
                            const duql::CompiledPattern& p);
    dftu_series* (*eq)(const dftu_series* v, dstd::StringView rhs);
    dftu_series* (*contains)(const dftu_series* v, dstd::StringView needle);
    dftu_series* (*starts_with)(const dftu_series* v, dstd::StringView prefix);
    dftu_series* (*ends_with)(const dftu_series* v, dstd::StringView suffix);
};

// The kernels of each build: the same code, the second one assembled with
// branch padding against the JCC erratum. jcc::table exists only when the
// build defines DFTRACER_UTILS_HAS_JCC_VARIANT as 1.
namespace plain {
const StringMatch& table();
}
namespace jcc {
const StringMatch& table();
}

/// The build @p request names on a CPU that has the erratum when
/// @p cpu_affected. Jcc without that build logs a warning and gives plain.
const StringMatch& resolve_string_match(common::JccKernels request,
                                        bool cpu_affected);

/// The kernels this process uses: @ref resolve_string_match of
/// @ref common::jcc_kernels_request and the CPU, chosen on first use.
const StringMatch& string_match();

/// The build @p kind names, or null when it was not built (Jcc) or is Auto.
const StringMatch* string_match_variant(common::JccKernels kind);

}  // namespace dftracer::utils::dataframe::kernels

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_STRING_MATCH_H
