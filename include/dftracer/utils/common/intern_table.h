#ifndef DFTRACER_UTILS_COMMON_INTERN_TABLE_H
#define DFTRACER_UTILS_COMMON_INTERN_TABLE_H

#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/memory/manager.h>

#include <atomic>
#include <cstdint>
#include <string_view>
#include <utility>

namespace dftracer::utils {

/// The id of a string in an InternTable, or NONE. Ids of two tables must not
/// mix.
struct InternId {
    std::uint32_t value;

    static const InternId NONE;

    constexpr InternId() noexcept : value(dstd::StringIntern::NO_ID) {}
    constexpr explicit InternId(std::uint32_t v) noexcept : value(v) {}

    friend constexpr bool operator==(InternId a, InternId b) noexcept {
        return a.value == b.value;
    }
    friend constexpr bool operator!=(InternId a, InternId b) noexcept {
        return a.value != b.value;
    }
};

inline constexpr InternId InternId::NONE{dstd::StringIntern::NO_ID};

class InternRef;

/**
 * @brief A string table shared by reference count.
 *
 * Made by InternTable::create. It allocates everything from its own pool,
 * "intern" below the root, so the pool shows its bytes. The last InternRef
 * destroys the table and closes the pool; the pool is a Results pool, so a
 * chunk that a column still holds is adopted by the parent instead of being
 * reported as a leak. Thread safe.
 */
class InternTable {
   public:
    InternTable(const InternTable&) = delete;
    InternTable& operator=(const InternTable&) = delete;

    /// Throws DFTUtilsException when the pool table is full.
    static InternRef create();

    dstd::StringIntern& strings() noexcept { return strings_; }
    const dstd::StringIntern& strings() const noexcept { return strings_; }

    InternId intern(std::string_view sv) {
        return InternId(strings_.get_or_insert(sv));
    }

    /// Empty for NONE and for an id with no string.
    std::string_view get(InternId id) const {
        return id == InternId::NONE ? std::string_view{}
                                    : strings_.resolve(id.value);
    }

    /// The reference stays readable after the table is gone.
    memory::Pool& pool() const noexcept { return *handle_; }

    std::uint32_t use_count() const noexcept {
        return refs_.load(std::memory_order_relaxed);
    }

    void add_ref() noexcept { refs_.fetch_add(1, std::memory_order_relaxed); }
    void release() noexcept;

   private:
    explicit InternTable(memory::PoolHandle&& handle)
        : strings_(handle.get()), handle_(std::move(handle)) {}

    dstd::StringIntern strings_;
    memory::PoolHandle handle_;
    std::atomic<std::uint32_t> refs_{1};
};

/// An owning handle: a copy adds a reference, the destructor releases one.
class InternRef {
   public:
    InternRef() noexcept = default;
    /// Takes over one reference.
    static InternRef adopt(InternTable* table) noexcept {
        return InternRef(table);
    }
    /// Adds a reference; null stays null.
    static InternRef share(InternTable* table) noexcept {
        if (table) table->add_ref();
        return InternRef(table);
    }

    InternRef(const InternRef& other) noexcept : table_(other.table_) {
        if (table_) table_->add_ref();
    }
    InternRef(InternRef&& other) noexcept
        : table_(std::exchange(other.table_, nullptr)) {}
    InternRef& operator=(InternRef other) noexcept {
        std::swap(table_, other.table_);
        return *this;
    }
    ~InternRef() {
        if (table_) table_->release();
    }

    InternTable* get() const noexcept { return table_; }
    InternTable* operator->() const noexcept { return table_; }
    InternTable& operator*() const noexcept { return *table_; }
    explicit operator bool() const noexcept { return table_ != nullptr; }

   private:
    explicit InternRef(InternTable* table) noexcept : table_(table) {}

    InternTable* table_ = nullptr;
};

/// The table of the running task, or null. Read it once per batch.
InternTable* current_intern() noexcept;

/// Make @p table the calling thread's table without restoring it. For a
/// coroutine that enters its own table each time it resumes; whoever resumed
/// it restores its own with an InternScope.
void enter_intern(InternTable* table) noexcept;

/**
 * @brief Make a table the current one of this thread until destruction.
 *
 * Nests and restores the previous table. Holds no reference: the table must
 * outlive the scope.
 */
class InternScope {
   public:
    explicit InternScope(InternTable* table) noexcept
        : previous_(current_intern()) {
        enter_intern(table);
    }
    explicit InternScope(const InternRef& table) noexcept
        : InternScope(table.get()) {}
    InternScope(const InternScope&) = delete;
    InternScope& operator=(const InternScope&) = delete;
    ~InternScope() { enter_intern(previous_); }

   private:
    InternTable* previous_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_COMMON_INTERN_TABLE_H
