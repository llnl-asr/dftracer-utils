#ifndef DFTRACER_UTILS_COMMON_MEMORY_CONFIG_H
#define DFTRACER_UTILS_COMMON_MEMORY_CONFIG_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>

namespace dftracer::utils {

/**
 * @brief Read the configure-time memory settings from the environment.
 *
 * Sets budget, hard_cap, limit_headroom and spill_headroom of the result from
 * constants::memory_settings::BUDGET_ENV, HARD_LIMIT_ENV, LIMIT_HEADROOM_ENV
 * and SPILL_HEADROOM_ENV. A byte value is a number or a number with a unit,
 * as parse_bytes reads it. An unset or empty variable leaves the field zero,
 * which selects the default: a third of the available memory for the budget,
 * no cap, and for each headroom a percent of the limit or the budget with a
 * floor. A value that does not parse is an ErrorCode::InvalidArgument that
 * names the variable. The backend and the decay time stay at their defaults:
 * the memory library reads those itself before its first allocation.
 */
Result<memory::Config> memory_config_from_env();

/**
 * @brief Apply memory_config_from_env() to the memory library, once per
 * process.
 *
 * Safe to call from every Runtime, the Python module init and each CLI main.
 * The first call reads the environment and applies it; every later call does
 * nothing and returns the first call's result, so a bad value fails each
 * caller the same way and nothing is applied.
 */
Result<void> configure_memory_from_env();

/**
 * @brief The ErrorCode for a failed memory::Status.
 *
 * OverBudget, OverCap and Pending are ErrorCode::MemoryBudgetExceeded.
 * PoolTableFull and SlotTableFull are ErrorCode::Internal: no code names an
 * exhausted resource, and a full table is a sizing bug, not a budget a caller
 * can wait out. Cancelled is ErrorCode::Unknown, as is the Cancelled
 * condition (no code names a cancellation). Ok is not an error and gives
 * ErrorCode::Unknown.
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

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_COMMON_MEMORY_CONFIG_H
