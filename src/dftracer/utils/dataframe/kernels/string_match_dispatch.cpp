// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/cpu_features.h>
#include <dftracer/utils/common/env.h>
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

const StringMatch& auto_table(bool cpu_affected) {
    const StringMatch* jcc = jcc_table();
    return cpu_affected && jcc ? *jcc : plain::table();
}

}  // namespace

const StringMatch& resolve_string_match(dstd::StringView request,
                                        bool cpu_affected) {
    if (request.empty() ||
        request == common::constants::string_kernel_settings::DEFAULT_KERNEL)
        return auto_table(cpu_affected);
    if (request == "plain") return plain::table();
    if (request == "jcc") {
        if (const StringMatch* jcc = jcc_table()) return *jcc;
        DFTRACER_UTILS_LOG_WARN(
            "{}=jcc, but this build has no jcc string kernels; using plain",
            common::constants::string_kernel_settings::KERNEL_ENV);
        return plain::table();
    }
    DFTRACER_UTILS_LOG_WARN(
        "{}={} is not plain, jcc or auto; using auto",
        common::constants::string_kernel_settings::KERNEL_ENV, request);
    return auto_table(cpu_affected);
}

const StringMatch& string_match() {
    static const StringMatch& chosen = []() -> const StringMatch& {
        const auto request = common::Env::get(
            common::constants::string_kernel_settings::KERNEL_ENV);
        return resolve_string_match(request ? *request : dstd::StringView{},
                                    common::cpu_has_jcc_erratum());
    }();
    return chosen;
}

const StringMatch* string_match_variant(StringKernel kind) {
    switch (kind) {
        case StringKernel::Plain:
            return &plain::table();
        case StringKernel::Jcc:
            return jcc_table();
        case StringKernel::Auto:
            break;
    }
    return nullptr;
}

}  // namespace dftracer::utils::dataframe::kernels
