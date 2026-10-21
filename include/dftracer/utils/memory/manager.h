#ifndef DFTRACER_UTILS_MEMORY_MANAGER_H
#define DFTRACER_UTILS_MEMORY_MANAGER_H

#include <dftracer/utils/memory/heap.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::memory {

class Manager;
class Pool;
struct PoolAccess;
struct ManagerAccess;
struct ReclaimEntry;
struct ExternalEntry;
struct AttribCache;

inline constexpr std::size_t RECLAIMER_SLOTS = 4096;
inline constexpr std::size_t EXTERNAL_SLOTS = 256;

enum class PoolKind : std::uint8_t {
    /// Query state, arenas, hash tables. Closing it with live bytes or open
    /// children is a leak.
    Scratch,
    /// Results that outlive the query. Closing it leaves its live bytes in
    /// place and reports them as adopted by the parent.
    Results,
};

enum class Status : std::uint8_t {
    Ok,
    /// Bytes were requested from holders and are not free yet.
    Pending,
    OverBudget,
    OverCap,
    PoolTableFull,
    /// Every reclaimer or external slot is taken.
    SlotTableFull,
    /// The wait was abandoned because the executor is shutting down. It is not
    /// a budget condition and a retry cannot succeed.
    Cancelled,
};

struct Limits {
    /// Largest reserved total of the pool, counted with every pool below it by
    /// its rounded amount (see Pool::reserve); zero is no limit.
    std::size_t bytes = 0;
};

/**
 * @brief A reservation of bytes in one pool, released on destruction.
 *
 * Empty when Pool::reserve failed; status() says why. Moving transfers the
 * bytes. A reservation counts against the pool limit and the budget but
 * allocates nothing.
 */
class Reservation {
   public:
    Reservation() noexcept = default;
    Reservation(Reservation&& other) noexcept;
    Reservation& operator=(Reservation&& other) noexcept;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    ~Reservation();

    explicit operator bool() const noexcept {
        return pool_ != nullptr && status_ == Status::Ok;
    }
    Status status() const noexcept { return status_; }
    std::size_t bytes() const noexcept { return bytes_; }
    /**
     * @brief Reserve @p bytes more in the same pool, as Pool::reserve does.
     *
     * On failure the reservation keeps its bytes and the status says why. An
     * empty reservation has no pool to grow in: it returns its own failure,
     * or Status::OverBudget when it was default constructed.
     */
    Status grow(std::size_t bytes) noexcept;
    /**
     * @brief Grow as grow() does, through Pool::try_reserve: a refusal asks
     * no holder for bytes and trims nothing.
     */
    Status try_grow(std::size_t bytes) noexcept;
    /**
     * @brief Move up to @p bytes of @p other into this reservation without
     * touching the pool and return the bytes moved.
     *
     * Both must hold bytes of the same pool; an empty reservation takes the
     * pool of @p other.
     */
    std::size_t take_from(Reservation& other, std::size_t bytes) noexcept;
    void release() noexcept;

   private:
    friend class Pool;
    friend class ReserveWait;
    Reservation(Pool* pool, std::size_t bytes, Status status) noexcept
        : pool_(pool), bytes_(bytes), status_(status) {}

    Pool* pool_ = nullptr;
    std::size_t bytes_ = 0;
    Status status_ = Status::Ok;
};

enum class WaitState : std::uint8_t {
    Waiting,
    Granted,
    Cancelled,
};

/**
 * @brief A request for bytes in a pool that waits for a release to free room.
 *
 * Waiters are granted strictly in push order, and the grant stops at the
 * first waiter that does not fit. A grant applies the same checks as
 * Pool::reserve: the pool limit, each ancestor's limit and the budget with
 * the external bytes. It charges the bytes before the node turns Granted, so
 * no other reservation can take the room before the waiter resumes.
 *
 * The node is reference counted: create() returns one reference, push() adds
 * one for the list, and each other holder (a timer) calls retain(). The
 * last drop() frees it and releases granted bytes that nobody took. The pool
 * must stay open until the waiter is cancelled or its reservation is
 * released. Every call takes no lock and never waits.
 */
