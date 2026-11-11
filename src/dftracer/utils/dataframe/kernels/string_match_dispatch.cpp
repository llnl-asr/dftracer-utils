// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/cpu_features.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/kernels/string_match.h>

#ifndef DFTRACER_UTILS_HAS_JCC_VARIANT
#define DFTRACER_UTILS_HAS_JCC_VARIANT 0
#endif

namespace dftracer::utils::dataframe::kernels {

namespace {

const StringMatch* jcc_table() {
#if DFTRACER_UTILS_HAS_JCC_VARIANT
    return &jcc::table();
#else
    return nullptr;
#endif
}

}  // namespace

const StringMatch& resolve_string_match(common::JccKernels request,
                                        bool cpu_affected) {
    if (!common::prefers_jcc_kernels(request, cpu_affected))
        return plain::table();
    if (const StringMatch* jcc = jcc_table()) return *jcc;
    if (request == common::JccKernels::Jcc)
        DFTRACER_UTILS_LOG_WARN(
            "{}=jcc, but this build has no jcc string kernels; using plain",
            common::constants::jcc_kernel_settings::KERNELS_ENV);
    return plain::table();
}

const StringMatch& string_match() {
    static const StringMatch& chosen = resolve_string_match(
        common::jcc_kernels_request(), common::cpu_has_jcc_erratum());
    return chosen;
}

const StringMatch* string_match_variant(common::JccKernels kind) {
    switch (kind) {
        case common::JccKernels::Plain:
            return &plain::table();
        case common::JccKernels::Jcc:
            return jcc_table();
        case common::JccKernels::Auto:
            break;
    }
    return nullptr;
}

}  // namespace dftracer::utils::dataframe::kernels
