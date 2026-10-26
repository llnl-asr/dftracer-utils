#ifndef DFTRACER_UTILS_INDEX_STORE_ENGINE_IO_FAULT_H
#define DFTRACER_UTILS_INDEX_STORE_ENGINE_IO_FAULT_H

#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::index::store::engine::io_fault {

/// Which write of a data file an event is. A Page goes through the I/O backend,
/// so only the range it reserved is recorded and its bytes come from the
/// finished file; every other write goes through pwrite_all and carries its
/// bytes, because the header is rewritten in place.
enum class WriteKind : std::uint8_t {
    Header,
    Record,
    Big,
    Footer,
    Trailer,
    Page
};

/// The number of WriteKind values, the length of a Counts array.
inline constexpr std::size_t KINDS = 6;

/// What every descriptor wrote and synced since start_counting(), indexed by
/// WriteKind. A Page counts the range it reserved, which is what it writes.
/// `max_bytes` is the longest single write of the kind.
struct Counts {
    std::uint64_t writes[KINDS] = {};
    std::uint64_t bytes[KINDS] = {};
    std::uint64_t max_bytes[KINDS] = {};
    std::uint64_t syncs = 0;
};

/// One write or one sync a writer issued, in issue order.
struct Event {
    enum class Type : std::uint8_t { Write, Sync };

    Type type = Type::Write;
    WriteKind kind = WriteKind::Header;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    dstd::Vector<std::uint8_t> bytes;
};

/// Test-only recorder of the writes and syncs one descriptor issues. It is off
/// until start() and off for every other descriptor, so on_write and on_sync
/// cost one relaxed load and nothing else while neither it nor counting is on.
void start(int fd);
/// Ends recording and returns the log in issue order.
dstd::Vector<Event> stop();

/// Turns counting on for every descriptor and clears the counts. Counting and
/// recording are independent; a call adds to the counts with one relaxed
/// atomic per field while counting, and returns after one relaxed load while
/// neither mode is on.
void start_counting();
/// Ends counting and returns the counts it took.
Counts stop_counting();

void on_write(int fd, WriteKind kind, std::uint64_t offset,
              const std::uint8_t* data, std::uint64_t length);
void on_sync(int fd);

}  // namespace dftracer::utils::index::store::engine::io_fault

#endif  // DFTRACER_UTILS_INDEX_STORE_ENGINE_IO_FAULT_H
