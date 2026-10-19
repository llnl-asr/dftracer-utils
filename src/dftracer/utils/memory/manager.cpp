#include <dftracer/utils/memory/attrib.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/memory/debug.h>
#include <dftracer/utils/memory/heap_page.h>
#include <dftracer/utils/memory/heap_system.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/memory/pressure.h>
#include <dftracer/utils/memory/settings.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace dftracer::utils::memory {

namespace {

constexpr std::uint32_t STATE_FREE = 0;
constexpr std::uint32_t STATE_OPEN = 1;
constexpr std::uint32_t STATE_CLOSED = 2;
constexpr std::uint32_t STATE_BUSY = 3;

constexpr std::ptrdiff_t MIB = std::ptrdiff_t{1} << 20;
constexpr std::size_t PATH_DEPTH = 64;
constexpr std::size_t REPORT_BYTES = 1024;
constexpr std::size_t TABLE_ALIGN = std::size_t{16} << 10;
constexpr std::size_t MAX_POSTS = 64;
constexpr std::size_t NAME_MAX_BYTES = 47;
// The arrival of no waiter; the counter never reaches it.
constexpr std::uint64_t NO_WAITER = ~std::uint64_t{0};

constexpr std::uint32_t ENTRY_FREE = 0;
constexpr std::uint32_t ENTRY_BUSY = 1;
constexpr std::uint32_t ENTRY_ATTACHED = 2;

struct Admit {
    Status status;
    std::size_t shortfall;
};

std::atomic<Manager*> g_manager{nullptr};
std::atomic<AttribCache*> g_attrib_caches{nullptr};
std::atomic<Reporter> g_reporter{nullptr};
std::atomic<bool> g_exit_check{false};
std::atomic<bool> g_exit_hook{false};

void write_report(std::string_view text) noexcept {
    const char* data = text.data();
    std::size_t left = text.size();
    while (left > 0) {
        ssize_t n = ::write(STDERR_FILENO, data, left);
        if (n <= 0) break;
        data += n;
        left -= static_cast<std::size_t>(n);
    }
}

void report(const char* text, std::size_t size) noexcept {
    Reporter reporter = g_reporter.load(std::memory_order_acquire);
    if (reporter == nullptr) {
        write_report({text, size});
    } else {
        reporter({text, size});
    }
}

constexpr std::ptrdiff_t quantize(std::ptrdiff_t total) noexcept {
    if (total <= 0) return 0;
    std::ptrdiff_t q = total < 16 * MIB   ? MIB
                       : total < 64 * MIB ? 4 * MIB
                                          : 8 * MIB;
    return (total + q - 1) / q * q;
}

void raise(std::atomic<std::ptrdiff_t>& peak, std::ptrdiff_t value) noexcept {
    std::ptrdiff_t seen = peak.load(std::memory_order_relaxed);
    while (seen < value && !peak.compare_exchange_weak(
                               seen, value, std::memory_order_relaxed)) {
    }
}

constexpr std::size_t clamp_size(std::ptrdiff_t n) noexcept {
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

template <class Fn>
void for_each_entry(AttribCache& cache, Fn&& fn) noexcept {
    fn(cache.hot);
    for (AttribEntry& e : cache.entries) fn(e);
}

}  // namespace

// A zero-filled mapping is the initial state of every entry, so none is
// constructed. The reclaimable and requested counts are the only fields the
// arbiter touches.
struct alignas(64) ReclaimEntry {
    std::atomic<std::uint32_t> state;
    std::atomic<std::uint32_t> generation;
    std::atomic<std::uint16_t> pool_id;
    std::atomic<std::size_t> reclaimable;
    std::atomic<std::size_t> requested;
};

struct alignas(64) ExternalEntry {
    std::atomic<std::uint32_t> state;
    std::atomic<std::size_t> counted;
    std::atomic<std::size_t> reported;
    std::uint8_t name_size;
    char name[NAME_MAX_BYTES + 1];
};

struct ManagerAccess {
    static Pool& slot(Manager& m, std::size_t id) noexcept {
        return m.table_[id];
    }
    // The mapping is zero-filled, which is the initial state of every field.
    // A constructor would write the atomics while a counter read walks them.
    static Pool* init_slot(Manager& m, std::size_t id) noexcept {
        Pool* p = &m.table_[id];
        p->id_ = static_cast<std::uint16_t>(id);
        return p;
    }
    static std::size_t slot_count(const Manager& m) noexcept {
        return m.fresh_.load(std::memory_order_acquire);
    }
    static void mark_dirty(Manager& m) noexcept {
        if (!m.dirty_.load(std::memory_order_relaxed)) {
            m.dirty_.store(true, std::memory_order_relaxed);
        }
    }
    static const ExternalEntry& external_at(const Manager& m,
                                            std::size_t i) noexcept {
        return m.externals_[i];
    }
    static std::size_t counted_total(const Manager& m) noexcept;
    static std::size_t reported_total(const Manager& m) noexcept;
    static std::size_t limit(Manager& m) noexcept;
    static void refresh(Manager& m, bool read_process) noexcept;
    static void apply(Manager& m, const Config& config) noexcept;
    static std::uint16_t top_of(Manager& m, std::uint16_t id) noexcept;
    static std::size_t post_requests(Manager& m, const Pool& caller,
                                     std::size_t shortfall) noexcept;
    static ReclaimEntry* claim_reclaimer(Manager& m) noexcept;
    static ExternalEntry* claim_external(Manager& m) noexcept;
    static std::uint16_t pop_free(Manager& m) noexcept;
    static void push_free(Manager& m, Pool& p) noexcept;
    static Pool* claim(Manager& m) noexcept;
    static Pool* reap(Manager& m) noexcept;
    static Manager* create() noexcept;
    static void after_fork_child() noexcept;
    static std::ptrdiff_t exported_total(Manager& m) noexcept;
    static void exit_check() noexcept;
    static void report_sites(Manager& m, std::uint16_t pool) noexcept;
    static bool push_wait(Manager& m, ReserveWait& w) noexcept;
    static void wake(Manager& m) noexcept;
    static bool grant(Manager& m, std::uint64_t self) noexcept;
    static void grant_pass(Manager& m, std::uint64_t self,
                           bool& self_granted) noexcept;
    static ReserveWait* merge_arrivals(ReserveWait* a, ReserveWait* b) noexcept;
    static ReserveWait* sort_arrivals(ReserveWait* list) noexcept;
};

struct PoolAccess {
    struct Snapshot {
        std::vector<std::ptrdiff_t> own_bytes;
        std::vector<std::ptrdiff_t> own_blocks;
        std::vector<std::ptrdiff_t> sub_bytes;
        std::vector<std::ptrdiff_t> sub_blocks;
    };

