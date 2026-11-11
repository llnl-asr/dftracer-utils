// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/intern_table.h>
#include <dftracer/utils/memory/detail/manager.h>

#include <new>

namespace dftracer::utils::common {

dstd::Result<InternRef> InternTable::create() {
    auto handle =
        memory::root_pool().child("intern", memory::PoolKind::Results);
    if (!handle) {
        return make_error(ErrorCode::MemoryBudgetExceeded,
                          "intern table: the pool table is full");
    }
    void* mem = memory::detail::allocate(sizeof(InternTable),
                                         alignof(InternTable), *handle);
    return InternRef::adopt(new (mem) InternTable(std::move(handle)));
}

void InternTable::release() noexcept {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    // The table lives in its own pool, so the handle leaves it before the
    // table is freed and closes the pool last.
    memory::PoolHandle handle = std::move(handle_);
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