class ReserveWait {
   public:
    /// Runs at most once, on the thread that granted the waiter. It must only
    /// enqueue the waiter's work: it runs while that thread grants others.
    using Resume = void (*)(void* context) noexcept;

    /// Null when the heap has no memory. The node is charged to the general
    /// pool, so a node that outlives its pool is no leak of that pool.
    static ReserveWait* create(Pool& pool, std::size_t bytes, Resume resume,
                               void* context) noexcept;

    ReserveWait(const ReserveWait&) = delete;
    ReserveWait& operator=(const ReserveWait&) = delete;

    void retain() noexcept;
    void drop() noexcept;

    /**
     * @brief List the waiter, then grant waiters on the calling thread.
     *
     * Call once. Status::Ok means this call granted the waiter and Resume is
     * never called. Status::Pending means another thread grants it later and
     * calls Resume; from the push on, it may do so before push() returns.
     * Status::OverBudget means the request is larger than the budget or a
     * limit on the way to the root, so it can never fit; it is not listed.
     */
    Status push() noexcept;

    /// Move a waiting node to Cancelled. False when it was granted first.
    bool cancel() noexcept;

    WaitState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    std::size_t bytes() const noexcept { return bytes_; }

    /// The granted bytes as a Reservation, once. Empty when the node is not
    /// Granted or the bytes were taken.
    Reservation take() noexcept;

   private:
    friend struct ManagerAccess;
    ReserveWait(Pool& pool, std::size_t bytes, Resume resume,
                void* context) noexcept
        : bytes_(bytes), pool_(&pool), resume_(resume), context_(context) {}
    ~ReserveWait() = default;

    std::uint64_t arrival_ = 0;
    std::size_t bytes_;
    Pool* pool_;
    ReserveWait* next_ = nullptr;
    Resume resume_;
    void* context_;
    std::atomic<std::uint32_t> refs_{1};
    std::atomic<WaitState> state_{WaitState::Waiting};
    bool taken_ = false;
};

/**
 * @brief Owns an open pool and closes it on destruction.
 *
 * Empty when Pool::child failed; status() says why. Close each child before
 * its parent.
 */
class PoolHandle {
   public:
    PoolHandle() noexcept = default;
    PoolHandle(PoolHandle&& other) noexcept;
    PoolHandle& operator=(PoolHandle&& other) noexcept;
    PoolHandle(const PoolHandle&) = delete;
    PoolHandle& operator=(const PoolHandle&) = delete;
    ~PoolHandle();

    explicit operator bool() const noexcept { return pool_ != nullptr; }
    Status status() const noexcept { return status_; }
    Pool* get() const noexcept { return pool_; }
    Pool& operator*() const noexcept { return *pool_; }
    Pool* operator->() const noexcept { return pool_; }
    void close() noexcept;

   private:
    friend class Pool;
    PoolHandle(Pool* pool, std::uint32_t generation, Status status) noexcept
        : pool_(pool), generation_(generation), status_(status) {}

    Pool* pool_ = nullptr;
    std::uint32_t generation_ = 0;
    Status status_ = Status::Ok;
};

/**
 * @brief A holder that can give bytes back: a cursor, an arena, a cache.
 *
 * The manager never calls it and keeps no pointer to it. A pool over budget
 * posts a request to the holder's ReclaimSlot; the holder reads it with
 * ReclaimSlot::take_request() on its own thread and calls reclaim() there.
 */
class Reclaimer {
   public:
    virtual ~Reclaimer() = default;
    /// Bytes the holder could give back now.
    virtual std::size_t reclaimable() const = 0;
    /// Give back about @p target bytes and return how many were given.
    virtual std::size_t reclaim(std::size_t target) = 0;
};

/**
 * @brief Marks a pool as reclaiming until destruction.
 *
 * Reservations of the pool may pass the budget by the spill headroom and
 * never post requests, so a spill can allocate its buffers. Destroy it before
 * the pool closes.
 */
class ReclaimGuard {
   public:
    ReclaimGuard() noexcept = default;
    ReclaimGuard(ReclaimGuard&& other) noexcept : pool_(other.pool_) {
        other.pool_ = nullptr;
    }
    ReclaimGuard& operator=(ReclaimGuard&& other) noexcept;
    ReclaimGuard(const ReclaimGuard&) = delete;
    ReclaimGuard& operator=(const ReclaimGuard&) = delete;
    ~ReclaimGuard();