    static Snapshot snapshot(Manager& m);
    static void credit(Pool& p, std::ptrdiff_t bytes,
                       std::ptrdiff_t blocks) noexcept {
        p.live_.fetch_add(bytes, std::memory_order_relaxed);
        p.blocks_.fetch_add(blocks, std::memory_order_relaxed);
    }
    static std::ptrdiff_t live_of(const Pool& p) noexcept {
        return p.live_.load(std::memory_order_relaxed);
    }
    static bool is_open(const Pool& p) noexcept {
        return p.state_.load(std::memory_order_acquire) == STATE_OPEN;
    }
    static std::size_t peak_of(const Pool& p) noexcept {
        return clamp_size(p.peak_.load(std::memory_order_relaxed));
    }
    static std::vector<std::size_t> adopted_by(Manager& m, const Snapshot& s);
    static bool try_add(Manager& m, Pool& p, std::ptrdiff_t n,
                        std::size_t slack, std::size_t& shortfall) noexcept;
    static Status settle(Manager& m, Pool& p, std::size_t slack,
                         std::size_t& shortfall) noexcept;
    static bool never_fits(Manager& m, const Pool& p,
                           std::size_t bytes) noexcept;
    static std::size_t slack_of(Manager& m, const Pool& p) noexcept {
        return p.reclaiming_.load(std::memory_order_relaxed) != 0
                   ? m.spill_headroom()
                   : 0;
    }
    static Admit add_reserved(Manager& m, Pool& p, std::size_t bytes,
                              std::size_t slack) noexcept;
    static void uncharge(Manager& m, Pool& p, std::size_t bytes) noexcept;
    static void sub_reserved(Pool& p, std::size_t bytes) noexcept;
    static void own_totals(Manager& m, std::uint16_t id, std::ptrdiff_t& bytes,
                           std::ptrdiff_t& blocks) noexcept;
    static bool reapable(Manager& m, Pool& p) noexcept;
    static bool take_closed(Manager& m, Pool& p) noexcept;
    static void retire(Manager& m, Pool& p) noexcept;
    static void close(Pool& p, std::uint32_t generation) noexcept;
    static std::size_t path(Manager& m, std::uint16_t id, char* out,
                            std::size_t capacity) noexcept;
};

// Every slot below the high-water mark: its flushed counters plus what the
// threads have not flushed yet, then each total added to its ancestors.
PoolAccess::Snapshot PoolAccess::snapshot(Manager& m) {
    std::size_t n = ManagerAccess::slot_count(m);
    Snapshot s;
    s.own_bytes.assign(n, 0);
    s.own_blocks.assign(n, 0);
    s.sub_bytes.assign(n, 0);
    s.sub_blocks.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        Pool& p = ManagerAccess::slot(m, i);
        s.own_bytes[i] = p.live_.load(std::memory_order_relaxed);
        s.own_blocks[i] = p.blocks_.load(std::memory_order_relaxed);
    }
    for (AttribCache* c = g_attrib_caches.load(std::memory_order_acquire);
         c != nullptr; c = c->next.load(std::memory_order_relaxed)) {
        for_each_entry(*c, [&](AttribEntry& e) {
            std::uint32_t id = e.id.load(std::memory_order_relaxed);
            if (id >= n) return;
            std::int64_t word = e.word.load(std::memory_order_relaxed);
            s.own_bytes[id] += attrib_bytes(word);
            s.own_blocks[id] += attrib_blocks(word);
        });
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (s.own_bytes[i] == 0 && s.own_blocks[i] == 0) continue;
        std::size_t a = i;
        for (std::size_t depth = 0; depth <= n; ++depth) {
            s.sub_bytes[a] += s.own_bytes[i];
            s.sub_blocks[a] += s.own_blocks[i];
            if (a == ROOT_POOL_ID) break;
            a = ManagerAccess::slot(m, a).parent_;
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (s.sub_bytes[i] > 0) {
            raise(ManagerAccess::slot(m, i).peak_, s.sub_bytes[i]);
        }
    }
    return s;
}

std::uint16_t ManagerAccess::pop_free(Manager& m) noexcept {
    std::uint64_t old = m.free_head_.load(std::memory_order_acquire);
    while (true) {
        auto id = static_cast<std::uint16_t>(old & 0xffff);
        if (id == 0) return 0;
        std::uint16_t next =
            m.table_[id].next_free_.load(std::memory_order_relaxed);
        std::uint64_t tag = (old >> 16) + 1;
        if (m.free_head_.compare_exchange_weak(old, (tag << 16) | next,
                                               std::memory_order_acquire,
                                               std::memory_order_acquire)) {
            return id;
        }
    }
}

void ManagerAccess::push_free(Manager& m, Pool& p) noexcept {
    std::uint64_t old = m.free_head_.load(std::memory_order_relaxed);
    do {
        p.next_free_.store(static_cast<std::uint16_t>(old & 0xffff),
                           std::memory_order_relaxed);
    } while (!m.free_head_.compare_exchange_weak(
        old, (((old >> 16) + 1) << 16) | p.id_, std::memory_order_release,
        std::memory_order_relaxed));
}

Pool* ManagerAccess::reap(Manager& m) noexcept {
    std::size_t n = slot_count(m);
    for (std::size_t i = GENERAL_POOL_ID + 1; i < n; ++i) {
        Pool& p = m.table_[i];
        if (p.state_.load(std::memory_order_relaxed) != STATE_CLOSED) continue;
        if (PoolAccess::take_closed(m, p)) return &p;
    }
    return nullptr;
}

Pool* ManagerAccess::claim(Manager& m) noexcept {
    if (std::uint16_t id = pop_free(m); id != 0) return &m.table_[id];
    std::uint32_t n = m.fresh_.load(std::memory_order_relaxed);
    while (n < POOL_SLOTS) {
        if (m.fresh_.compare_exchange_weak(n, n + 1,
                                           std::memory_order_acq_rel)) {
            return init_slot(m, n);
        }
    }
    return reap(m);
}

void ManagerAccess::after_fork_child() noexcept { system_attrib_after_fork(); }

std::ptrdiff_t ManagerAccess::exported_total(Manager& m) noexcept {
    std::ptrdiff_t total = 0;
    std::size_t n = slot_count(m);
    for (std::size_t i = 0; i < n; ++i) {
        total += m.table_[i].exported_.load(std::memory_order_relaxed);
    }
    return total;
}

void ManagerAccess::report_sites(Manager& m, std::uint16_t pool) noexcept {
    struct Context {
        Manager* manager;
    } context{&m};
    debug_for_each_site(
        pool,
        [](void* raw, const DebugSite& group) noexcept {
            Manager& manager = *static_cast<Context*>(raw)->manager;
            char text[REPORT_BYTES];
            char where[REPORT_BYTES / 2];
            int n;
            if (group.site == 0) {
                n = std::snprintf(text, sizeof(text),
                                  "dftracer-utils memory: leak: other sites "
                                  "hold %zu bytes in %zu blocks\n",
                                  group.bytes, group.blocks);
            } else {
                std::size_t size = PoolAccess::path(manager, group.pool, where,
                                                    sizeof(where) - 1);
                where[size] = '\0';
                n = std::snprintf(text, sizeof(text),
                                  "dftracer-utils memory: leak: pool %s site "
                                  "0x%llx holds %zu bytes in %zu blocks\n",
                                  where,
                                  static_cast<unsigned long long>(group.site),
                                  group.bytes, group.blocks);
            }
            report(text, std::min<std::size_t>(static_cast<std::size_t>(n),
                                               sizeof(text) - 1));
        },
        &context);
}

// Fails the process when bytes are still live that no host buffer owns, or a
// scratch pool leaked. Debug mode adds the callers of the live blocks.
void ManagerAccess::exit_check() noexcept {
    if (!g_exit_check.load(std::memory_order_relaxed)) return;
    Manager& m = Manager::get();
    debug_drain();
    std::ptrdiff_t live = attrib_live_total() - exported_total(m);
    bool leaked = m.leaked_.load(std::memory_order_relaxed);
    if (live == 0 && !leaked) return;
    char text[REPORT_BYTES];
    int n = std::snprintf(text, sizeof(text),
                          "dftracer-utils memory: exit check failed: %lld "
                          "live bytes not exported, pool leak flag %d\n",
                          static_cast<long long>(live), leaked ? 1 : 0);
    report(text, std::min<std::size_t>(static_cast<std::size_t>(n),
                                       sizeof(text) - 1));
    std::size_t slots = slot_count(m);
    for (std::size_t i = 0; i < slots; ++i) {
        std::ptrdiff_t bytes = 0;
        std::ptrdiff_t blocks = 0;
        PoolAccess::own_totals(m, static_cast<std::uint16_t>(i), bytes, blocks);
        if (bytes == 0 && blocks == 0) continue;
        char where[REPORT_BYTES / 2];
        std::size_t size = PoolAccess::path(m, static_cast<std::uint16_t>(i),
                                            where, sizeof(where) - 1);
        where[size] = '\0';
        n = std::snprintf(text, sizeof(text),
                          "dftracer-utils memory: leak: pool %s holds %lld "
                          "bytes in %lld blocks\n",
                          where, static_cast<long long>(bytes),
                          static_cast<long long>(blocks));
        report(text, std::min<std::size_t>(static_cast<std::size_t>(n),
                                           sizeof(text) - 1));
    }
    if (debug_enabled()) report_sites(m, DEBUG_ALL_POOLS);
    ::_Exit(constants::EXIT_CHECK_STATUS);
}

void set_exit_check(bool enabled) noexcept {
    g_exit_check.store(enabled, std::memory_order_relaxed);
    if (enabled && !g_exit_hook.exchange(true, std::memory_order_relaxed)) {
        if (std::atexit([] { ManagerAccess::exit_check(); }) != 0) {
            abort_with("dftracer-utils memory: cannot register exit hook\n");
        }
    }
}

Manager* ManagerAccess::create() noexcept {
    constexpr std::size_t entry_align = alignof(ReclaimEntry);
    std::size_t header =
        (sizeof(Manager) + TABLE_ALIGN - 1) & ~(TABLE_ALIGN - 1);
    std::size_t pools =
        (POOL_SLOTS * sizeof(Pool) + entry_align - 1) & ~(entry_align - 1);
    std::size_t bytes = header + pools +
                        RECLAIMER_SLOTS * sizeof(ReclaimEntry) +
                        EXTERNAL_SLOTS * sizeof(ExternalEntry);
    void* raw = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    if (raw == MAP_FAILED) {
        abort_with("dftracer-utils memory: cannot map the pool table\n");
    }
    auto* m = new (raw) Manager;
    m->table_ = reinterpret_cast<Pool*>(static_cast<char*>(raw) + header);
    m->reclaimers_ = reinterpret_cast<ReclaimEntry*>(static_cast<char*>(raw) +
                                                     header + pools);
    m->externals_ = reinterpret_cast<ExternalEntry*>(
        reinterpret_cast<char*>(m->reclaimers_) +
        RECLAIMER_SLOTS * sizeof(ReclaimEntry));
    Pool* root = init_slot(*m, ROOT_POOL_ID);
    std::memcpy(root->name_, "root", 4);
    root->name_size_ = 4;
    root->generation_.store(1, std::memory_order_relaxed);
    root->state_.store(STATE_OPEN, std::memory_order_relaxed);
    Pool* general = init_slot(*m, GENERAL_POOL_ID);
    std::memcpy(general->name_, "general", 7);
    general->name_size_ = 7;
    general->parent_ = ROOT_POOL_ID;
    general->generation_.store(1, std::memory_order_relaxed);
    general->state_.store(STATE_OPEN, std::memory_order_relaxed);
    root->children_.store(1, std::memory_order_relaxed);
    root->open_children_.store(1, std::memory_order_relaxed);
    m->fresh_.store(GENERAL_POOL_ID + 1, std::memory_order_relaxed);

    Manager* expected = nullptr;
    if (!g_manager.compare_exchange_strong(expected, m,
                                           std::memory_order_acq_rel)) {
        ::munmap(raw, bytes);
        return expected;
    }
    if (::pthread_atfork(nullptr, nullptr, after_fork_child) != 0) {
        abort_with("dftracer-utils memory: cannot register fork hook\n");
    }
    if (exit_check_env()) set_exit_check(true);
    return m;
}

void attrib_register(AttribCache& cache) noexcept {
    AttribCache* head = g_attrib_caches.load(std::memory_order_relaxed);
    do {
        cache.next.store(head, std::memory_order_relaxed);
    } while (!g_attrib_caches.compare_exchange_weak(
        head, &cache, std::memory_order_release, std::memory_order_relaxed));
}

void attrib_flush(AttribEntry& entry) noexcept {
    std::int64_t word = entry.word.load(std::memory_order_relaxed);
    if (word == 0) return;
    Pool& p = ManagerAccess::slot(Manager::get(),
                                  entry.id.load(std::memory_order_relaxed));
    PoolAccess::credit(p, attrib_bytes(word), attrib_blocks(word));
    entry.word.store(0, std::memory_order_relaxed);
}

void attrib_direct(std::uint16_t id, std::ptrdiff_t bytes,
                   std::ptrdiff_t blocks) noexcept {
    PoolAccess::credit(ManagerAccess::slot(Manager::get(), id), bytes, blocks);
}

void attrib_switch(AttribEntry& entry, std::uint16_t id) noexcept {
    attrib_flush(entry);
    entry.id.store(id, std::memory_order_relaxed);
}

// The entry that names `id`, else the first empty one from its home slot, so
// two pools whose ids share a slot (a task pool and general) do not evict each
// other on every resume. The home slot when every entry holds counts.
static AttribEntry& attrib_park_entry(AttribCache& cache,
                                      std::uint16_t id) noexcept {
    AttribEntry* empty = nullptr;
    for (std::size_t i = 0; i < ATTRIB_ENTRIES; ++i) {
        AttribEntry& e = cache.entries[(id + i) % ATTRIB_ENTRIES];
        if (e.id.load(std::memory_order_relaxed) == id) return e;
        if (!empty && e.word.load(std::memory_order_relaxed) == 0) empty = &e;
    }
    return empty ? *empty : cache.entries[id % ATTRIB_ENTRIES];
}

// Parks the hot counts in the leaving pool's entry rather than flushing, so a
// switch on the resume path does no atomic read-modify-write. A concurrent
// read may see them twice or not at all, as with a flush.
void attrib_set_scope(AttribCache& cache, std::uint16_t id) noexcept {
    AttribEntry& hot = cache.hot;
    std::int64_t word = hot.word.load(std::memory_order_relaxed);
    if (word != 0) {
        auto leaving =
            static_cast<std::uint16_t>(hot.id.load(std::memory_order_relaxed));
        AttribEntry& park = attrib_park_entry(cache, leaving);
        if (park.id.load(std::memory_order_relaxed) != leaving)
            attrib_switch(park, leaving);
        std::int64_t parked = park.word.load(std::memory_order_relaxed) + word;
        park.word.store(parked, std::memory_order_relaxed);
        hot.word.store(0, std::memory_order_relaxed);
        std::int64_t bytes = attrib_bytes(parked);
        if (bytes >= ATTRIB_FLUSH_BYTES || bytes <= -ATTRIB_FLUSH_BYTES)
            attrib_flush(park);
    }
    hot.id.store(id, std::memory_order_relaxed);
    cache.scope = id;
}

std::ptrdiff_t attrib_live_total() noexcept {
    Manager& m = Manager::get();
    std::ptrdiff_t total = 0;
    std::size_t n = ManagerAccess::slot_count(m);
    for (std::size_t i = 0; i < n; ++i) {
        total += PoolAccess::live_of(ManagerAccess::slot(m, i));
    }
    for (AttribCache* c = g_attrib_caches.load(std::memory_order_acquire);
         c != nullptr; c = c->next.load(std::memory_order_relaxed)) {
        for_each_entry(*c, [&](AttribEntry& e) {
            total += attrib_bytes(e.word.load(std::memory_order_relaxed));
        });
    }
    return total;
}

Manager& Manager::get() noexcept {
    Manager* m = g_manager.load(std::memory_order_acquire);
    if (m == nullptr) [[unlikely]]
        m = ManagerAccess::create();
    return *m;
}

Pool& Manager::root() noexcept { return table_[ROOT_POOL_ID]; }

Pool& Manager::general() noexcept { return table_[GENERAL_POOL_ID]; }

std::size_t Manager::budget() noexcept {
    std::size_t b = budget_.load(std::memory_order_relaxed);
    if (b != 0) return b;
    std::uint64_t resolved = resolve_spill_budget(0);
    b = resolved >= std::numeric_limits<std::size_t>::max()
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(resolved);
    std::size_t expected = 0;
    budget_.compare_exchange_strong(expected, b, std::memory_order_relaxed);
    b = budget_.load(std::memory_order_relaxed);
    set_map_budget(b);
    return b;
}

void Manager::set_budget(std::size_t bytes) noexcept {
    budget_.store(bytes, std::memory_order_relaxed);
    set_map_budget(bytes != 0 ? bytes : budget());
    ManagerAccess::mark_dirty(*this);
    ManagerAccess::wake(*this);
}

std::size_t Manager::limit_headroom() noexcept {
    std::size_t configured = limit_headroom_.load(std::memory_order_relaxed);
    if (configured != 0) return configured;
    std::uint64_t share =
        static_cast<std::uint64_t>(ManagerAccess::limit(*this)) / 100 *
        constants::LIMIT_HEADROOM_PERCENT;
    return static_cast<std::size_t>(
        std::max(share, constants::LIMIT_HEADROOM_MIN_BYTES));
}

std::size_t Manager::spill_headroom() noexcept {
    std::size_t configured = spill_headroom_.load(std::memory_order_relaxed);
    if (configured != 0) return configured;
    std::uint64_t share = static_cast<std::uint64_t>(budget()) / 100 *
                          constants::SPILL_HEADROOM_PERCENT;
    return static_cast<std::size_t>(
        std::max(share, constants::SPILL_HEADROOM_MIN_BYTES));
}

std::size_t ManagerAccess::limit(Manager& m) noexcept {
    std::size_t limit = m.limit_.load(std::memory_order_relaxed);
    if (limit != 0) return limit;
    limit = read_process_limit();
    if (limit == 0) limit = detect_available_memory();
    m.limit_.store(limit, std::memory_order_relaxed);
    return limit;
}

std::size_t ManagerAccess::counted_total(const Manager& m) noexcept {
    std::size_t total = 0;
    for (std::size_t i = 0; i < EXTERNAL_SLOTS; ++i) {
        total += m.externals_[i].counted.load(std::memory_order_relaxed);
    }
    return total;
}

std::size_t ManagerAccess::reported_total(const Manager& m) noexcept {
    std::size_t total = 0;
    for (std::size_t i = 0; i < EXTERNAL_SLOTS; ++i) {
        total += m.externals_[i].reported.load(std::memory_order_relaxed);
    }
    return total;
}

namespace {

std::uint64_t now_ms() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000 +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000000;
}

std::size_t heap_reserved() noexcept {
    return active_backend() == Backend::PageHeap
               ? page_reserved()
               : clamp_size(attrib_live_total());
}

}  // namespace

