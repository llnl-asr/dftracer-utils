#include <dftracer/utils/memory/asan.h>
#include <dftracer/utils/memory/attrib.h>
#include <dftracer/utils/memory/heap_page.h>
#include <dftracer/utils/memory/heap_system.h>
#include <dftracer/utils/memory/link.h>
#include <dftracer/utils/memory/settings.h>
#include <dftracer/utils/memory/vg.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

namespace dftracer::utils::memory {

namespace {

constexpr std::size_t PAGES_PER_SPAN = HEAP_SPAN_SIZE / HEAP_PAGE_SIZE;
constexpr std::size_t SPAN_MASK = HEAP_SPAN_SIZE - 1;
constexpr std::size_t MIN_ALIGN = 16;
constexpr std::size_t CLASS_COUNT = 40;
constexpr std::uint64_t ALL_PAGES_FREE = ~std::uint64_t{1};
constexpr std::size_t REMOTE_BUFFER = 64;
constexpr std::size_t USABLE_PAGES = PAGES_PER_SPAN - 1;
constexpr std::size_t CACHE_COUNT = 8;
constexpr std::size_t CACHE_ENTRIES_PER_CALL = 4;
constexpr std::size_t PURGE_SPANS_PER_CALL = 4;
constexpr std::size_t UNLIMITED = std::numeric_limits<std::size_t>::max();
constexpr unsigned HEAP_ADDR_SHIFT = 22;
constexpr unsigned HEAP_TAG_SHIFT = 34;
constexpr std::uint64_t HEAP_ADDR_MASK =
    (std::uint64_t{1} << HEAP_TAG_SHIFT) - 1;
static_assert(HEAP_SPAN_SIZE == std::size_t{1} << HEAP_ADDR_SHIFT);

constexpr std::size_t class_size_of(std::size_t idx) noexcept {
    if (idx < 4) return (idx + 1) * MIN_ALIGN;
    std::size_t j = idx - 4;
    std::size_t k = 6 + j / 4;
    return (std::size_t{1} << k) + (j % 4 + 1) * (std::size_t{1} << (k - 2));
}

constexpr std::array<std::uint32_t, CLASS_COUNT> make_class_sizes() noexcept {
    std::array<std::uint32_t, CLASS_COUNT> sizes{};
    for (std::size_t i = 0; i < CLASS_COUNT; ++i) {
        sizes[i] = static_cast<std::uint32_t>(class_size_of(i));
    }
    return sizes;
}

constexpr std::array<std::uint32_t, CLASS_COUNT> CLASS_SIZES =
    make_class_sizes();
static_assert(CLASS_SIZES[CLASS_COUNT - 1] == SMALL_MAX);

enum class PageKind : std::uint8_t { Free, Small, MediumHead };
enum class SpanKind : std::uint8_t { Standard, Large, Huge };

struct PageDesc {
    void* free_list;
    std::uint16_t* ids;
    PageDesc* prev;
    PageDesc* next;
    std::atomic<void*> thread_free;
    PageDesc* inbox_next;
    std::size_t size;
    std::uint32_t used;
    std::uint32_t capacity;
    std::uint32_t bump;
    std::uint32_t pages;
    std::uint8_t cls;
    PageKind kind;
    bool listed;
    std::uint16_t pool_id;
};

struct Heap;

struct SpanHeader {
    Heap* owner;
    SpanHeader* prev;
    SpanHeader* next;
    std::size_t map_bytes;
    std::size_t size;
    std::size_t usable;
    std::uint64_t free_mask;
    std::uint64_t purged_mask;
    std::uint64_t cached_since;
    std::atomic<SpanHeader*> cache_next;
    std::uint32_t free_pages;
    SpanKind kind;
    std::uint16_t pool_id;
    std::uint64_t free_since[PAGES_PER_SPAN];
    PageDesc pages[PAGES_PER_SPAN];
};
static_assert(sizeof(SpanHeader) <= (std::size_t{16} << 10));

struct Chain {
    PageDesc* page;
    void* head;
    void* tail;
};

struct Heap {
    PageDesc* avail[CLASS_COUNT];
    SpanHeader* spans;
    SpanHeader* purge_cursor;
    std::atomic<PageDesc*> inbox;
    std::atomic<Heap*> stack_next;
    Heap* all_next;
    std::atomic<std::ptrdiff_t> reserved;
    std::uint32_t remote_count;
    std::uint32_t chain_count;
    Chain chains[REMOTE_BUFFER];
    AttribCache attrib;
};
static_assert(sizeof(Heap) <= HEAP_PAGE_SIZE);

thread_local Heap* t_heap = nullptr;

// Low bits: heap address >> 22 (heaps are 4 MiB aligned). High bits: a tag
// that grows on every push and pop, so a stale pop cannot succeed (ABA).
std::atomic<std::uint64_t> g_free_heaps{0};
std::atomic<Heap*> g_all_heaps{nullptr};

// A cache holds idle spans (index 0) or idle large runs (index i is a run of
// 1 MiB << (i - 1)). Pushes land on young. A decay pass moves the whole young
// stack to old when old is empty, then purges old from its newest entry
// down once that entry is past the delay, and parks purged entries on cold.
// Entries are never unmapped and their header page is never purged, so a
// pop that read a stale head still reads mapped memory; the tag makes its CAS
// fail.
struct Cache {
    std::atomic<std::uint64_t> young;
    std::atomic<std::uint64_t> old;
    std::atomic<std::uint64_t> cold;
    std::atomic<bool> busy;
};
Cache g_caches[CACHE_COUNT];
std::atomic<std::ptrdiff_t> g_cache_reserved{0};
pthread_key_t g_key;
pthread_once_t g_once = PTHREAD_ONCE_INIT;

template <class T>
void list_push(T*& head, T* node) noexcept {
    node->prev = nullptr;
    node->next = head;
    if (head != nullptr) head->prev = node;
    head = node;
}

template <class T>
void list_remove(T*& head, T* node) noexcept {
    if (node->prev != nullptr) {
        node->prev->next = node->next;
    } else {
        head = node->next;
    }
    if (node->next != nullptr) node->next->prev = node->prev;
    node->prev = nullptr;
    node->next = nullptr;
}

constexpr std::size_t round_up(std::size_t n, std::size_t to) noexcept {
    return (n + to - 1) & ~(to - 1);
}

constexpr std::size_t PAGE_MASK = HEAP_PAGE_SIZE - 1;

// Every small block has a 16-bit pool id. A class of HEADER_IDS_MIN_BLOCK
// bytes or more keeps its ids in the span's header page, so its page is all
// blocks. A smaller class keeps them at the start of its page, padded so the
// blocks keep the alignment of their class. The slot of a block is found by a
// multiply with the reciprocal of the class size, exact for offsets below a
// page.
constexpr std::size_t HEADER_IDS_MIN_BLOCK = 512;
constexpr std::size_t HEADER_IDS_PER_PAGE =
    HEAP_PAGE_SIZE / HEADER_IDS_MIN_BLOCK;
constexpr std::size_t HEADER_IDS_OFFSET = std::size_t{16} << 10;
static_assert(HEADER_IDS_OFFSET + PAGES_PER_SPAN * HEADER_IDS_PER_PAGE * 2 <=
              HEAP_PAGE_SIZE);

struct SmallLayout {
    std::uint32_t capacity;
    std::uint32_t side;
    std::uint32_t magic;
    std::uint32_t delta;
};

constexpr SmallLayout make_small_layout(std::size_t block) noexcept {
    bool in_header = block >= HEADER_IDS_MIN_BLOCK;
    std::size_t capacity = HEAP_PAGE_SIZE / block;
    std::size_t side = 0;
    if (!in_header) {
        while (round_up(capacity * 2, SMALL_MAX_ALIGN) + capacity * block >
               HEAP_PAGE_SIZE) {
            --capacity;
        }
        side = round_up(capacity * 2, SMALL_MAX_ALIGN);
    }
    std::uint64_t magic = ((std::uint64_t{1} << 32) + block - 1) / block;
    return {static_cast<std::uint32_t>(capacity),
            static_cast<std::uint32_t>(side), static_cast<std::uint32_t>(magic),
            static_cast<std::uint32_t>(attrib_delta(block))};
}

constexpr std::array<SmallLayout, CLASS_COUNT> make_small_layouts() noexcept {
    std::array<SmallLayout, CLASS_COUNT> layouts{};
    for (std::size_t i = 0; i < CLASS_COUNT; ++i) {
        layouts[i] = make_small_layout(CLASS_SIZES[i]);
    }
    return layouts;
}

constexpr std::array<SmallLayout, CLASS_COUNT> SMALL_LAYOUTS =
    make_small_layouts();

std::uint16_t* page_ids(SpanHeader* span, std::size_t index,
                        const SmallLayout& layout) noexcept {
    if (layout.side != 0) {
        return reinterpret_cast<std::uint16_t*>(reinterpret_cast<char*>(span) +
                                                index * HEAP_PAGE_SIZE);
    }
    return reinterpret_cast<std::uint16_t*>(reinterpret_cast<char*>(span) +
                                            HEADER_IDS_OFFSET) +
           index * HEADER_IDS_PER_PAGE;
}

std::size_t slot_of(const SmallLayout& layout, const void* p) noexcept {
    std::uint64_t offset =
        (reinterpret_cast<std::uintptr_t>(p) & PAGE_MASK) - layout.side;
    return static_cast<std::size_t>((offset * layout.magic) >> 32);
}

std::uintptr_t addr(const void* p) noexcept {
    return reinterpret_cast<std::uintptr_t>(p);
}

SpanHeader* span_of(const void* p) noexcept {
    return reinterpret_cast<SpanHeader*>(addr(p) & ~SPAN_MASK);
}

char* page_base(SpanHeader* span, std::size_t index) noexcept {
    return reinterpret_cast<char*>(addr(span) + index * HEAP_PAGE_SIZE);
}

std::size_t class_index(std::size_t size) noexcept {
    if (size <= 64) return (size + MIN_ALIGN - 1) / MIN_ALIGN - 1;
    std::size_t k = std::bit_width(size - 1) - 1U;
    std::size_t q = (size - 1 - (std::size_t{1} << k)) >> (k - 2);
    return 4 + (k - 6) * 4 + q;
}

std::size_t class_for(std::size_t size, std::size_t align) noexcept {
    std::size_t idx = class_index(std::max<std::size_t>(size, 1));
    if (align > MIN_ALIGN) {
        while (CLASS_SIZES[idx] % align != 0) ++idx;
    }
    return idx;
}

#ifdef DFTRACER_UTILS_MEMORY_CHECKS
bool small_size_matches(std::size_t cls, std::size_t size) noexcept {
    for (std::size_t align = MIN_ALIGN; align <= SMALL_MAX_ALIGN; align *= 2) {
        if (class_for(size, align) == cls) return true;
    }
    return false;
}

void check_size(bool matches) noexcept {
    if (!matches) {
        abort_with(
            "dftracer-utils memory: sized free does not match the allocated "
            "size\n");
    }
}
#endif

void* map_aligned(std::size_t bytes) noexcept {
    std::size_t total = bytes + HEAP_SPAN_SIZE;
    void* raw = ::mmap(nullptr, total, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    if (raw == MAP_FAILED) return nullptr;
    std::uintptr_t start = addr(raw);
    std::uintptr_t base = (start + SPAN_MASK) & ~SPAN_MASK;
    if (base > start) {
        ::munmap(raw, base - start);
    }
    std::uintptr_t tail = start + total - (base + bytes);
    if (tail > 0) {
        ::munmap(reinterpret_cast<void*>(base + bytes), tail);
    }
    return reinterpret_cast<void*>(base);
}

// Only the thread that holds a heap writes its counters, so a relaxed load
// and store replaces a read-modify-write; other threads only read them.
void bump(std::atomic<std::ptrdiff_t>& counter, std::size_t n,
          bool grow) noexcept {
    auto delta = static_cast<std::ptrdiff_t>(n);
    counter.store(
        counter.load(std::memory_order_relaxed) + (grow ? delta : -delta),
        std::memory_order_relaxed);
}

// Called before bytes are mapped, never for a cached span or run: those move
// between the cache and a heap and leave the total as it was.
void check_cap(std::size_t bytes) {
    std::size_t cap = hard_cap();
    if (cap != 0 && page_reserved() + bytes > cap) [[unlikely]]
        throw std::bad_alloc();
}

std::uint64_t pack_heap(Heap* h, std::uint64_t tag) noexcept {
    return (tag << HEAP_TAG_SHIFT) | (addr(h) >> HEAP_ADDR_SHIFT);
}

Heap* heap_of(std::uint64_t packed) noexcept {
    return reinterpret_cast<Heap*>((packed & HEAP_ADDR_MASK)
                                   << HEAP_ADDR_SHIFT);
}

std::uint64_t tag_of(std::uint64_t packed) noexcept {
    return packed >> HEAP_TAG_SHIFT;
}

Heap* pop_free_heap() noexcept {
    std::uint64_t old = g_free_heaps.load(std::memory_order_acquire);
    while (true) {
        Heap* h = heap_of(old);
        if (h == nullptr) return nullptr;
        Heap* next = h->stack_next.load(std::memory_order_relaxed);
        if (g_free_heaps.compare_exchange_weak(
                old, pack_heap(next, tag_of(old) + 1),
                std::memory_order_acquire, std::memory_order_acquire)) {
            return h;
        }
    }
}

void push_free_heap(Heap* h) noexcept {
    std::uint64_t old = g_free_heaps.load(std::memory_order_relaxed);
    do {
        h->stack_next.store(heap_of(old), std::memory_order_relaxed);
    } while (!g_free_heaps.compare_exchange_weak(
        old, pack_heap(h, tag_of(old) + 1), std::memory_order_release,
        std::memory_order_relaxed));
}

Heap* create_heap() {
    void* raw = map_aligned(HEAP_PAGE_SIZE);
    if (raw == nullptr) throw std::bad_alloc();
    if ((addr(raw) >> HEAP_ADDR_SHIFT) > HEAP_ADDR_MASK) {
        abort_with("dftracer-utils memory: address space is above 56 bits\n");
    }
    auto* h = new (raw) Heap{};
    h->reserved.store(static_cast<std::ptrdiff_t>(HEAP_PAGE_SIZE),
                      std::memory_order_relaxed);
    attrib_register(h->attrib);
    Heap* head = g_all_heaps.load(std::memory_order_relaxed);
    do {
        h->all_next = head;
    } while (!g_all_heaps.compare_exchange_weak(
        head, h, std::memory_order_release, std::memory_order_relaxed));
    return h;
}

void push_chain(const Chain& c) noexcept {
    PageDesc* d = c.page;
    void* old = d->thread_free.load(std::memory_order_relaxed);
    do {
        store_link(c.tail, old);
    } while (!d->thread_free.compare_exchange_weak(
        old, c.head, std::memory_order_acq_rel, std::memory_order_relaxed));
    if (old != nullptr) return;
    // Only the push onto an empty list registers the page, so a page is in
    // the inbox at most once and its inbox_next is free to reuse.
    Heap* owner = span_of(d)->owner;
    PageDesc* head = owner->inbox.load(std::memory_order_relaxed);
    do {
        d->inbox_next = head;
    } while (!owner->inbox.compare_exchange_weak(
        head, d, std::memory_order_release, std::memory_order_relaxed));
}

void flush_remote(Heap& h) noexcept {
    for (std::uint32_t i = 0; i < h.chain_count; ++i) push_chain(h.chains[i]);
    h.chain_count = 0;
    h.remote_count = 0;
}

void remote_free(Heap& h, PageDesc* d, void* p) noexcept {
    Chain* c = nullptr;
    for (std::uint32_t i = h.chain_count; i-- > 0;) {
        if (h.chains[i].page == d) {
            c = &h.chains[i];
            break;
        }
    }
    if (c == nullptr) {
        c = &h.chains[h.chain_count++];
        *c = Chain{d, nullptr, nullptr};
    }
    store_link(p, c->head);
    c->head = p;
    if (c->tail == nullptr) c->tail = p;
    if (++h.remote_count == REMOTE_BUFFER) flush_remote(h);
}

void free_small_block(Heap& h, SpanHeader* s, std::size_t index,
                      void* p) noexcept;
void free_medium_run(Heap& h, SpanHeader* s, std::size_t index) noexcept;

bool drain_inbox(Heap& h) noexcept {
    if (h.inbox.load(std::memory_order_relaxed) == nullptr) return false;
    PageDesc* d = h.inbox.exchange(nullptr, std::memory_order_acq_rel);
    bool any = false;
    while (d != nullptr) {
        PageDesc* next = d->inbox_next;
        void* list =
            d->thread_free.exchange(nullptr, std::memory_order_acq_rel);
        SpanHeader* s = span_of(d);
        auto index = static_cast<std::size_t>(d - s->pages);
        while (list != nullptr) {
            void* following = load_link(list);
            if (d->kind == PageKind::Small) {
                free_small_block(h, s, index, list);
            } else {
                free_medium_run(h, s, index);
            }
            list = following;
            any = true;
        }
        d = next;
    }
    return any;
}

bool slow_path(Heap& h) noexcept {
    flush_remote(h);
    return drain_inbox(h);
}

void thread_exit(void* arg) {
    auto* h = static_cast<Heap*>(arg);
    flush_remote(*h);
    drain_inbox(*h);
    attrib_set_scope(h->attrib, GENERAL_POOL_ID);
    t_heap = nullptr;
    push_free_heap(h);
}

// Blocks in the remote buffers of threads that did not survive the fork are
// lost, and so are cache entries such a thread had popped for a flip or a
// purge and not yet pushed back. They stay mapped and stay counted in the
// child.
void after_fork_child() noexcept {
    Heap* own = t_heap;
    g_free_heaps.store(0, std::memory_order_relaxed);
    for (Cache& c : g_caches) c.busy.store(false, std::memory_order_relaxed);
    for (Heap* h = g_all_heaps.load(std::memory_order_acquire); h != nullptr;
         h = h->all_next) {
        if (h == own) continue;
        h->remote_count = 0;
        h->chain_count = 0;
        push_free_heap(h);
    }
}

void process_init() {
    vg_init();
    if (::pthread_key_create(&g_key, thread_exit) != 0 ||
        ::pthread_atfork(nullptr, nullptr, after_fork_child) != 0) {
        abort_with("dftracer-utils memory: cannot register thread hooks\n");
    }
}

// A thread that allocates or frees after its key destructor ran lands here
// again: the key is set again, so the destructor runs another round.
Heap& attach_heap() {
    ::pthread_once(&g_once, process_init);
    Heap* h = pop_free_heap();
    if (h == nullptr) h = create_heap();
    t_heap = h;
    if (::pthread_setspecific(g_key, h) != 0) {
        abort_with("dftracer-utils memory: cannot register thread heap\n");
    }
    drain_inbox(*h);
    return *h;
}

Heap& local_heap() {
    if (t_heap == nullptr) [[unlikely]]
        return attach_heap();
    return *t_heap;
}

std::uint64_t run_bits(std::size_t index, std::size_t n) noexcept {
    std::uint64_t ones =
        n >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << n) - 1;
    return ones << index;
}

std::uint64_t now_ms() noexcept {
    timespec ts{};
#if defined(__APPLE__)
    ::clock_gettime(CLOCK_MONOTONIC_RAW_APPROX, &ts);
#elif defined(CLOCK_MONOTONIC_COARSE)
    ::clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
#else
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000 +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000000;
}

bool purge_range(void* p, std::size_t n) noexcept {
#if defined(__APPLE__)
    return ::madvise(p, n, MADV_FREE_REUSABLE) == 0;
#else
    return ::madvise(p, n, MADV_DONTNEED) == 0;
#endif
}

void reuse_range([[maybe_unused]] void* p,
                 [[maybe_unused]] std::size_t n) noexcept {
#if defined(__APPLE__)
    ::madvise(p, n, MADV_FREE_REUSE);
#endif
}

template <class Fn>
void for_each_page_run(std::uint64_t mask, Fn&& fn) noexcept {
    while (mask != 0) {
        auto first = static_cast<std::size_t>(std::countr_zero(mask));
        std::size_t last = first;
        while (last < PAGES_PER_SPAN && ((mask >> last) & 1) != 0) ++last;
        fn(first, last - first);
        mask &= ~run_bits(first, last - first);
    }
}

std::size_t purge_pages(SpanHeader* s, std::uint64_t mask) noexcept {
    std::size_t purged = 0;
    for_each_page_run(mask, [&](std::size_t first, std::size_t n) {
        if (purge_range(page_base(s, first), n * HEAP_PAGE_SIZE)) {
            s->purged_mask |= run_bits(first, n);
            purged += n;
        }
    });
    return purged;
}

void unpurge_pages(Heap& h, SpanHeader* s, std::size_t index,
                   std::size_t n) noexcept {
    std::uint64_t mask = run_bits(index, n) & s->purged_mask;
    if (mask == 0) return;
    for_each_page_run(mask, [&](std::size_t first, std::size_t count) {
        reuse_range(page_base(s, first), count * HEAP_PAGE_SIZE);
    });
    s->purged_mask &= ~mask;
    bump(h.reserved,
         static_cast<std::size_t>(std::popcount(mask)) * HEAP_PAGE_SIZE, true);
}

std::size_t span_backed(const SpanHeader* s) noexcept {
    if (s->kind == SpanKind::Standard) {
        return HEAP_SPAN_SIZE -
               static_cast<std::size_t>(std::popcount(s->purged_mask)) *
                   HEAP_PAGE_SIZE;
    }
    return s->purged_mask != 0 ? HEAP_PAGE_SIZE : s->map_bytes;
}

void span_unlink(Heap& h, SpanHeader* s) noexcept {
    if (h.purge_cursor == s) h.purge_cursor = s->next;
    list_remove(h.spans, s);
}

std::uint64_t pack_span(const SpanHeader* s, std::uint64_t tag) noexcept {
    return (tag << HEAP_TAG_SHIFT) | (addr(s) >> HEAP_ADDR_SHIFT);
}

SpanHeader* span_from(std::uint64_t packed) noexcept {
    return reinterpret_cast<SpanHeader*>((packed & HEAP_ADDR_MASK)
                                         << HEAP_ADDR_SHIFT);
}

SpanHeader* stack_pop(std::atomic<std::uint64_t>& head) noexcept {
    std::uint64_t old = head.load(std::memory_order_acquire);
    while (true) {
        SpanHeader* s = span_from(old);
        if (s == nullptr) return nullptr;
        SpanHeader* next = s->cache_next.load(std::memory_order_relaxed);
        if (head.compare_exchange_weak(old, pack_span(next, tag_of(old) + 1),
                                       std::memory_order_acquire,
                                       std::memory_order_acquire)) {
            return s;
        }
    }
}

void stack_push(std::atomic<std::uint64_t>& head, SpanHeader* s) noexcept {
    std::uint64_t old = head.load(std::memory_order_relaxed);
    do {
        s->cache_next.store(span_from(old), std::memory_order_relaxed);
    } while (!head.compare_exchange_weak(old, pack_span(s, tag_of(old) + 1),
                                         std::memory_order_release,
                                         std::memory_order_relaxed));
}

bool stack_empty(const std::atomic<std::uint64_t>& head) noexcept {
    return span_from(head.load(std::memory_order_relaxed)) == nullptr;
}

SpanHeader* stack_take_all(std::atomic<std::uint64_t>& head) noexcept {
    std::uint64_t old = head.load(std::memory_order_relaxed);
    while (!head.compare_exchange_weak(old, pack_span(nullptr, tag_of(old) + 1),
                                       std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) {
    }
    return span_from(old);
}

void stack_install(std::atomic<std::uint64_t>& head,
                   SpanHeader* list) noexcept {
    std::uint64_t old = head.load(std::memory_order_relaxed);
    while (!head.compare_exchange_weak(old, pack_span(list, tag_of(old) + 1),
                                       std::memory_order_release,
                                       std::memory_order_relaxed)) {
    }
}

SpanHeader* cache_pop(Cache& c) noexcept {
    SpanHeader* s = stack_pop(c.young);
    if (s == nullptr) s = stack_pop(c.old);
    if (s == nullptr) s = stack_pop(c.cold);
    return s;
}

void cache_put(std::size_t index, SpanHeader* s, std::size_t backed) noexcept {
    g_cache_reserved.fetch_add(static_cast<std::ptrdiff_t>(backed),
                               std::memory_order_relaxed);
    s->cached_since = now_ms();
    stack_push(g_caches[index].young, s);
}

void cache_take_accounting(std::size_t backed) noexcept {
    g_cache_reserved.fetch_sub(static_cast<std::ptrdiff_t>(backed),
                               std::memory_order_relaxed);
}

void purge_entry(SpanHeader* s) noexcept {
    if (s->kind == SpanKind::Standard) {
        std::size_t n = purge_pages(s, ALL_PAGES_FREE & ~s->purged_mask);
        cache_take_accounting(n * HEAP_PAGE_SIZE);
    } else if (s->purged_mask == 0 &&
               purge_range(page_base(s, 1), s->map_bytes - HEAP_PAGE_SIZE)) {
        s->purged_mask = 1;
        cache_take_accounting(s->map_bytes - HEAP_PAGE_SIZE);
    }
}

void decay_cache(Cache& c, std::uint64_t now, std::uint64_t delay,
                 std::size_t budget) noexcept {
    if (stack_empty(c.young) && stack_empty(c.old)) return;
    if (c.busy.exchange(true, std::memory_order_acquire)) return;
    bool flipped = false;
    std::size_t done = 0;
    while (done < budget) {
        SpanHeader* e = stack_pop(c.old);
        if (e == nullptr) {
            if (flipped) break;
            flipped = true;
            SpanHeader* batch = stack_take_all(c.young);
            if (batch == nullptr) break;
            stack_install(c.old, batch);
            continue;
        }
        if (now - e->cached_since < delay) {
            stack_push(c.old, e);
            break;
        }
        purge_entry(e);
        stack_push(c.cold, e);
        ++done;
    }
    c.busy.store(false, std::memory_order_release);
}

void purge_span_pages(Heap& h, SpanHeader* s, std::uint64_t now,
                      std::uint64_t delay) noexcept {
    std::uint64_t idle = s->free_mask & ~s->purged_mask;
    std::uint64_t mask = 0;
    while (idle != 0) {
        auto i = static_cast<std::size_t>(std::countr_zero(idle));
        idle &= idle - 1;
        if (now - s->free_since[i] >= delay) mask |= std::uint64_t{1} << i;
    }
    if (mask != 0)
        bump(h.reserved, purge_pages(s, mask) * HEAP_PAGE_SIZE, false);
}

void purge_heap(Heap& h, std::uint64_t now, std::uint64_t delay,
                std::size_t max_spans) noexcept {
    SpanHeader* s = max_spans == UNLIMITED || h.purge_cursor == nullptr
                        ? h.spans
                        : h.purge_cursor;
    for (std::size_t visited = 0; s != nullptr && visited < max_spans;
         ++visited) {
        SpanHeader* next = s->next;
        purge_span_pages(h, s, now, delay);
        s = next;
    }
    h.purge_cursor = s;
}

// Work per call: at most PURGE_SPANS_PER_CALL spans of the heap, and
// CACHE_ENTRIES_PER_CALL purged entries in each of the CACHE_COUNT caches.
void decay_step(Heap& h) noexcept {
    std::uint64_t now = now_ms();
    std::uint64_t delay = decay_ms();
    purge_heap(h, now, delay, PURGE_SPANS_PER_CALL);
    for (Cache& c : g_caches) {
        decay_cache(c, now, delay, CACHE_ENTRIES_PER_CALL);
    }
}

void stamp_free_pages(SpanHeader* s, std::size_t index, std::size_t n,
                      std::uint64_t now) noexcept {
    for (std::size_t i = index; i < index + n; ++i) s->free_since[i] = now;
}

void adopt_cached_span(Heap& h, SpanHeader* s) noexcept {
    std::size_t backed = span_backed(s);
    cache_take_accounting(backed);
    s->owner = &h;
    stamp_free_pages(s, 1, USABLE_PAGES, now_ms());
    list_push(h.spans, s);
    bump(h.reserved, backed, true);
}

void cache_empty_span(Heap& h, SpanHeader* s) noexcept {
    span_unlink(h, s);
    std::size_t backed = span_backed(s);
    bump(h.reserved, backed, false);
    cache_put(0, s, backed);
}

SpanHeader* adopt_cached_run(Heap& h, std::size_t run) noexcept {
    SpanHeader* s = cache_pop(g_caches[1 + std::countr_zero(run >> 20)]);
    if (s == nullptr) return nullptr;
    if (s->purged_mask != 0) {
        reuse_range(page_base(s, 1), s->map_bytes - HEAP_PAGE_SIZE);
        s->purged_mask = 0;
        cache_take_accounting(HEAP_PAGE_SIZE);
    } else {
        cache_take_accounting(s->map_bytes);
    }
    bump(h.reserved, s->map_bytes, true);
    return s;
}

bool run_cacheable(const SpanHeader* s) noexcept {
    return s->kind == SpanKind::Large &&
           s->map_bytes == HEAP_PAGE_SIZE + s->usable;
}

void trim_dormant_heaps(std::uint64_t now) noexcept {
    Heap* held = nullptr;
    for (std::size_t n = page_heap_count(); n > 0; --n) {
        Heap* h = pop_free_heap();
        if (h == nullptr) break;
        h->stack_next.store(held, std::memory_order_relaxed);
        held = h;
    }
    for (Heap* h = held; h != nullptr;
         h = h->stack_next.load(std::memory_order_relaxed)) {
        drain_inbox(*h);
        purge_heap(*h, now, 0, UNLIMITED);
    }
    while (held != nullptr) {
        Heap* next = held->stack_next.load(std::memory_order_relaxed);
        push_free_heap(held);
        held = next;
    }
}

void trim_all(std::uint64_t now) noexcept {
    if (t_heap != nullptr) {
        flush_remote(*t_heap);
        drain_inbox(*t_heap);
        purge_heap(*t_heap, now, 0, UNLIMITED);
    }
    trim_dormant_heaps(now);
    for (Cache& c : g_caches) {
        for (std::atomic<std::uint64_t>* stack : {&c.young, &c.old}) {
            while (SpanHeader* e = stack_pop(*stack)) {
                purge_entry(e);
                stack_push(c.cold, e);
            }
        }
    }
}

void trim_decayed(std::uint64_t now) noexcept {
    std::uint64_t delay = decay_ms();
    if (t_heap != nullptr) {
        flush_remote(*t_heap);
        drain_inbox(*t_heap);
        purge_heap(*t_heap, now, delay, UNLIMITED);
    }
    for (Cache& c : g_caches) decay_cache(c, now, delay, UNLIMITED);
}

SpanHeader* new_span(Heap& h) {
    SpanHeader* s = cache_pop(g_caches[0]);
    if (s != nullptr) {
        adopt_cached_span(h, s);
    } else {
        check_cap(HEAP_SPAN_SIZE);
        void* base = map_aligned(HEAP_SPAN_SIZE);
        if (base == nullptr) throw std::bad_alloc();
        s = new (base) SpanHeader{};
        s->owner = &h;
        s->kind = SpanKind::Standard;
        s->map_bytes = HEAP_SPAN_SIZE;
        s->free_mask = ALL_PAGES_FREE;
        s->free_pages = static_cast<std::uint32_t>(USABLE_PAGES);
        stamp_free_pages(s, 1, USABLE_PAGES, now_ms());
        asan_poison(page_base(s, 1), USABLE_PAGES * HEAP_PAGE_SIZE);
        vg_noaccess(page_base(s, 1), USABLE_PAGES * HEAP_PAGE_SIZE);
        list_push(h.spans, s);
        bump(h.reserved, HEAP_SPAN_SIZE, true);
    }
    decay_step(h);
    return s;
}

struct Run {
    SpanHeader* span;
    std::size_t index;
};

bool find_run(std::uint64_t free_mask, std::size_t n, std::size_t step,
              std::size_t& index) noexcept {
    for (std::size_t i = 0; i + n <= PAGES_PER_SPAN; i += step) {
        std::uint64_t bits = run_bits(i, n);
        if ((free_mask & bits) == bits) {
            index = i;
            return true;
        }
    }
    return false;
}

void take_run(Heap& h, SpanHeader* s, std::size_t index,
              std::size_t n) noexcept {
    s->free_mask &= ~run_bits(index, n);
    s->free_pages -= static_cast<std::uint32_t>(n);
    if (s->free_pages == 0) span_unlink(h, s);
    unpurge_pages(h, s, index, n);
}

void give_run(Heap& h, SpanHeader* s, std::size_t index,
              std::size_t n) noexcept {
    s->free_mask |= run_bits(index, n);
    asan_poison(page_base(s, index), n * HEAP_PAGE_SIZE);
    vg_noaccess(page_base(s, index), n * HEAP_PAGE_SIZE);
    stamp_free_pages(s, index, n, now_ms());
    if (s->free_pages == 0) list_push(h.spans, s);
    s->free_pages += static_cast<std::uint32_t>(n);
    if (s->free_pages == USABLE_PAGES) {
        if (h.spans != s || s->next != nullptr) cache_empty_span(h, s);
        decay_step(h);
    }
}

bool find_in_spans(Heap& h, std::size_t n, std::size_t step,
                   Run& run) noexcept {
    std::size_t index = 0;
    for (SpanHeader* s = h.spans; s != nullptr; s = s->next) {
        if (s->free_pages >= n && find_run(s->free_mask, n, step, index)) {
            take_run(h, s, index, n);
            run = {s, index};
            return true;
        }
    }
    return false;
}

Run acquire_run(Heap& h, std::size_t n, std::size_t step) {
    Run run{};
    if (find_in_spans(h, n, step, run)) return run;
    if (slow_path(h) && find_in_spans(h, n, step, run)) return run;
    SpanHeader* s = new_span(h);
    std::size_t index = 0;
    find_run(s->free_mask, n, step, index);
    take_run(h, s, index, n);
    return {s, index};
}

PageDesc* new_small_page(Heap& h, std::size_t cls) {
    Run r = acquire_run(h, 1, 1);
    PageDesc* d = &r.span->pages[r.index];
    d->free_list = nullptr;
    d->used = 0;
    d->bump = 0;
    d->capacity = SMALL_LAYOUTS[cls].capacity;
    d->ids = page_ids(r.span, r.index, SMALL_LAYOUTS[cls]);
    asan_unpoison(page_base(r.span, r.index), SMALL_LAYOUTS[cls].side);
    vg_define(page_base(r.span, r.index), SMALL_LAYOUTS[cls].side);
    d->cls = static_cast<std::uint8_t>(cls);
    d->kind = PageKind::Small;
    d->listed = true;
    list_push(h.avail[cls], d);
    return d;
}

void* allocate_small(Heap& h, std::size_t size, std::size_t align) {
    std::size_t cls = class_for(size, align);
    PageDesc* d = h.avail[cls];
    if (d == nullptr) [[unlikely]] {
        slow_path(h);
        d = h.avail[cls];
        if (d == nullptr) d = new_small_page(h, cls);
    }
    const SmallLayout& layout = SMALL_LAYOUTS[cls];
    std::size_t block = layout.delta - ATTRIB_BLOCK;
    void* p;
    std::size_t slot;
    if (d->free_list != nullptr) {
        p = d->free_list;
        d->free_list = load_link(p);
        slot = slot_of(layout, p);
    } else {
        SpanHeader* s = span_of(d);
        std::size_t index = static_cast<std::size_t>(d - s->pages);
        slot = d->bump;
        p = page_base(s, index) + layout.side + slot * block;
        ++d->bump;
    }
    ++d->used;
    if (d->free_list == nullptr && d->bump == d->capacity) {
        list_remove(h.avail[cls], d);
        d->listed = false;
    }
    d->ids[slot] = h.attrib.scope;
    attrib_charge(h.attrib, layout.delta);
    asan_unpoison(p, size);
    vg_alloc(p, size, block);
    return p;
}

void free_small_block(Heap& h, SpanHeader* s, std::size_t index,
                      void* p) noexcept {
    PageDesc* d = &s->pages[index];
    std::size_t cls = d->cls;
    store_link(p, d->free_list);
    d->free_list = p;
    --d->used;
    if (!d->listed) {
        list_push(h.avail[cls], d);
        d->listed = true;
    } else if (d->used == 0 && (d->prev != nullptr || d->next != nullptr)) {
        list_remove(h.avail[cls], d);
        d->listed = false;
        d->kind = PageKind::Free;
        give_run(h, s, index, 1);
    }
}

void* allocate_medium(Heap& h, std::size_t size, std::size_t align) {
    std::size_t n =
        (std::max<std::size_t>(size, 1) + HEAP_PAGE_SIZE - 1) / HEAP_PAGE_SIZE;
    Run r = acquire_run(h, n, std::max<std::size_t>(align / HEAP_PAGE_SIZE, 1));
    PageDesc* d = &r.span->pages[r.index];
    d->kind = PageKind::MediumHead;
    d->pages = static_cast<std::uint32_t>(n);
    d->size = size;
    d->pool_id = h.attrib.scope;
    attrib_charge(h.attrib, attrib_delta(n * HEAP_PAGE_SIZE));
    char* p = page_base(r.span, r.index);
    asan_unpoison(p, size);
    vg_alloc(p, size, n * HEAP_PAGE_SIZE);
    return p;
}

void free_medium_run(Heap& h, SpanHeader* s, std::size_t index) noexcept {
    PageDesc* d = &s->pages[index];
    std::size_t n = d->pages;
    d->kind = PageKind::Free;
    give_run(h, s, index, n);
}

void* allocate_mapped(Heap& h, std::size_t size, std::size_t align) {
    std::size_t offset = std::max(HEAP_PAGE_SIZE, align);
    if (size >
        std::numeric_limits<std::size_t>::max() - offset - 2 * HEAP_SPAN_SIZE) {
        throw std::bad_alloc();
    }
    SpanKind kind = size <= LARGE_MAX ? SpanKind::Large : SpanKind::Huge;
    std::size_t run = kind == SpanKind::Large
                          ? std::max(MEDIUM_MAX, std::bit_ceil(size))
                          : round_up(size, HEAP_PAGE_SIZE);
    std::size_t bytes = offset + run;
    SpanHeader* s = nullptr;
    if (kind == SpanKind::Large && offset == HEAP_PAGE_SIZE) {
        s = adopt_cached_run(h, run);
    }
    if (s == nullptr) {
        check_cap(bytes);
        void* base = map_aligned(bytes);
        if (base == nullptr) throw std::bad_alloc();
        s = new (base) SpanHeader{};
        s->map_bytes = bytes;
        bump(h.reserved, bytes, true);
    }
    s->owner = &h;
    s->kind = kind;
    s->size = size;
    s->usable = run;
    s->pool_id = h.attrib.scope;
    attrib_direct(s->pool_id, static_cast<std::ptrdiff_t>(run), 1);
    if (kind == SpanKind::Large) decay_step(h);
    char* p = reinterpret_cast<char*>(s) + offset;
    // A huge run can be gigabytes, so only its slack is poisoned.
    std::size_t from = kind == SpanKind::Large ? 0 : round_up(size, 8);
    asan_poison(p + from, run - from);
    asan_unpoison(p, size);
    vg_alloc(p, size, run);
    return p;
}

// Any thread unmaps a block that owns its mapping and credits its own heap;
// only the sum over all heaps is meaningful.
void free_mapped(Heap& h, SpanHeader* s, [[maybe_unused]] std::size_t size,
                 [[maybe_unused]] bool sized) noexcept {
#ifdef DFTRACER_UTILS_MEMORY_CHECKS
    if (sized) check_size(s->size == size);
#endif
    attrib_direct(s->pool_id, -static_cast<std::ptrdiff_t>(s->usable), -1);
    bump(h.reserved, s->map_bytes, false);
    char* user = reinterpret_cast<char*>(s) + (s->map_bytes - s->usable);
    vg_free(user, s->usable);
    if (run_cacheable(s)) {
        asan_poison(user, s->usable);
        s->purged_mask = 0;
        cache_put(
            1 + static_cast<std::size_t>(std::countr_zero(s->usable >> 20)), s,
            s->map_bytes);
        decay_step(h);
        return;
    }
    // The shadow of an unmapped range stays; another mapping may land there.
    std::size_t from = s->kind == SpanKind::Large ? 0 : round_up(s->size, 8);
    asan_unpoison(user + from, s->usable - from);
    ::munmap(s, s->map_bytes);
}

void check_align(std::size_t align) noexcept {
    if (align == 0 || (align & (align - 1)) != 0) {
        abort_with("dftracer-utils memory: alignment is not a power of two\n");
    }
    if (align > MAX_ALIGN) {
        abort_with("dftracer-utils memory: alignment is above 2 MiB\n");
    }
}

void free_standard(Heap& me, SpanHeader* s, void* p,
                   [[maybe_unused]] std::size_t size,
                   [[maybe_unused]] bool sized) noexcept {
    std::size_t index = (addr(p) - addr(s)) / HEAP_PAGE_SIZE;
    PageDesc* d = &s->pages[index];
    bool small = d->kind == PageKind::Small;
    std::size_t bytes =
        small ? CLASS_SIZES[d->cls] : std::size_t{d->pages} * HEAP_PAGE_SIZE;
    asan_poison(p, bytes);
    vg_free(p, bytes);
#ifdef DFTRACER_UTILS_MEMORY_CHECKS
    if (sized) {
        check_size(small ? small_size_matches(d->cls, size) : d->size == size);
    }
#endif
    if (small) {
        const SmallLayout& layout = SMALL_LAYOUTS[d->cls];
        attrib_credit(me.attrib, d->ids[slot_of(layout, p)], layout.delta);
    } else {
        attrib_credit(me.attrib, d->pool_id, attrib_delta(bytes));
    }
    if (s->owner != &me) {
        remote_free(me, d, p);
    } else if (small) {
        free_small_block(me, s, index, p);
    } else {
        free_medium_run(me, s, index);
    }
}

void release(void* p, std::size_t size, bool sized) noexcept {
    if (p == nullptr) return;
    Heap* me;
    try {
        me = &local_heap();
    } catch (const std::bad_alloc&) {
        abort_with("dftracer-utils memory: cannot get a heap to free into\n");
    }
    SpanHeader* s = span_of(p);
    if (s->kind != SpanKind::Standard) {
        free_mapped(*me, s, size, sized);
    } else {
        free_standard(*me, s, p, size, sized);
    }
}

}  // namespace

void* page_allocate(std::size_t size, std::size_t align) {
    check_align(align);
    Heap& h = local_heap();
    if (size <= SMALL_MAX && align <= SMALL_MAX_ALIGN) {
        return allocate_small(h, size, align);
    }
    if (size <= MEDIUM_MAX && align <= MEDIUM_MAX) {
        return allocate_medium(h, size, align);
    }
    return allocate_mapped(h, size, align);
}

void page_deallocate(void* p) noexcept { release(p, 0, false); }

void page_deallocate(void* p, std::size_t size) noexcept {
    release(p, size, true);
}

void page_trim(Mode mode) noexcept {
    std::uint64_t now = now_ms();
    if (mode == Mode::All) {
        trim_all(now);
    } else {
        trim_decayed(now);
    }
}

void* page_grow_huge(void* p, std::size_t size, std::size_t align) {
    check_align(align);
    SpanHeader* s = span_of(p);
    if (s->kind != SpanKind::Huge || size <= s->size) {
        abort_with(
            "dftracer-utils memory: block cannot grow as a huge block\n");
    }
#ifdef __linux__
    Heap& h = local_heap();
    std::size_t offset = addr(p) - addr(s);
    if (size >
        std::numeric_limits<std::size_t>::max() - offset - 2 * HEAP_SPAN_SIZE) {
        throw std::bad_alloc();
    }
    std::size_t old_bytes = s->map_bytes;
    std::size_t old_usable = s->usable;
    std::size_t old_size = s->size;
    std::size_t bytes = round_up(offset + size, HEAP_PAGE_SIZE);
    check_cap(bytes - old_bytes);
    // The old block ends here and the new one starts after the move; a failed
    // move puts the old one back.
    asan_unpoison(p, old_usable);
    vg_free(p, old_usable);
    auto restore = [&] {
        asan_poison(static_cast<char*>(p) + round_up(old_size, 8),
                    old_usable - round_up(old_size, 8));
        vg_alloc(p, old_size, old_usable);
        vg_define(p, old_size);
    };
    void* moved = ::mremap(s, old_bytes, bytes, 0);
    if (moved == MAP_FAILED) {
        void* target = map_aligned(bytes);
        if (target == nullptr) {
            restore();
            throw std::bad_alloc();
        }
        moved = ::mremap(s, old_bytes, old_bytes, MREMAP_MAYMOVE | MREMAP_FIXED,
                         target);
        if (moved == MAP_FAILED) {
            ::munmap(target, bytes);
            restore();
            throw std::bad_alloc();
        }
    }
    auto* grown = static_cast<SpanHeader*>(moved);
    grown->map_bytes = bytes;
    grown->size = size;
    grown->usable = bytes - offset;
    asan_poison(reinterpret_cast<char*>(moved) + offset + round_up(size, 8),
                grown->usable - round_up(size, 8));
    bump(h.reserved, bytes - old_bytes, true);
    attrib_direct(grown->pool_id,
                  static_cast<std::ptrdiff_t>(grown->usable - old_usable), 0);
    char* user = reinterpret_cast<char*>(moved) + offset;
    vg_alloc(user, size, grown->usable);
    vg_define(user, old_size);
    return user;
#else
    AttribCache& attrib = local_heap().attrib;
    std::uint16_t scope = attrib.scope;
    attrib_set_scope(attrib, s->pool_id);
    void* grown = page_allocate(size, align);
    attrib_set_scope(attrib, scope);
    std::memcpy(grown, p, s->size);
    page_deallocate(p);
    return grown;
#endif
}

std::size_t page_reserved() noexcept {
    std::ptrdiff_t reserved = 0;
    for (Heap* h = g_all_heaps.load(std::memory_order_acquire); h != nullptr;
         h = h->all_next) {
        reserved += h->reserved.load(std::memory_order_relaxed);
    }
    reserved += g_cache_reserved.load(std::memory_order_relaxed);
    return static_cast<std::size_t>(std::max<std::ptrdiff_t>(reserved, 0));
}

PageStats page_stats() noexcept {
    std::ptrdiff_t live = attrib_live_total();
    return {static_cast<std::size_t>(std::max<std::ptrdiff_t>(live, 0)),
            page_reserved()};
}

std::size_t page_heap_count() noexcept {
    std::size_t n = 0;
    for (Heap* h = g_all_heaps.load(std::memory_order_acquire); h != nullptr;
         h = h->all_next) {
        ++n;
    }
    return n;
}

void page_flush_remote() noexcept {
    if (t_heap != nullptr) flush_remote(*t_heap);
}

bool page_checks_enabled() noexcept {
#ifdef DFTRACER_UTILS_MEMORY_CHECKS
    return true;
#else
    return false;
#endif
}

AttribCache& page_attrib() { return local_heap().attrib; }

std::size_t page_class_count() noexcept { return CLASS_COUNT; }

std::size_t page_class_size(std::size_t cls) noexcept {
    return CLASS_SIZES[cls];
}

std::size_t page_class_for(std::size_t size, std::size_t align) noexcept {
    return class_for(size, align);
}

}  // namespace dftracer::utils::memory