   private:
    friend class ReclaimSlot;
    explicit ReclaimGuard(Pool* pool) noexcept : pool_(pool) {}

    Pool* pool_ = nullptr;
};

/**
 * @brief A holder's entry in the reclaimer table, released on destruction.
 *
 * Empty when Pool::attach failed; every call below then does nothing. The
 * calls are thread safe and take no lock. A request that was posted while the
 * slot detached can reach the next holder of the same entry, so a request is
 * advice: act on it or ignore it.
 */
class ReclaimSlot {
   public:
    ReclaimSlot() noexcept = default;
    ReclaimSlot(ReclaimSlot&& other) noexcept;
    ReclaimSlot& operator=(ReclaimSlot&& other) noexcept;
    ReclaimSlot(const ReclaimSlot&) = delete;
    ReclaimSlot& operator=(const ReclaimSlot&) = delete;
    ~ReclaimSlot();

    explicit operator bool() const noexcept { return entry_ != nullptr; }
    Status status() const noexcept { return status_; }

    /// Tell the arbiter how many bytes the holder could give back. Cheap
    /// enough to call once per morsel.
    void set_reclaimable(std::size_t bytes) noexcept;

    /// The bytes asked for since the last call; resets them to zero.
    std::size_t take_request() noexcept;

    /// Mark the pool as reclaiming until the guard dies.
    ReclaimGuard begin_reclaim() noexcept;

    void detach() noexcept;

   private:
    friend class Pool;
    ReclaimSlot(ReclaimEntry* entry, Pool* pool, std::uint32_t generation,
                Status status) noexcept
        : entry_(entry),
          pool_(pool),
          generation_(generation),
          status_(status) {}

    ReclaimEntry* entry_ = nullptr;
    Pool* pool_ = nullptr;
    std::uint32_t generation_ = 0;
    Status status_ = Status::Ok;
};

/**
 * @brief Bytes the process holds outside this library, such as the RocksDB
 * memtables and caches, released on destruction.
 *
 * Counted bytes count toward the budget like reservations do; reported
 * bytes, such as spill files on a disk, show in Stats::external only. Empty
 * when attach_external failed; set() and add() then do nothing.
 */
class ExternalSlot {
   public:
    ExternalSlot() noexcept = default;
    ExternalSlot(ExternalSlot&& other) noexcept;
    ExternalSlot& operator=(ExternalSlot&& other) noexcept;
    ExternalSlot(const ExternalSlot&) = delete;
    ExternalSlot& operator=(const ExternalSlot&) = delete;
    ~ExternalSlot();

    explicit operator bool() const noexcept { return entry_ != nullptr; }
    Status status() const noexcept { return status_; }

    /// Whether bytes given to add() count toward the budget.
    enum class Kind : std::uint8_t { Counted, Reported };

    /// Replace the bytes this slot reports, all counted. Thread safe.
    void set(std::size_t bytes) noexcept;
    /// Add `delta` bytes of `kind`; a negative delta must not exceed what was
    /// added of that kind. Thread safe, and concurrent calls never lose one.
    void add(std::ptrdiff_t delta, Kind kind) noexcept;
    std::size_t bytes() const noexcept;
    void detach() noexcept;

   private:
    friend ExternalSlot attach_external(std::string_view name);
    ExternalSlot(ExternalEntry* entry, Status status) noexcept
        : entry_(entry), status_(status) {}

    ExternalEntry* entry_ = nullptr;
    Status status_ = Status::Ok;
};

/**
 * @brief Open an external slot. The name is cut at 47 bytes.
 *
 * The slot is empty with Status::SlotTableFull when all EXTERNAL_SLOTS are
 * taken.
 */
ExternalSlot attach_external(std::string_view name);

/**
 * @brief A node of the pool tree: the process, a query, a task, an operator.
 *
 * A pool is a slot in a table that is never unmapped, so a Pool reference
 * stays readable after the pool closes. Blocks are charged to the pool that
 * was the scope when they were allocated and credited to it when they are
 * freed, on any thread. The counters read below are exact while no thread
 * allocates or frees; a concurrent read is a snapshot.
 */
