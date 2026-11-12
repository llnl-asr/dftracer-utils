// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_STORE_IO_FAULT_H
#define DFTRACER_UTILS_STORE_IO_FAULT_H

#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::store::engine::io_fault {

/// Which write of a data file an event is. An Extent goes through the I/O
/// backend, so only the range it reserved is recorded and its bytes come from
/// the finished file. Every other write goes through pwrite_all and carries its
/// bytes, because the header is rewritten in place.
enum class WriteKind : std::uint8_t {
    Header,
    Record,
    Big,
    Footer,
    Trailer,
    Extent
};

inline constexpr std::size_t KINDS = 6;

/// What every descriptor wrote and synced since start_counting(), indexed by
/// WriteKind. An Extent counts the range it reserved, which is what it writes.
/// `max_bytes` is the longest single write of the kind.
struct Counts {
    std::uint64_t writes[KINDS] = {};
    std::uint64_t bytes[KINDS] = {};
    std::uint64_t max_bytes[KINDS] = {};
    std::uint64_t syncs = 0;
};

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
dstd::Vector<Event> stop();

/// Turns counting on for every descriptor and clears the counts. Counting and
/// recording are independent. A call adds to the counts with one relaxed
/// atomic per field while counting, and returns after one relaxed load while
/// neither mode is on.
void start_counting();
Counts stop_counting();

void on_write(int fd, WriteKind kind, std::uint64_t offset,
              const std::uint8_t* data, std::uint64_t length);
void on_sync(int fd);

}  // namespace dftracer::utils::store::engine::io_fault

#endif  // DFTRACER_UTILS_STORE_IO_FAULT_H
