#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/intern_table.h>

#include <new>

namespace dftracer::utils {

InternRef InternTable::create() {
    auto handle = memory::Manager::get().root().child(
        "intern", memory::PoolKind::Results);
    if (!handle) {
        throw DFTUtilsException(ErrorCode::MemoryBudgetExceeded,
                                "intern table: the pool table is full");
    }
    void* mem =
        memory::allocate(sizeof(InternTable), alignof(InternTable), *handle);
    try {
        return InternRef::adopt(new (mem) InternTable(std::move(handle)));
    } catch (...) {
        memory::deallocate(mem, sizeof(InternTable));
        throw;
    }
}

void InternTable::release() noexcept {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    // The table lives in its own pool, so the handle leaves it before the
    // table is freed and closes the pool last.
    memory::PoolHandle handle = std::move(handle_);
    this->~InternTable();
    memory::deallocate(this, sizeof(InternTable));
    handle.close();
}

InternTable* current_intern() noexcept {
    return static_cast<InternTable*>(memory::task_user());
}

void enter_intern(InternTable* table) noexcept { memory::set_task_user(table); }

}  // namespace dftracer::utils