class Pool {
   public:
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    /**
     * @brief Open a pool below this one.
     *
     * The name is cut at 47 bytes. The handle is empty with
     * Status::PoolTableFull when all 65,535 slots are taken; a slot is never
     * reused while a block or a reservation still names it.
     */
    PoolHandle child(std::string_view name, PoolKind kind = PoolKind::Scratch,
                     Limits limits = {});

    /**
     * @brief Reserve @p bytes in this pool.
     *
     * Reservations reach the parent in quanta of 1 MiB below 16 MiB, 4 MiB
     * below 64 MiB and 8 MiB above, by the pool's total, so the root counts
     * the rounded amount. The root total plus the external bytes must stay
     * within the budget, and the pool total within the limit of this pool and
     * of each ancestor.
     *
     * A request larger than the budget or a limit fails with
     * Status::OverBudget and asks nobody for bytes. When a request fits in
     * neither and holders are attached, the call posts requests to their
     * slots, trims idle heap memory and tries once more. It returns
     * Status::Pending when it posted anything and the retry still failed, and
     * Status::OverBudget when no slot had bytes to give. It never calls a
     * holder and never waits.
     *
     * While a ReclaimGuard of this pool lives, the budget check allows the
     * spill headroom on top of the budget and the call posts nothing.
     */
    Reservation reserve(std::size_t bytes) noexcept;

    /**
     * @brief Reserve as reserve() does, but on a refusal ask no holder for
     * bytes and trim nothing.
     *
     * For a holder that answers a refusal itself, such as by writing what it
     * holds to disk: a refusal costs only the failed check.
     */
    Reservation try_reserve(std::size_t bytes) noexcept;

    /**
     * @brief Raise the limit of this pool by @p bytes. A pool with no limit
     * keeps none.
     *
     * Thread safe. A reservation checked against the old limit at the same
     * time may still be refused.
     */
    void raise_limit(std::size_t bytes) noexcept;

    /**
     * @brief Register a holder of this pool that can give bytes back.
     *
     * Reads Reclaimer::reclaimable() once, on the calling thread, as the first
     * value of the slot. The manager keeps no pointer to @p reclaimer. The
     * slot is empty with Status::SlotTableFull when all RECLAIMER_SLOTS are
     * taken.
     */
    ReclaimSlot attach(Reclaimer& reclaimer);

    std::uint16_t id() const noexcept { return id_; }
    /// Changes each time the slot opens a new pool.
    std::uint32_t generation() const noexcept {
        return generation_.load(std::memory_order_relaxed);
    }
    std::uint16_t parent() const noexcept { return parent_; }
    PoolKind kind() const noexcept { return kind_; }
    std::string_view name() const noexcept { return {name_, name_size_}; }
    /// Names from the root joined by '/', for example "root/query/scan".
    std::string path() const;
    std::size_t limit() const noexcept {
        return limit_.load(std::memory_order_relaxed);
    }

    /// Bytes and blocks held by this pool and every pool below it.
    std::size_t live() const;
    std::size_t blocks() const;
    /// Largest live() seen by a counter read; the heap does not sample it.
    std::size_t peak() const;
    /// Reserved bytes, with every pool below counted by its rounded amount.
    std::size_t reserved() const noexcept;
    /// Live bytes of closed Results pools directly below this one.
    std::size_t adopted() const;
    std::size_t exported() const noexcept;
    /// Mark bytes of this pool as handed to a host buffer. They stay live.
    void add_exported(std::size_t bytes) noexcept;
    void sub_exported(std::size_t bytes) noexcept;

   private:
    friend struct PoolAccess;
    friend struct ManagerAccess;
    friend class PoolHandle;
    friend class Reservation;
    friend class ReclaimGuard;
    friend class ReclaimSlot;
    Pool() = default;

    static constexpr std::size_t NAME_CAPACITY = 48;

