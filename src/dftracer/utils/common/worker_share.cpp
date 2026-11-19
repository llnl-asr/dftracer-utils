// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/memory_config.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/common/worker_share.h>
#include <dftracer/utils/memory/budget.h>

#include <algorithm>
#include <atomic>

namespace dftracer::utils::common {

namespace {
std::atomic<std::uint64_t> g_unshared_budget{0};
}  // namespace

dstd::Result<WorkerShare> worker_share(std::size_t workers, bool pin) {
    workers = std::max<std::size_t>(workers, 1);
    std::uint64_t budget = pin ? g_unshared_budget.load() : 0;
    if (budget == 0) {
        auto config = memory_config_from_env();
        if (!config) return dstd::Unexpected(std::move(config).error());
        budget = memory::resolve_spill_budget(config->budget);
        if (pin) {
            std::uint64_t expected = 0;
            g_unshared_budget.compare_exchange_strong(expected, budget);
            budget = g_unshared_budget.load();
        }
    }
    WorkerShare share;
    share.threads = std::max<std::size_t>(hardware_concurrency() / workers, 1);
    share.io_threads = share.threads;
    share.memory_budget = static_cast<std::size_t>(std::max<std::uint64_t>(
        budget / workers, memory::MIN_MEMORY_BUDGET_BYTES));
    return share;
}

}  // namespace dftracer::utils::common