// The dirty flag goes down before the terms are read, so a change that lands
// meanwhile raises it again and the next call reads once more.
void ManagerAccess::refresh(Manager& m, bool read_process) noexcept {
    std::uint64_t now = now_ms();
    if (read_process ||
        now >= m.next_sample_ms_.load(std::memory_order_relaxed)) {
        m.measured_.store(read_process_memory(), std::memory_order_relaxed);
        m.next_sample_ms_.store(now + constants::PRESSURE_SAMPLE_MS,
                                std::memory_order_relaxed);
    }
    m.dirty_.store(false, std::memory_order_relaxed);
    std::size_t ours =
        std::max(heap_reserved(), m.root().reserved()) + counted_total(m);
    bool over_budget = ours > m.budget();
    std::size_t limit = ManagerAccess::limit(m);
    std::size_t headroom = m.limit_headroom();
    bool near_limit =
        limit > headroom &&
        m.measured_.load(std::memory_order_relaxed) > limit - headroom;
    m.pressure_.store(over_budget || near_limit, std::memory_order_relaxed);
}

bool Manager::under_pressure() noexcept {
    if (dirty_.load(std::memory_order_relaxed) ||
        now_ms() >= next_sample_ms_.load(std::memory_order_relaxed)) {
        ManagerAccess::refresh(*this, false);
    }
    return pressure_.load(std::memory_order_relaxed);
}

