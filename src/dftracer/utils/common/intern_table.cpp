// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/intern_table.h>

#include <new>

namespace dftracer::utils::common {

InternRef InternTable::create() {
    auto handle = memory::detail::Manager::get().root().child(
        "intern", memory::detail::PoolKind::Results);
    if (!handle) {
        throw DFTUtilsException(ErrorCode::MemoryBudgetExceeded,
                                "intern table: the pool table is full");
    }
    void* mem = memory::detail::allocate(sizeof(InternTable),
                                         alignof(InternTable), *handle);
    try {
        return InternRef::adopt(new (mem) InternTable(std::move(handle)));
    } catch (...) {
        memory::detail::deallocate(mem, sizeof(InternTable));
        throw;
    }
}

void InternTable::release() noexcept {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    // The table lives in its own pool, so the handle leaves it before the
    // table is freed and closes the pool last.
    memory::detail::PoolHandle handle = std::move(handle_);
    this->~InternTable();
    memory::detail::deallocate(this, sizeof(InternTable));
    handle.close();
}

InternTable* current_intern() noexcept {
    return static_cast<InternTable*>(memory::detail::task_user());
}

void enter_intern(InternTable* table) noexcept {
    memory::detail::set_task_user(table);
}

}  // namespace dftracer::utils::common