    std::atomic<std::ptrdiff_t> live_{0};
    std::atomic<std::ptrdiff_t> blocks_{0};
    std::atomic<std::ptrdiff_t> reserved_{0};
    std::atomic<std::ptrdiff_t> charged_{0};
    std::atomic<std::ptrdiff_t> peak_{0};
    std::atomic<std::ptrdiff_t> exported_{0};
    std::atomic<std::size_t> limit_{0};
    std::atomic<std::uint32_t> generation_{0};
    std::atomic<std::uint32_t> state_{0};
    std::atomic<std::uint32_t> children_{0};
    std::atomic<std::uint32_t> open_children_{0};
    std::atomic<std::uint32_t> reclaiming_{0};
    std::atomic<std::uint16_t> next_free_{0};
    std::uint16_t id_ = 0;
    std::atomic<std::uint16_t> parent_{0};
    PoolKind kind_ = PoolKind::Scratch;
    std::uint8_t name_size_ = 0;
    char name_[NAME_CAPACITY] = {};
};

struct PoolStats {
    std::uint16_t id;
    std::uint16_t parent;
    PoolKind kind;
    std::string path;
    std::size_t live;
    std::size_t blocks;
    std::size_t reserved;
    std::size_t peak;
    std::size_t adopted;
    std::size_t exported;
};

struct Stats {
    /// Bytes and blocks held by every pool, the root's subtree.
    std::size_t live;
    std::size_t blocks;
    /// Largest root live() seen by a counter read.
    std::size_t peak;
    /// Bytes the heap holds from the operating system. With the system
    /// backend the C library does not report it and this is live.
    std::size_t reserved;
    /// The root's reservations, counted in quanta.
    std::size_t pool_reserved;
    std::size_t budget;
    /// Sum of the external slots, counted and reported bytes.
    std::size_t external;
    /// Anonymous and shared memory of the process (resident footprint on
    /// macOS), read when stats() runs.
    std::size_t measured;
    /// measured minus reserved and counted external, zero when that is
    /// negative: memory of Python, pandas, pyarrow and the C library.
    std::size_t unattributed;
    /// The process limit: the cgroup limit, else the available memory at the
    /// first read.
    std::size_t limit;
};

struct ExternalStats {
    std::string name;
    std::size_t bytes;
};

/**
 * @brief The one memory manager of the process.
 *
 * Created at the first use and never destroyed, so blocks freed after static
 * destructors have run still find their pool. Its state is a mapping that is
 * never unmapped. The calls below take no lock and never wait.
 */
class Manager {
   public:
    static Manager& get() noexcept;

    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    /// Id 0. Blocks allocated with no scope go to its child "general", id 1.
    Pool& root() noexcept;
    Pool& general() noexcept;

    /// The sum of reservations the root accepts. Unless set, a third of the
    /// available memory as resolve_spill_budget(0) computes it at first use.
    std::size_t budget() noexcept;
    /// Zero returns to the automatic value.
    void set_budget(std::size_t bytes) noexcept;

    /**
     * @brief Read every counter and the process memory now.
     *
     * Unlike under_pressure(), it does not wait for the next sample.
     */
    Stats stats();
    void trim(Mode mode = Mode::All) noexcept;

    /**
     * @brief True when holders should give bytes back.
     *
     * Either max(heap reserved, root reservations) plus counted external
     * bytes pass
     * the budget, or the measured memory passes the limit minus the limit
     * headroom. Memory that is neither ours nor external never trips the
     * budget. The budget term follows a reservation or an external change at
     * the next call; the heap and process terms are read at most every
     * constants::PRESSURE_SAMPLE_MS. Takes no lock, never waits.
     */
    bool under_pressure() noexcept;

    /// The values configure() sets, resolved: see Config.
    std::size_t limit_headroom() noexcept;
    std::size_t spill_headroom() noexcept;

    /// True once a Scratch pool closed with live bytes or open children.
    bool leaked() const noexcept {
        return leaked_.load(std::memory_order_relaxed);
    }

   private:
    friend struct ManagerAccess;
    friend struct PoolAccess;
    Manager() = default;