Stats Manager::stats() {
    PoolAccess::Snapshot s = PoolAccess::snapshot(*this);
    ManagerAccess::refresh(*this, true);
    Stats out{};
    out.live = clamp_size(s.sub_bytes[ROOT_POOL_ID]);
    out.blocks = clamp_size(s.sub_blocks[ROOT_POOL_ID]);
    out.peak = PoolAccess::peak_of(table_[ROOT_POOL_ID]);
    out.reserved =
        active_backend() == Backend::PageHeap ? page_reserved() : out.live;
    out.pool_reserved = table_[ROOT_POOL_ID].reserved();
    out.budget = budget();
    const std::size_t counted = ManagerAccess::counted_total(*this);
    out.external = counted + ManagerAccess::reported_total(*this);
    out.measured = measured_.load(std::memory_order_relaxed);
    std::size_t ours = out.reserved + counted;
    out.unattributed = out.measured > ours ? out.measured - ours : 0;
    out.limit = ManagerAccess::limit(*this);
    return out;
}

void ManagerAccess::apply(Manager& m, const Config& config) noexcept {
    m.set_budget(config.budget);
    set_hard_cap(config.hard_cap);
    m.limit_headroom_.store(config.limit_headroom, std::memory_order_relaxed);
    m.spill_headroom_.store(config.spill_headroom, std::memory_order_relaxed);
    m.limit_.store(0, std::memory_order_relaxed);
    mark_dirty(m);
}

void configure(const Config& config) noexcept {
    ManagerAccess::apply(Manager::get(), config);
}

void Manager::trim(Mode mode) noexcept { dftracer::utils::memory::trim(mode); }

Reporter set_reporter(Reporter reporter) noexcept {
    return g_reporter.exchange(reporter, std::memory_order_acq_rel);
}

Pool& current_pool() {
    return ManagerAccess::slot(Manager::get(), attrib_local().scope);
}

Scope::Scope(Pool& pool) noexcept
    : cache_(&attrib_local()), previous_(cache_->scope) {
    if (pool.id() != previous_) attrib_set_scope(*cache_, pool.id());
}

Scope::~Scope() {
    if (cache_->scope != previous_) attrib_set_scope(*cache_, previous_);
}

PoolTag current_pool_tag() noexcept {
    std::uint16_t id = attrib_local().scope;
    return PoolTag{ManagerAccess::slot(Manager::get(), id).generation(), id};
}

void enter_pool(PoolTag tag) noexcept {
    AttribCache& cache = attrib_local();
    if (cache.scope == tag.id) return;
    std::uint16_t id = GENERAL_POOL_ID;
    if (tag.id < ManagerAccess::slot_count(Manager::get())) {
        Pool& p = ManagerAccess::slot(Manager::get(), tag.id);
        // The acquire of the open state orders the generation read after it.
        if (PoolAccess::is_open(p) && p.generation() == tag.generation)
            id = tag.id;
    }
    if (cache.scope != id) attrib_set_scope(cache, id);
}

bool PoolAccess::try_add(Manager& m, Pool& p, std::ptrdiff_t n,
                         std::size_t slack, std::size_t& shortfall) noexcept {
    bool is_root = p.id_ == ROOT_POOL_ID;
    std::size_t budget = is_root ? m.budget() : 0;
    std::size_t external = is_root ? ManagerAccess::counted_total(m) : 0;
    std::ptrdiff_t cur = p.reserved_.load(std::memory_order_relaxed);
    do {
        auto next = static_cast<std::size_t>(cur + n);
        const std::size_t limit = p.limit_.load(std::memory_order_relaxed);
        if (limit != 0 && next > limit) {
            shortfall = next - limit;
            return false;
        }
        if (is_root) {
            std::size_t total = next + external;
            if (total > budget && total - budget > slack) {
                shortfall = total - budget;
                return false;
            }
        }
    } while (!p.reserved_.compare_exchange_weak(cur, cur + n,
                                                std::memory_order_relaxed));
    return true;
}

// Bring the amount charged to the parent to the rounded total, one level at a
// time up to the root. Growing can fail at a limit or the budget; it leaves
// every level as it found it. Shrinking cannot fail.
Status PoolAccess::settle(Manager& m, Pool& p, std::size_t slack,
                          std::size_t& shortfall) noexcept {
    if (p.id_ == ROOT_POOL_ID) return Status::Ok;
    std::size_t ignored = 0;
    while (true) {
        std::ptrdiff_t want =
            quantize(p.reserved_.load(std::memory_order_relaxed));
        std::ptrdiff_t cur = p.charged_.load(std::memory_order_relaxed);
        if (want == cur) return Status::Ok;
        if (!p.charged_.compare_exchange_weak(cur, want,
                                              std::memory_order_relaxed)) {
            continue;
        }
        Pool& parent = ManagerAccess::slot(m, p.parent_);
        std::ptrdiff_t delta = want - cur;
        if (delta < 0) {
            parent.reserved_.fetch_add(delta, std::memory_order_relaxed);
            settle(m, parent, 0, ignored);
            return Status::Ok;
        }
        if (!try_add(m, parent, delta, slack, shortfall)) {
            p.charged_.fetch_sub(delta, std::memory_order_relaxed);
            return Status::OverBudget;
        }
        Status status = settle(m, parent, slack, shortfall);
        if (status != Status::Ok) {
            parent.reserved_.fetch_sub(delta, std::memory_order_relaxed);
            settle(m, parent, 0, ignored);
            p.charged_.fetch_sub(delta, std::memory_order_relaxed);
        }
        return status;
    }
}

