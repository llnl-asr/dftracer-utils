// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_MEMORY_CONFIG_H
#define DFTRACER_UTILS_COMMON_MEMORY_CONFIG_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>

namespace dftracer::utils::common {

/**
 * Fills the budget, hard_cap and headroom fields from the *_ENV variables of
 * constants::memory_settings. An unset or empty one leaves the field zero,
 * which selects the default (a third of available memory for the budget, no
 * cap). A bad value is an ErrorCode::InvalidArgument naming the variable.
 */
dstd::Result<memory::Config> memory_config_from_env();

/**
 * Applies memory_config_from_env() to the memory library once per process.
 * Safe to call from every Runtime, the Python module init and each CLI main.
 * Later calls apply nothing and return the first call's result, so a bad value
 * fails every caller alike.
 */
dstd::Result<void> configure_memory_from_env();

/**
 * OverBudget, OverCap and Pending map to MemoryBudgetExceeded. PoolTableFull
 * and SlotTableFull map to Internal, since a full table is a sizing bug that a
 * caller cannot wait out. Cancelled and Ok map to Unknown, since no code names
 * a cancellation.
 */
constexpr ErrorCode error_code_of(memory::Status status) noexcept {
    switch (status) {
        case memory::Status::OverBudget:
        case memory::Status::OverCap:
        case memory::Status::Pending:
            return ErrorCode::MemoryBudgetExceeded;
        case memory::Status::PoolTableFull:
        case memory::Status::SlotTableFull:
            return ErrorCode::Internal;
        case memory::Status::Cancelled:
        case memory::Status::Ok:
            return ErrorCode::Unknown;
    }
    return ErrorCode::Unknown;
}

}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_MEMORY_CONFIG_H
