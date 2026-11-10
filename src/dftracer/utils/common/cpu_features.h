// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_CPU_FEATURES_H
#define DFTRACER_UTILS_COMMON_CPU_FEATURES_H

#include <dftracer/utils/dstd/string.h>

#include <cstdint>

namespace dftracer::utils::common {

/// True when an Intel CPU of this family, model and stepping has the Jump
/// Conditional Code erratum: a conditional jump that crosses or ends on a
/// 32-byte boundary runs from the slow decoder.
bool jcc_erratum_affected(std::uint32_t family, std::uint32_t model,
                          std::uint32_t stepping);

/// True when the CPU this process runs on has the erratum. False on a CPU that
/// is not Intel x86-64. Reads CPUID once and keeps the answer.
bool cpu_has_jcc_erratum();

/// Which build of a padded kernel (one compiled twice, plain and with branch
/// padding against the erratum) to use. Auto takes Jcc on a CPU that has the
/// erratum, else Plain.
enum class JccKernels { Auto, Plain, Jcc };

/// "plain", "jcc" or "auto" (empty is "auto"). Any other text logs a warning
/// and means Auto.
JccKernels parse_jcc_kernels(dstd::StringView request);

/// Whether @p request asks for the padded build on a CPU that has the erratum
/// when @p cpu_affected.
bool prefers_jcc_kernels(JccKernels request, bool cpu_affected);

/// The DFTRACER_UTILS_JCC_KERNELS request of this process, read once.
JccKernels jcc_kernels_request();

}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_CPU_FEATURES_H