bool PoolAccess::never_fits(Manager& m, const Pool& p,
                            std::size_t bytes) noexcept {
    if (bytes > m.budget() ||
        bytes > static_cast<std::size_t>(
                    std::numeric_limits<std::ptrdiff_t>::max() / 2)) {
        return true;
    }
    for (const Pool* a = &p; a->id_ != ROOT_POOL_ID;
         a = &ManagerAccess::slot(m, a->parent_)) {
        const std::size_t limit = a->limit_.load(std::memory_order_relaxed);
        if (limit != 0 && bytes > limit) return true;
    }
    return false;
}

// A request above the budget or above a limit on the way to the root can
// never fit, so it carries no shortfall and nobody is asked for bytes.
Admit PoolAccess::add_reserved(Manager& m, Pool& p, std::size_t bytes,
                               std::size_t slack) noexcept {
    if (never_fits(m, p, bytes)) return {Status::OverBudget, 0};
    auto n = static_cast<std::ptrdiff_t>(bytes);
    std::size_t shortfall = 0;
    if (!try_add(m, p, n, slack, shortfall)) {
        return {Status::OverBudget, shortfall};
    }
    Status status = settle(m, p, slack, shortfall);
    if (status != Status::Ok) {
        std::size_t ignored = 0;
        p.reserved_.fetch_sub(n, std::memory_order_relaxed);
        settle(m, p, 0, ignored);
        return {status, shortfall};
    }
    return {Status::Ok, 0};
}

std::vector<std::size_t> PoolAccess::adopted_by(Manager& m, const Snapshot& s) {
    std::vector<std::size_t> adopted(s.sub_bytes.size(), 0);
    for (std::size_t i = GENERAL_POOL_ID + 1; i < s.sub_bytes.size(); ++i) {
        const Pool& c = ManagerAccess::slot(m, i);
        if (c.state_.load(std::memory_order_acquire) == STATE_CLOSED &&
            c.kind_ == PoolKind::Results) {
            adopted[c.parent_] += clamp_size(s.sub_bytes[i]);
        }
    }
    return adopted;
}

void PoolAccess::uncharge(Manager& m, Pool& p, std::size_t bytes) noexcept {
    std::size_t ignored = 0;
    p.reserved_.fetch_sub(static_cast<std::ptrdiff_t>(bytes),
                          std::memory_order_relaxed);
    settle(m, p, 0, ignored);
    ManagerAccess::mark_dirty(m);
}

void PoolAccess::sub_reserved(Pool& p, std::size_t bytes) noexcept {
    Manager& m = Manager::get();
    uncharge(m, p, bytes);
    ManagerAccess::wake(m);
}

void PoolAccess::own_totals(Manager& m, std::uint16_t id, std::ptrdiff_t& bytes,
                            std::ptrdiff_t& blocks) noexcept {
    Pool& p = ManagerAccess::slot(m, id);
    bytes = p.live_.load(std::memory_order_relaxed);
    blocks = p.blocks_.load(std::memory_order_relaxed);
    for (AttribCache* c = g_attrib_caches.load(std::memory_order_acquire);
         c != nullptr; c = c->next.load(std::memory_order_relaxed)) {
        for_each_entry(*c, [&](AttribEntry& e) {
            if (e.id.load(std::memory_order_relaxed) != id) return;
            std::int64_t word = e.word.load(std::memory_order_relaxed);
            bytes += attrib_bytes(word);
            blocks += attrib_blocks(word);
        });
    }
}

// A slot is reused only when no block, reservation, child or export still
// names it. A thread that flushes while the totals are read can show a total
// twice or not at all, so two reads in a row must agree on zero.
bool PoolAccess::reapable(Manager& m, Pool& p) noexcept {
    if (p.children_.load(std::memory_order_acquire) != 0 ||
        p.reserved_.load(std::memory_order_relaxed) != 0 ||
        p.reclaiming_.load(std::memory_order_relaxed) != 0 ||
        p.exported_.load(std::memory_order_relaxed) != 0) {
        return false;
    }
    for (int read = 0; read < 2; ++read) {
        std::ptrdiff_t bytes = 0;
        std::ptrdiff_t blocks = 0;
        own_totals(m, p.id_, bytes, blocks);
        if (bytes != 0 || blocks != 0) return false;
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    return true;
}

bool PoolAccess::take_closed(Manager& m, Pool& p) noexcept {
    if (!reapable(m, p)) return false;
    std::uint32_t expected = STATE_CLOSED;
    if (!p.state_.compare_exchange_strong(expected, STATE_BUSY,
                                          std::memory_order_acq_rel)) {
        return false;
    }
    ManagerAccess::slot(m, p.parent_)
        .children_.fetch_sub(1, std::memory_order_release);
    return true;
}

std::size_t PoolAccess::path(Manager& m, std::uint16_t id, char* out,
                             std::size_t capacity) noexcept {
    std::uint16_t chain[PATH_DEPTH];
    std::size_t depth = 0;
    for (std::uint16_t a = id; depth < PATH_DEPTH;) {
        chain[depth++] = a;
        if (a == ROOT_POOL_ID) break;
        a = ManagerAccess::slot(m, a).parent_;
    }
    std::size_t n = 0;
    for (std::size_t i = depth; i-- > 0;) {
        const Pool& p = ManagerAccess::slot(m, chain[i]);
        if (n > 0 && n < capacity) out[n++] = '/';
        std::size_t take = std::min<std::size_t>(p.name_size_, capacity - n);
        std::memcpy(out + n, p.name_, take);
        n += take;
    }
    return n;
}

void PoolAccess::close(Pool& p, std::uint32_t generation) noexcept {
    if (p.generation_.load(std::memory_order_relaxed) != generation) return;
    std::uint32_t expected = STATE_OPEN;
    if (!p.state_.compare_exchange_strong(expected, STATE_CLOSED,
                                          std::memory_order_acq_rel)) {
        return;
    }
    Manager& m = Manager::get();
    ManagerAccess::slot(m, p.parent_)
        .open_children_.fetch_sub(1, std::memory_order_relaxed);

    debug_drain();
    char text[REPORT_BYTES];
    char where[REPORT_BYTES / 2];
    std::size_t where_size = path(m, p.id_, where, sizeof(where) - 1);
    where[where_size] = '\0';
    std::ptrdiff_t bytes = 0;
    std::ptrdiff_t blocks = 0;
    own_totals(m, p.id_, bytes, blocks);
    if (p.kind_ == PoolKind::Scratch && (bytes != 0 || blocks != 0)) {
        int n = std::snprintf(
            text, sizeof(text),
            "dftracer-utils memory: leak: pool %s closed with %lld live "
            "bytes in %lld blocks\n",
            where, static_cast<long long>(bytes),
            static_cast<long long>(blocks));
        m.leaked_.store(true, std::memory_order_relaxed);
        report(text, std::min<std::size_t>(static_cast<std::size_t>(n),
                                           sizeof(text) - 1));
        if (debug_enabled()) ManagerAccess::report_sites(m, p.id_);
    }
    std::uint32_t open = p.open_children_.load(std::memory_order_relaxed);
    if (open != 0) {
        int n = std::snprintf(text, sizeof(text),
                              "dftracer-utils memory: leak: pool %s closed "
                              "with %u open children\n",
                              where, open);
        m.leaked_.store(true, std::memory_order_relaxed);
        report(text, std::min<std::size_t>(static_cast<std::size_t>(n),
                                           sizeof(text) - 1));
    }
    if (take_closed(m, p)) retire(m, p);
}

// A slot that take_closed claimed goes back to the free stack, and its closed
// parent may have been waiting for it.
void PoolAccess::retire(Manager& m, Pool& p) noexcept {
    Pool& parent = ManagerAccess::slot(m, p.parent_);
    p.state_.store(STATE_FREE, std::memory_order_release);
    ManagerAccess::push_free(m, p);
    if (parent.id_ > GENERAL_POOL_ID &&
        parent.state_.load(std::memory_order_relaxed) == STATE_CLOSED &&
        take_closed(m, parent)) {
        retire(m, parent);
    }
}

Reservation::Reservation(Reservation&& other) noexcept
    : pool_(other.pool_), bytes_(other.bytes_), status_(other.status_) {
    other.pool_ = nullptr;
    other.bytes_ = 0;
}

Reservation& Reservation::operator=(Reservation&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = other.pool_;
        bytes_ = other.bytes_;
        status_ = other.status_;
        other.pool_ = nullptr;
        other.bytes_ = 0;
    }
    return *this;
}