    Pool* table_ = nullptr;
    ReclaimEntry* reclaimers_ = nullptr;
    ExternalEntry* externals_ = nullptr;
    std::atomic<std::uint32_t> fresh_{0};
    std::atomic<std::uint64_t> free_head_{0};
    std::atomic<std::size_t> budget_{0};
    std::atomic<std::size_t> limit_headroom_{0};
    std::atomic<std::size_t> spill_headroom_{0};
    std::atomic<std::size_t> limit_{0};
    std::atomic<std::size_t> measured_{0};
    std::atomic<std::uint64_t> next_sample_ms_{0};
    std::atomic<std::uint64_t> wait_arrivals_{0};
    std::atomic<ReserveWait*> wait_head_{nullptr};
    ReserveWait* wait_held_ = nullptr;
    std::atomic<std::uint32_t> waiters_{0};
    std::atomic<bool> granting_{false};
    std::atomic<bool> again_{false};
    std::atomic<bool> dirty_{true};
    std::atomic<bool> pressure_{false};
    std::atomic<bool> leaked_{false};
};

/**
 * @brief Charge the blocks this thread allocates to a pool until destruction.
 *
 * Nests and restores the previous pool and the task pointer (see TaskTag).
 * It sets the pool of the calling thread, so a task that resumes on another
 * thread opens its own Scope there. The one-argument form leaves the task
 * pointer as it is; the two-argument form sets it for the lifetime of the
 * Scope. The pool must stay open while any Scope names it.
 */
class Scope {
   public:
    explicit Scope(Pool& pool) noexcept;
    Scope(Pool& pool, void* user) noexcept;
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope();

   private:
    AttribCache* cache_;
    void* previous_user_;
    std::uint16_t previous_;
};

/// The pool a Scope set on the calling thread, else the general pool.
Pool& current_pool();

/**
 * @brief A pool named by its slot and generation.
 *
 * Unlike a Pool reference it may outlive the pool: enter_pool() then picks
 * the general pool. A value-initialized tag names no pool.
 */
struct PoolTag {
    std::uint32_t generation = 0;
    std::uint16_t id = 0;
};

/// The tag of current_pool().
PoolTag current_pool_tag() noexcept;

/**
 * @brief The pool and the opaque pointer a task runs with.
 *
 * The pointer is the calling thread's task pointer: this library never reads
 * it, a higher layer sets it with set_task_user() and a Scope clears it.
 */
struct TaskTag {
    PoolTag pool;
    void* user = nullptr;
};

/// current_pool_tag() and the task pointer of the calling thread.
TaskTag current_task() noexcept;

/**
 * @brief Make the tagged pool and task pointer the calling thread's, as
 * enter_pool() does for the pool, without restoring them.
 *
 * Returns early when both already match.
 */
void enter_task(TaskTag tag) noexcept;

/// The task pointer of the calling thread, null if none was set.
void* task_user() noexcept;

/// Set the task pointer of the calling thread.
void set_task_user(void* user) noexcept;

/**
 * @brief Make the tagged pool the calling thread's scope without restoring
 * it, or the general pool when that pool has closed.
 *
 * For a coroutine that enters its own pool each time it resumes; whoever
 * resumed it restores its own scope with a Scope. Takes no lock and does no
 * atomic read-modify-write.
 */
void enter_pool(PoolTag tag) noexcept;

/// Every open pool, root first.
std::vector<PoolStats> pool_stats();

/// Every attached external slot.
std::vector<ExternalStats> external_stats();

/// Receives each leak report as one line that ends with a newline.
using Reporter = void (*)(std::string_view text) noexcept;

/**
 * @brief Replace the leak reporter and return the previous one.
 *
 * The default writes to standard error with write(2). Null restores it. The
 * reporter runs on the thread that closes the pool and must not allocate
 * through this library.
 */
Reporter set_reporter(Reporter reporter) noexcept;

/**
 * @brief Turn the exit check on or off.
 *
 * Starts as constants::EXIT_CHECK_ENV set it. When on, the process exit
 * prints the leak report to the reporter and calls _Exit with
 * constants::EXIT_CHECK_STATUS if live bytes minus the exported bytes of every
 * pool are not zero or a Scratch pool leaked. A host that may keep blocks at
 * exit, such as the Python extension, turns it off.
 */
void set_exit_check(bool enabled) noexcept;

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_MANAGER_H
