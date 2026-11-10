// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_CPU_FEATURES_H
#define DFTRACER_UTILS_COMMON_CPU_FEATURES_H

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

}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_CPU_FEATURES_H