Reservation::~Reservation() { release(); }

Status Reservation::grow(std::size_t bytes) noexcept {
    if (pool_ == nullptr) {
        return status_ == Status::Ok ? Status::OverBudget : status_;
    }
    Reservation extra = pool_->reserve(bytes);
    if (!extra) return extra.status();
    bytes_ += extra.bytes_;
    extra.pool_ = nullptr;
    extra.bytes_ = 0;
    return Status::Ok;
}

Status Reservation::try_grow(std::size_t bytes) noexcept {
    if (pool_ == nullptr) {
        return status_ == Status::Ok ? Status::OverBudget : status_;
    }
    Reservation extra = pool_->try_reserve(bytes);
    if (!extra) return extra.status();
    bytes_ += extra.bytes_;
    extra.pool_ = nullptr;
    extra.bytes_ = 0;
    return Status::Ok;
}

std::size_t Reservation::take_from(Reservation& other,
                                   std::size_t bytes) noexcept {
    const std::size_t moved = std::min(bytes, other.bytes_);
    if (pool_ == nullptr) {
        pool_ = other.pool_;
        status_ = Status::Ok;
    }
    other.bytes_ -= moved;
    bytes_ += moved;
    return moved;
}

void Reservation::release() noexcept {
    if (pool_ != nullptr && bytes_ != 0) {
        PoolAccess::sub_reserved(*pool_, bytes_);
    }
    pool_ = nullptr;
    bytes_ = 0;
}

ReserveWait* ReserveWait::create(Pool& pool, std::size_t bytes, Resume resume,
                                 void* context) noexcept {
    void* raw = nullptr;
    try {
        raw = allocate(sizeof(ReserveWait), alignof(ReserveWait),
                       Manager::get().general());
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    return new (raw) ReserveWait(pool, bytes, resume, context);
}

void ReserveWait::retain() noexcept {
    refs_.fetch_add(1, std::memory_order_relaxed);
}

void ReserveWait::drop() noexcept {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    if (state_.load(std::memory_order_relaxed) == WaitState::Granted &&
        !taken_) {
        PoolAccess::sub_reserved(*pool_, bytes_);
    }
    this->~ReserveWait();
    deallocate(this, sizeof(ReserveWait));
}

Status ReserveWait::push() noexcept {
    Manager& m = Manager::get();
    if (PoolAccess::never_fits(m, *pool_, bytes_)) return Status::OverBudget;
    return ManagerAccess::push_wait(m, *this) ? Status::Ok : Status::Pending;
}

bool ReserveWait::cancel() noexcept {
    WaitState expected = WaitState::Waiting;
    if (!state_.compare_exchange_strong(expected, WaitState::Cancelled,
                                        std::memory_order_acq_rel)) {
        return false;
    }
    // A cancelled head no longer blocks the waiters behind it.
    ManagerAccess::wake(Manager::get());
    return true;
}

Reservation ReserveWait::take() noexcept {
    if (taken_ || state() != WaitState::Granted) return Reservation{};
    taken_ = true;
    return Reservation{pool_, bytes_, Status::Ok};
}

// The seq_cst fence pairs with the one in wake(): either the releaser sees
// this waiter in the count, or the grant pass below sees the released bytes.
// The pusher names its node by arrival, never by address: once listed, the
// node can be granted, freed and its block reused by a newer node.
bool ManagerAccess::push_wait(Manager& m, ReserveWait& w) noexcept {
    w.refs_.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t arrival =
        m.wait_arrivals_.fetch_add(1, std::memory_order_relaxed);
    w.arrival_ = arrival;
    m.waiters_.fetch_add(1, std::memory_order_relaxed);
    ReserveWait* head = m.wait_head_.load(std::memory_order_relaxed);
    do {
        w.next_ = head;
    } while (!m.wait_head_.compare_exchange_weak(
        head, &w, std::memory_order_release, std::memory_order_relaxed));
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return grant(m, arrival);
}

void ManagerAccess::wake(Manager& m) noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (m.waiters_.load(std::memory_order_relaxed) == 0) return;
    grant(m, NO_WAITER);
}

// `again` is raised before `granting` is tried, and the holder re-reads it
// after it lowers `granting`, so a caller that loses the flag is always
// served by the holder or wins the flag itself. Every write to `again` is a
// read-modify-write, so the holder that clears it synchronizes with each
// caller that raised it.
bool ManagerAccess::grant(Manager& m, std::uint64_t self) noexcept {
    bool self_granted = false;
    m.again_.exchange(true, std::memory_order_seq_cst);
    while (true) {
        if (m.granting_.exchange(true, std::memory_order_seq_cst)) {
            return self_granted;
        }
        while (m.again_.exchange(false, std::memory_order_seq_cst)) {
            grant_pass(m, self, self_granted);
        }
        m.granting_.store(false, std::memory_order_seq_cst);
        if (!m.again_.load(std::memory_order_seq_cst)) return self_granted;
    }
}

// Runs only while `granting` is held, which owns wait_held_. The held nodes
// are sorted by arrival and every newer push has a larger number, so merging
// the taken list keeps strict first in, first out order.
void ManagerAccess::grant_pass(Manager& m, std::uint64_t self,
                               bool& self_granted) noexcept {
    ReserveWait* taken =
        m.wait_head_.exchange(nullptr, std::memory_order_acquire);
    if (taken != nullptr) {
        m.wait_held_ = merge_arrivals(m.wait_held_, sort_arrivals(taken));
    }
    ReserveWait** link = &m.wait_held_;
    while (ReserveWait* w = *link) {
        if (w->state_.load(std::memory_order_acquire) == WaitState::Cancelled) {
            *link = w->next_;
            m.waiters_.fetch_sub(1, std::memory_order_relaxed);
            w->drop();
            continue;
        }
        Pool& p = *w->pool_;
        if (PoolAccess::add_reserved(m, p, w->bytes_,
                                     PoolAccess::slack_of(m, p))
                .status != Status::Ok) {
            break;
        }
        *link = w->next_;
        m.waiters_.fetch_sub(1, std::memory_order_relaxed);
        WaitState expected = WaitState::Waiting;
        if (w->state_.compare_exchange_strong(expected, WaitState::Granted,
                                              std::memory_order_acq_rel)) {
            mark_dirty(m);
            if (w->arrival_ == self) {
                self_granted = true;
            } else {
                w->resume_(w->context_);
            }
        } else {
            PoolAccess::uncharge(m, p, w->bytes_);
        }
        w->drop();
    }
}

ReserveWait* ManagerAccess::merge_arrivals(ReserveWait* a,
                                           ReserveWait* b) noexcept {
    ReserveWait* out = nullptr;
    ReserveWait** tail = &out;
    while (a != nullptr && b != nullptr) {
        ReserveWait*& pick = a->arrival_ < b->arrival_ ? a : b;
        *tail = pick;
        tail = &pick->next_;
        pick = pick->next_;
    }
    *tail = a != nullptr ? a : b;
    return out;
}

ReserveWait* ManagerAccess::sort_arrivals(ReserveWait* list) noexcept {
    if (list == nullptr || list->next_ == nullptr) return list;
    ReserveWait* slow = list;
    for (ReserveWait* fast = list->next_;
         fast != nullptr && fast->next_ != nullptr; fast = fast->next_->next_) {
        slow = slow->next_;
    }
    ReserveWait* second = slow->next_;
    slow->next_ = nullptr;
    return merge_arrivals(sort_arrivals(list), sort_arrivals(second));
}

