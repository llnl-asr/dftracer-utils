#include <dftracer/utils/index/store/engine/io_fault.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace dftracer::utils::index::store::engine::io_fault {

static_assert(KINDS == static_cast<std::size_t>(WriteKind::Page) + 1,
              "KINDS must name every WriteKind");

namespace {

/// The recorder: one descriptor, one log, one lock, plus the counters of the
/// counting mode, which cover every descriptor.
///
/// Both modes share one word, so a write or sync with neither on returns after
/// one relaxed load: bits 0..31 are the recording descriptor plus one (zero
/// means none) and bit 63 is set while counting.
struct Recorder {
    static constexpr std::uint64_t COUNTING = std::uint64_t{1} << 63;
    static constexpr std::uint64_t FD_MASK = 0xFFFFFFFFULL;

    std::atomic<std::uint64_t> mode{0};
    std::atomic<std::uint64_t> writes[KINDS]{};
    std::atomic<std::uint64_t> bytes[KINDS]{};
    std::atomic<std::uint64_t> max_bytes[KINDS]{};
    std::atomic<std::uint64_t> syncs{0};
    std::mutex mutex;
    dstd::Vector<Event> events;
};

/// Never destroyed: the log outlives the memory manager its allocator charges,
/// so a recorder left over at exit must not free through it.
Recorder& recorder() {
    static Recorder* instance = new Recorder();
    return *instance;
}

/// True while `mode` names `fd` as the recording descriptor.
bool records(std::uint64_t mode, int fd) {
    return fd >= 0 &&
           (mode & Recorder::FD_MASK) == static_cast<std::uint64_t>(fd) + 1U;
}

void count_write(Recorder& log, WriteKind kind, std::uint64_t length) {
    const std::size_t at = static_cast<std::size_t>(kind);
    log.writes[at].fetch_add(1, std::memory_order_relaxed);
    log.bytes[at].fetch_add(length, std::memory_order_relaxed);
    std::uint64_t longest = log.max_bytes[at].load(std::memory_order_relaxed);
    while (length > longest &&
           !log.max_bytes[at].compare_exchange_weak(
               longest, length, std::memory_order_relaxed)) {
    }
}

}  // namespace

void start(int fd) {
    Recorder& log = recorder();
    std::lock_guard<std::mutex> lock(log.mutex);
    log.events.clear();
    const std::uint64_t counting =
        log.mode.load(std::memory_order_relaxed) & Recorder::COUNTING;
    const std::uint64_t recording =
        fd < 0 ? 0 : static_cast<std::uint64_t>(fd) + 1U;
    log.mode.store(counting | recording, std::memory_order_release);
}

dstd::Vector<Event> stop() {
    Recorder& log = recorder();
    std::lock_guard<std::mutex> lock(log.mutex);
    log.mode.store(
        log.mode.load(std::memory_order_relaxed) & Recorder::COUNTING,
        std::memory_order_release);
    dstd::Vector<Event> out;
    out.swap(log.events);
    log.events.clear();
    return out;
}

void start_counting() {
    Recorder& log = recorder();
    for (std::size_t at = 0; at < KINDS; ++at) {
        log.writes[at].store(0, std::memory_order_relaxed);
        log.bytes[at].store(0, std::memory_order_relaxed);
        log.max_bytes[at].store(0, std::memory_order_relaxed);
    }
    log.syncs.store(0, std::memory_order_relaxed);
    log.mode.store(
        log.mode.load(std::memory_order_relaxed) | Recorder::COUNTING,
        std::memory_order_release);
}

Counts stop_counting() {
    Recorder& log = recorder();
    log.mode.store(log.mode.load(std::memory_order_relaxed) & Recorder::FD_MASK,
                   std::memory_order_release);
    Counts out;
    for (std::size_t at = 0; at < KINDS; ++at) {
        out.writes[at] = log.writes[at].load(std::memory_order_relaxed);
        out.bytes[at] = log.bytes[at].load(std::memory_order_relaxed);
        out.max_bytes[at] = log.max_bytes[at].load(std::memory_order_relaxed);
    }
    out.syncs = log.syncs.load(std::memory_order_relaxed);
    return out;
}

void on_write(int fd, WriteKind kind, std::uint64_t offset,
              const std::uint8_t* data, std::uint64_t length) {
    Recorder& log = recorder();
    const std::uint64_t mode = log.mode.load(std::memory_order_relaxed);
    if (mode == 0) return;
    if ((mode & Recorder::COUNTING) != 0) count_write(log, kind, length);
    if (!records(mode, fd)) return;
    Event event;
    event.type = Event::Type::Write;
    event.kind = kind;
    event.offset = offset;
    event.length = length;
    if (kind != WriteKind::Page && data != nullptr && length > 0) {
        event.bytes.assign(data, data + length);
    }
    std::lock_guard<std::mutex> lock(log.mutex);
    log.events.push_back(std::move(event));
}

void on_sync(int fd) {
    Recorder& log = recorder();
    const std::uint64_t mode = log.mode.load(std::memory_order_relaxed);
    if (mode == 0) return;
    if ((mode & Recorder::COUNTING) != 0) {
        log.syncs.fetch_add(1, std::memory_order_relaxed);
    }
    if (!records(mode, fd)) return;
    Event event;
    event.type = Event::Type::Sync;
    std::lock_guard<std::mutex> lock(log.mutex);
    log.events.push_back(std::move(event));
}

}  // namespace dftracer::utils::index::store::engine::io_fault
