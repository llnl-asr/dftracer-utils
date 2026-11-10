// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/cpu_features.h>
#include <dftracer/utils/common/platform_compat.h>

#include <cstring>

namespace dftracer::utils::common {

// Affected parts are the rows of "Mitigations for Jump Conditional Code
// Erratum", Intel document 341810-001 (November 2019), section 4.0 "Affected
// Processors". All are Intel family 6. Model 0xA5 (Comet Lake S and H) is not
// in that table; OpenJDK lists it as affected for any stepping
// (src/hotspot/cpu/x86/vm_version_x86.cpp, compute_has_intel_jcc_erratum,
// https://github.com/openjdk/jdk, GPL-2.0 with Classpath exception), so it is
// included here. Only that fact is taken; this code follows the Intel table.
bool jcc_erratum_affected(std::uint32_t family, std::uint32_t model,
                          std::uint32_t stepping) {
    if (family != 6) return false;
    switch (model) {
        case 0x4E:
        case 0x5E:
            return stepping == 0x3;
        case 0x55:
            return stepping == 0x4 || stepping == 0x7;
        case 0x8E:
            return stepping >= 0x9 && stepping <= 0xC;
        case 0x9E:
            return stepping == 0x9 || stepping == 0xA || stepping == 0xB ||
                   stepping == 0xD;
        case 0xA5:
            return true;
        case 0xA6:
            return stepping == 0x0;
        case 0xAE:
            return stepping == 0xA;
        default:
            return false;
    }
}

bool cpu_has_jcc_erratum() {
    static const bool affected = [] {
        std::uint32_t r[4];
        if (!cpuid_leaf(0, r)) return false;
        char vendor[12];
        std::memcpy(vendor, &r[1], 4);
        std::memcpy(vendor + 4, &r[3], 4);
        std::memcpy(vendor + 8, &r[2], 4);
        if (std::memcmp(vendor, "GenuineIntel", 12) != 0) return false;
        if (!cpuid_leaf(1, r)) return false;
        const std::uint32_t eax = r[0];
        const std::uint32_t family = (eax >> 8) & 0xF;
        const std::uint32_t model =
            ((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4);
        return jcc_erratum_affected(family, model, eax & 0xF);
    }();
    return affected;
}

}  // namespace dftracer::utils::common