PoolHandle::PoolHandle(PoolHandle&& other) noexcept
    : pool_(other.pool_),
      generation_(other.generation_),
      status_(other.status_) {
    other.pool_ = nullptr;
}

PoolHandle& PoolHandle::operator=(PoolHandle&& other) noexcept {
    if (this != &other) {
        close();
        pool_ = other.pool_;
        generation_ = other.generation_;
        status_ = other.status_;
        other.pool_ = nullptr;
    }
    return *this;
}

PoolHandle::~PoolHandle() { close(); }

void PoolHandle::close() noexcept {
    if (pool_ != nullptr) PoolAccess::close(*pool_, generation_);
    pool_ = nullptr;
}

PoolHandle Pool::child(std::string_view name, PoolKind kind, Limits limits) {
    Manager& m = Manager::get();
    Pool* p = ManagerAccess::claim(m);
    if (p == nullptr) {
        return PoolHandle{nullptr, 0, Status::PoolTableFull};
    }
    std::size_t size = std::min<std::size_t>(name.size(), NAME_CAPACITY - 1);
    std::memcpy(p->name_, name.data(), size);
    p->name_size_ = static_cast<std::uint8_t>(size);
    p->parent_ = id_;
    p->kind_ = kind;
    p->limit_.store(limits.bytes, std::memory_order_relaxed);
    p->peak_.store(0, std::memory_order_relaxed);
    std::uint32_t generation =
        p->generation_.load(std::memory_order_relaxed) + 1;
    p->generation_.store(generation, std::memory_order_relaxed);
    children_.fetch_add(1, std::memory_order_relaxed);
    open_children_.fetch_add(1, std::memory_order_relaxed);
    p->state_.store(STATE_OPEN, std::memory_order_release);
    return PoolHandle{p, generation, Status::Ok};
}

Reservation Pool::reserve(std::size_t bytes) noexcept {
    if (bytes == 0) return Reservation{this, 0, Status::Ok};
    Manager& m = Manager::get();
    bool reclaiming = reclaiming_.load(std::memory_order_relaxed) != 0;
    std::size_t slack = reclaiming ? m.spill_headroom() : 0;
    Admit admit = PoolAccess::add_reserved(m, *this, bytes, slack);
    if (admit.status != Status::Ok && admit.shortfall != 0 && !reclaiming) {
        std::size_t posted =
            ManagerAccess::post_requests(m, *this, admit.shortfall);
        m.trim(Mode::All);
        admit = PoolAccess::add_reserved(m, *this, bytes, slack);
        if (admit.status != Status::Ok && posted != 0) {
            admit.status = Status::Pending;
        }
    }
    if (admit.status != Status::Ok) {
        // A failed attempt may have charged a level and taken it back while a
        // grant pass looked, so that pass may have stopped on room that is
        // free again.
        ManagerAccess::wake(m);
        return Reservation{nullptr, 0, admit.status};
    }
    ManagerAccess::mark_dirty(m);
    return Reservation{this, bytes, Status::Ok};
}

Reservation Pool::try_reserve(std::size_t bytes) noexcept {
    if (bytes == 0) return Reservation{this, 0, Status::Ok};
    Manager& m = Manager::get();
    const bool reclaiming = reclaiming_.load(std::memory_order_relaxed) != 0;
    const Admit admit = PoolAccess::add_reserved(
        m, *this, bytes, reclaiming ? m.spill_headroom() : 0);
    if (admit.status != Status::Ok) {
        ManagerAccess::wake(m);
        return Reservation{nullptr, 0, admit.status};
    }
    ManagerAccess::mark_dirty(m);
    return Reservation{this, bytes, Status::Ok};
}

void Pool::raise_limit(std::size_t bytes) noexcept {
    std::size_t cur = limit_.load(std::memory_order_relaxed);
    while (cur != 0 && !limit_.compare_exchange_weak(
                           cur, cur + bytes, std::memory_order_relaxed)) {
    }
    if (cur != 0) ManagerAccess::wake(Manager::get());
}

std::string Pool::path() const {
    char buffer[PATH_DEPTH * (NAME_CAPACITY + 1)];
    std::size_t n =
        PoolAccess::path(Manager::get(), id_, buffer, sizeof(buffer));
    return std::string(buffer, n);
}

std::size_t Pool::live() const {
    return clamp_size(PoolAccess::snapshot(Manager::get()).sub_bytes[id_]);
}

std::size_t Pool::blocks() const {
    return clamp_size(PoolAccess::snapshot(Manager::get()).sub_blocks[id_]);
}

std::size_t Pool::peak() const {
    PoolAccess::snapshot(Manager::get());
    return PoolAccess::peak_of(*this);
}

std::size_t Pool::reserved() const noexcept {
    return clamp_size(reserved_.load(std::memory_order_relaxed));
}

std::size_t Pool::adopted() const {
    Manager& m = Manager::get();
    return PoolAccess::adopted_by(m, PoolAccess::snapshot(m))[id_];
}

std::size_t Pool::exported() const noexcept {
    return clamp_size(exported_.load(std::memory_order_relaxed));
}

void Pool::add_exported(std::size_t bytes) noexcept {
    exported_.fetch_add(static_cast<std::ptrdiff_t>(bytes),
                        std::memory_order_relaxed);
}

void Pool::sub_exported(std::size_t bytes) noexcept {
    exported_.fetch_sub(static_cast<std::ptrdiff_t>(bytes),
                        std::memory_order_relaxed);
}

std::vector<PoolStats> pool_stats() {
    Manager& m = Manager::get();
    PoolAccess::Snapshot s = PoolAccess::snapshot(m);
    std::vector<std::size_t> adopted = PoolAccess::adopted_by(m, s);
    std::vector<PoolStats> out;
    for (std::size_t i = 0; i < s.sub_bytes.size(); ++i) {
        const Pool& p = ManagerAccess::slot(m, i);
        if (!PoolAccess::is_open(p)) continue;
        PoolStats st{};
        st.id = p.id();
        st.parent = p.parent();
        st.kind = p.kind();
        st.path = p.path();
        st.live = clamp_size(s.sub_bytes[i]);
        st.blocks = clamp_size(s.sub_blocks[i]);
        st.reserved = p.reserved();
        st.peak = PoolAccess::peak_of(p);
        st.adopted = adopted[i];
        st.exported = p.exported();
        out.push_back(std::move(st));
    }
    return out;
}

std::uint16_t ManagerAccess::top_of(Manager& m, std::uint16_t id) noexcept {
    for (std::size_t depth = 0; id != ROOT_POOL_ID && depth < PATH_DEPTH;
         ++depth) {
        std::uint16_t parent = m.table_[id].parent_;
        if (parent == ROOT_POOL_ID) return id;
        id = parent;
    }
    return ROOT_POOL_ID;
}

// Each round picks the entry with the most bytes to give among those not
// picked yet, entries below the caller's child of the root first, and asks it
// for the smaller of what it can give and what is still missing.
std::size_t ManagerAccess::post_requests(Manager& m, const Pool& caller,
                                         std::size_t shortfall) noexcept {
    std::uint16_t home = top_of(m, caller.id_);
    std::uint64_t picked[RECLAIMER_SLOTS / 64] = {};
    std::size_t posted = 0;
    for (std::size_t round = 0; round < MAX_POSTS && posted < shortfall;
         ++round) {
        std::size_t best = RECLAIMER_SLOTS;
        std::size_t best_give = 0;
        bool best_home = false;
        for (std::size_t i = 0; i < RECLAIMER_SLOTS; ++i) {
            if ((picked[i / 64] >> (i % 64)) & 1) continue;
            ReclaimEntry& e = m.reclaimers_[i];
            if (e.state.load(std::memory_order_acquire) != ENTRY_ATTACHED) {
                continue;
            }
            std::size_t have = e.reclaimable.load(std::memory_order_relaxed);
            std::size_t asked = e.requested.load(std::memory_order_relaxed);
            std::size_t give = have > asked ? have - asked : 0;
            if (give == 0) continue;
            bool is_home =
                top_of(m, e.pool_id.load(std::memory_order_relaxed)) == home;
            if (best == RECLAIMER_SLOTS || (is_home && !best_home) ||
                (is_home == best_home && give > best_give)) {
                best = i;
                best_give = give;
                best_home = is_home;
            }
        }
        if (best == RECLAIMER_SLOTS) break;
        picked[best / 64] |= std::uint64_t{1} << (best % 64);
        std::size_t take = std::min(best_give, shortfall - posted);
        m.reclaimers_[best].requested.fetch_add(take,
                                                std::memory_order_relaxed);
        posted += take;
    }
    return posted;
}

