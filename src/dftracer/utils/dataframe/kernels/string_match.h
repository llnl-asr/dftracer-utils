// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_STRING_MATCH_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_STRING_MATCH_H

#include <dftracer/utils/dataframe/abi.h>
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
    dftu_series* (*eq)(const dftu_series* v, const char* rhs,
                       std::int32_t rhs_size);
    dftu_series* (*contains)(const dftu_series* v, const char* needle,
                             std::int32_t needle_size);
    dftu_series* (*starts_with)(const dftu_series* v, const char* prefix,
                                std::int32_t prefix_size);
    dftu_series* (*ends_with)(const dftu_series* v, const char* suffix,
                              std::int32_t suffix_size);
};

/// Which build of the kernels to use. Auto takes Jcc on a CPU with the Intel
/// JCC erratum when that build exists, else Plain.
enum class StringKernel { Auto, Plain, Jcc };

// The kernels of each build: the same code, the second one assembled with
// branch padding against the JCC erratum. jcc::table exists only when the
// build defines DFTRACER_UTILS_HAS_JCC_VARIANT as 1.
namespace plain {
const StringMatch& table();
}
namespace jcc {
const StringMatch& table();
}

/// The build named by @p request ("plain", "jcc" or "auto"; empty is "auto")
/// on a CPU that has the erratum when @p cpu_affected. An unknown request logs
/// a warning and means auto; "jcc" without that build logs a warning and
/// gives plain.
const StringMatch& resolve_string_match(dstd::StringView request,
                                        bool cpu_affected);

/// The kernels this process uses: @ref resolve_string_match of the
/// DFTRACER_UTILS_STRING_KERNEL variable and the CPU, chosen on first use.
const StringMatch& string_match();

/// The build @p kind names, or null when it was not built (Jcc) or is Auto.
const StringMatch* string_match_variant(StringKernel kind);

}  // namespace dftracer::utils::dataframe::kernels

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_STRING_MATCH_H