ReclaimEntry* ManagerAccess::claim_reclaimer(Manager& m) noexcept {
    for (std::size_t i = 0; i < RECLAIMER_SLOTS; ++i) {
        std::uint32_t expected = ENTRY_FREE;
        if (m.reclaimers_[i].state.compare_exchange_strong(
                expected, ENTRY_BUSY, std::memory_order_acquire)) {
            return &m.reclaimers_[i];
        }
    }
    return nullptr;
}

ExternalEntry* ManagerAccess::claim_external(Manager& m) noexcept {
    for (std::size_t i = 0; i < EXTERNAL_SLOTS; ++i) {
        std::uint32_t expected = ENTRY_FREE;
        if (m.externals_[i].state.compare_exchange_strong(
                expected, ENTRY_BUSY, std::memory_order_acquire)) {
            return &m.externals_[i];
        }
    }
    return nullptr;
}

ReclaimSlot Pool::attach(Reclaimer& reclaimer) {
    std::size_t first = reclaimer.reclaimable();
    Manager& m = Manager::get();
    ReclaimEntry* e = ManagerAccess::claim_reclaimer(m);
    if (e == nullptr)
        return ReclaimSlot{nullptr, this, 0, Status::SlotTableFull};
    e->pool_id.store(id_, std::memory_order_relaxed);
    e->reclaimable.store(first, std::memory_order_relaxed);
    e->requested.store(0, std::memory_order_relaxed);
    std::uint32_t generation = e->generation.load(std::memory_order_relaxed);
    e->state.store(ENTRY_ATTACHED, std::memory_order_release);
    return ReclaimSlot{e, this, generation, Status::Ok};
}

ReclaimSlot::ReclaimSlot(ReclaimSlot&& other) noexcept
    : entry_(other.entry_),
      pool_(other.pool_),
      generation_(other.generation_),
      status_(other.status_) {
    other.entry_ = nullptr;
}

ReclaimSlot& ReclaimSlot::operator=(ReclaimSlot&& other) noexcept {
    if (this != &other) {
        detach();
        entry_ = other.entry_;
        pool_ = other.pool_;
        generation_ = other.generation_;
        status_ = other.status_;
        other.entry_ = nullptr;
    }
    return *this;
}

ReclaimSlot::~ReclaimSlot() { detach(); }

void ReclaimSlot::set_reclaimable(std::size_t bytes) noexcept {
    if (entry_ != nullptr) {
        entry_->reclaimable.store(bytes, std::memory_order_relaxed);
    }
}

std::size_t ReclaimSlot::take_request() noexcept {
    if (entry_ == nullptr) return 0;
    return entry_->requested.exchange(0, std::memory_order_relaxed);
}

ReclaimGuard ReclaimSlot::begin_reclaim() noexcept {
    if (entry_ == nullptr) return ReclaimGuard{};
    pool_->reclaiming_.fetch_add(1, std::memory_order_relaxed);
    return ReclaimGuard{pool_};
}

void ReclaimSlot::detach() noexcept {
    if (entry_ == nullptr) return;
    if (entry_->generation.load(std::memory_order_relaxed) == generation_) {
        entry_->reclaimable.store(0, std::memory_order_relaxed);
        entry_->requested.store(0, std::memory_order_relaxed);
        entry_->generation.fetch_add(1, std::memory_order_relaxed);
        entry_->state.store(ENTRY_FREE, std::memory_order_release);
    }
    entry_ = nullptr;
}

ReclaimGuard& ReclaimGuard::operator=(ReclaimGuard&& other) noexcept {
    if (this != &other) {
        if (pool_ != nullptr) {
            pool_->reclaiming_.fetch_sub(1, std::memory_order_relaxed);
        }
        pool_ = other.pool_;
        other.pool_ = nullptr;
    }
    return *this;
}

ReclaimGuard::~ReclaimGuard() {
    if (pool_ != nullptr) {
        pool_->reclaiming_.fetch_sub(1, std::memory_order_relaxed);
    }
}

ExternalSlot attach_external(std::string_view name) {
    Manager& m = Manager::get();
    ExternalEntry* e = ManagerAccess::claim_external(m);
    if (e == nullptr) return ExternalSlot{nullptr, Status::SlotTableFull};
    std::size_t size = std::min<std::size_t>(name.size(), NAME_MAX_BYTES);
    std::memcpy(e->name, name.data(), size);
    e->name_size = static_cast<std::uint8_t>(size);
    e->counted.store(0, std::memory_order_relaxed);
    e->reported.store(0, std::memory_order_relaxed);
    e->state.store(ENTRY_ATTACHED, std::memory_order_release);
    return ExternalSlot{e, Status::Ok};
}

ExternalSlot::ExternalSlot(ExternalSlot&& other) noexcept
    : entry_(other.entry_), status_(other.status_) {
    other.entry_ = nullptr;
}

ExternalSlot& ExternalSlot::operator=(ExternalSlot&& other) noexcept {
    if (this != &other) {
        detach();
        entry_ = other.entry_;
        status_ = other.status_;
        other.entry_ = nullptr;
    }
    return *this;
}

ExternalSlot::~ExternalSlot() { detach(); }

void ExternalSlot::set(std::size_t bytes) noexcept {
    if (entry_ == nullptr) return;
    std::size_t old =
        entry_->counted.exchange(bytes, std::memory_order_relaxed);
    Manager& m = Manager::get();
    ManagerAccess::mark_dirty(m);
    if (bytes < old) ManagerAccess::wake(m);
}

// Reported bytes never reach the budget, so they neither dirty the pressure
// flag nor wake a waiter.
void ExternalSlot::add(std::ptrdiff_t delta, Kind kind) noexcept {
    if (entry_ == nullptr || delta == 0) return;
    if (kind == Kind::Reported) {
        entry_->reported.fetch_add(static_cast<std::size_t>(delta),
                                   std::memory_order_relaxed);
        return;
    }
    entry_->counted.fetch_add(static_cast<std::size_t>(delta),
                              std::memory_order_relaxed);
    Manager& m = Manager::get();
    ManagerAccess::mark_dirty(m);
    if (delta < 0) ManagerAccess::wake(m);
}

std::size_t ExternalSlot::bytes() const noexcept {
    return entry_ == nullptr
               ? 0
               : entry_->counted.load(std::memory_order_relaxed) +
                     entry_->reported.load(std::memory_order_relaxed);
}

void ExternalSlot::detach() noexcept {
    if (entry_ == nullptr) return;
    entry_->counted.store(0, std::memory_order_relaxed);
    entry_->reported.store(0, std::memory_order_relaxed);
    entry_->state.store(ENTRY_FREE, std::memory_order_release);
    entry_ = nullptr;
    Manager& m = Manager::get();
    ManagerAccess::mark_dirty(m);
    ManagerAccess::wake(m);
}

std::vector<ExternalStats> external_stats() {
    Manager& m = Manager::get();
    std::vector<ExternalStats> out;
    for (std::size_t i = 0; i < EXTERNAL_SLOTS; ++i) {
        const ExternalEntry& e = ManagerAccess::external_at(m, i);
        if (e.state.load(std::memory_order_acquire) != ENTRY_ATTACHED) {
            continue;
        }
        out.push_back({std::string(e.name, e.name_size),
                       e.counted.load(std::memory_order_relaxed) +
                           e.reported.load(std::memory_order_relaxed)});
    }
    return out;
}

}  // namespace dftracer::utils::memory
