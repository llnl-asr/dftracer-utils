// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_SPILL_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_SPILL_H

#include <dftracer/utils/common/spill_dir.h>
#include <dftracer/utils/common/spill_file.h>
#include <dftracer/utils/dataframe/agg.h>        // AggState, AggStatePtr
#include <dftracer/utils/dataframe/lazyframe.h>  // Cursor, Morsel
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/ptr_hash.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>

// On-disk spill for bounded-memory pipeline breakers. Serializes FLAT columns
// to a temp file and reads them back through the same Cursor pull interface, so
// a spilled run and a live input are interchangeable. Same-machine, native
// endianness (temp files never move between hosts).
namespace dftracer::utils::dataframe::spill {

/// Append a Series to `out` (materialized FLAT first). Supports fixed-width,
/// String and Binary columns and list, large list, fixed-size list, map and
/// struct columns of those, and keeps a column's JSON flag, time unit and time
/// zone. An InvalidArgument error for a list child that cannot be gathered.
dstd::Result<void> put_series(dstd::String& out, const Series& s);

/// Read one Series written by put_series, advancing `p` toward `end`. A Parse
/// error when the bytes end before the column does.
dstd::Result<Series> get_series(const std::uint8_t*& p,
                                const std::uint8_t* end);

/// Approximate in-memory bytes of FLAT columns.
std::size_t columns_bytes(const dstd::Vector<Series>& cols);
/// Bytes of the buffers of `cols` not already in `seen`, which it records:
/// the memory a frame part adds when parts share buffers. The data buffers a
/// view column points into are not counted.
std::size_t new_buffer_bytes(const dstd::Vector<Series>& cols,
                             dstd::HashSet<const void*, dstd::PtrHash>& seen);

/// One collect's spill file. The file is unlinked as soon as it is created, so
/// nothing stays in the spill directory; the open descriptor and the mappings
/// keep its data. Each spilled part is a page-aligned region mapped read-only.
class PartFile {
   public:
    /// The Io error of the spill directory or file when it cannot be made.
    static dstd::Result<dstd::UniquePtr<PartFile>> create();
    /// Use @ref create.
    explicit PartFile(dstd::UniquePtr<common::SpillFile> file);
    ~PartFile();
    PartFile(const PartFile&) = delete;
    PartFile& operator=(const PartFile&) = delete;
    /// `cols` as FLAT columns whose buffers are windows over a mapped region
    /// of the file; a region is unmapped when the last window is released.
    /// Nested columns are returned as shared copies and stay in memory.
    /// An Io error naming the spill directory on a write or mapping failure.
    dstd::Result<dstd::Vector<Series>> append(const dstd::Vector<Series>& cols);

   private:
    dstd::UniquePtr<common::SpillFile> file_;
};

/// A self-cleaning spill directory holding one query's spill runs.
class Dir {
   public:
    /// The Io error of the spill directory when it cannot be made.
    static dstd::Result<dstd::SharedPtr<Dir>> create();
    /// Use @ref create.
    explicit Dir(common::ScopedSpillSubdir dir);
    ~Dir();
    Dir(const Dir&) = delete;
    Dir& operator=(const Dir&) = delete;
    /// Path of run file `id` within this directory.
    dstd::String run_path(int id) const;
    /// A run id no other caller of this directory has taken. Thread-safe.
    int next_run();
    common::SpillCharge& charge() const noexcept { return dir_.charge(); }
    /// Delete the file `path` in this directory and release its bytes from
    /// the charge.
    void remove_run(const dstd::String& path) const noexcept;

   private:
    common::ScopedSpillSubdir dir_;
    std::atomic<int> runs_{0};
};

/// Appends morsels (columns + row count) to one run file, charging the bytes
/// to the ScopedSpillSubdir that holds it.
class Writer {
   public:
    /// An Io error when `path` cannot be opened.
    static dstd::Result<Writer> open(const dstd::String& path);
    Writer(Writer&&) noexcept;
    Writer& operator=(Writer&&) noexcept;
    ~Writer();
    /// An Io error when the file cannot be written (disk full).
    dstd::Result<void> write(const dstd::Vector<Series>& cols,
                             std::int64_t rows);
    /// An Io error when the file cannot be finished.
    dstd::Result<void> close();
    /// The bytes written so far: where the next morsel starts.
    std::uint64_t bytes() const noexcept { return written_; }

   private:
    struct Codec;
    Writer(dstd::ScopedFile os, dstd::SharedPtr<common::SpillCharge> charge);
    dstd::ScopedFile os_;
    dstd::SharedPtr<common::SpillCharge> charge_;
    std::uint64_t written_ = 0;
    dstd::String blob_;
    dstd::UniquePtr<Codec> codec_;
};

/// Writes `cols` (`rows` rows each) to `w` in morsels of at most
/// `rows_per_morsel` rows (at least 1). Returns the bytes of the largest morsel
/// written, or the Io error of the write.
dstd::Result<std::uint64_t> write_run(Writer& w,
                                      const dstd::Vector<Series>& cols,
                                      std::int64_t rows,
                                      std::int64_t rows_per_morsel);

/// The run files of a hash fan-out: `parts` files of one Dir, a Writer on each,
/// and the bytes and rows written to each, so the owner can split a partition
/// again that came out too big.
class HashPartitions {
   public:
    /// An Io error when a partition file cannot be opened.
    static dstd::Result<HashPartitions> make(Dir& dir, std::size_t parts);
    std::size_t size() const noexcept { return paths_.size(); }
    /// Appends one morsel to partition `p`.
    dstd::Result<void> write(std::size_t p, const dstd::Vector<Series>& cols,
                             std::int64_t rows);
    /// Flushes and closes every partition's file.
    dstd::Result<void> close();
    const dstd::String& path(std::size_t p) const { return paths_[p]; }
    std::uint64_t bytes(std::size_t p) const { return bytes_[p]; }
    std::int64_t rows(std::size_t p) const { return rows_[p]; }

   private:
    explicit HashPartitions(std::size_t parts);
    dstd::Vector<dstd::String> paths_;
    dstd::Vector<Writer> writers_;
    dstd::Vector<std::uint64_t> bytes_;
    dstd::Vector<std::int64_t> rows_;
};

/// Reads morsels back from a run file. The max_rows hint is ignored: chunks
/// come back exactly as written.
class Reader : public Cursor {
   public:
    /// An Io error when `path` cannot be opened or sized.
    static dstd::Result<dstd::UniquePtr<Reader>> open(const dstd::String& path);
    /// Use @ref open.
    Reader(dstd::ScopedFile is, std::uint64_t size);
    ~Reader() override;
    /// The next morsel, no morsel at the end, or an Io error for a short read
    /// or a Parse one for a corrupt run.
    coro::CoroTask<MorselResult> next(std::int64_t max_rows) override;
    /// Moves to `pos`, the start of a morsel (a value of Writer::bytes()), so
    /// the next call of next() reads that morsel. An Io error when the file
    /// cannot seek.
    dstd::Result<void> seek(std::uint64_t pos);

   private:
    struct Codec;
    dstd::ScopedFile is_;
    std::uint64_t size_ = 0;
    dstd::UniquePtr<char[]> raw_;
    std::size_t raw_cap_ = 0;
    dstd::UniquePtr<char[]> stored_;
    std::size_t stored_cap_ = 0;
    dstd::UniquePtr<Codec> codec_;
};

/// One aggregation run file: the groups of an AggState sorted by composite
/// key, each `agg_extract_group` + `agg_serialize` blob written with a u32
/// length prefix. Both spill drivers (the lazy group-by breaker and the
/// long-lived AggSpiller) read and write this one format, so it cannot drift
/// between them. An Io error if `path` cannot be opened or written - a
/// silently empty run loses the groups it was meant to hold.
dstd::Result<void> write_agg_run(AggState& state, const dstd::String& path);

/// Streams one run's single-group states back in key order.
class AggRunReader {
   public:
    /// Opens `path` and reads its first group. An Io error when the file
    /// cannot be opened, a Parse one for a corrupt run.
    static dstd::Result<dstd::UniquePtr<AggRunReader>> open(
        const dstd::String& path);
    /// Use @ref open.
    AggRunReader(dstd::ScopedFile is, std::uint64_t size);
    bool valid() const { return valid_; }
    const AggState& state() const { return *cur_; }
    /// Reads the next group; the errors of @ref open.
    dstd::Result<void> advance();

   private:
    dstd::ScopedFile is_;
    std::uint64_t size_ = 0;
    std::uint64_t pos_ = 0;
    AggStatePtr cur_;
    bool valid_ = false;
};

/// A replayable copy of a morsel stream: keeps morsels in memory up to `budget`
/// bytes, spilling the overflow to a temp run. Lets a two-pass sink read its
/// upstream once (feed via add) and re-scan it (reader) with bounded memory,
/// regardless of how expensive the source is to reproduce.
class Spool {
   public:
    /// With `dir`, the run file lives in that shared directory under its own
    /// run id, so many spools can share one directory.
    explicit Spool(std::uint64_t budget, dstd::SharedPtr<Dir> dir = nullptr);
    ~Spool();
    Spool(const Spool&) = delete;
    Spool& operator=(const Spool&) = delete;
    /// Take ownership of one morsel (it may go to memory or disk). An Io error
    /// when the run file cannot be made or written. Adding after a reader is
    /// fatal.
    dstd::Result<void> add(dstd::Vector<Series> columns, std::int64_t rows);
    /// A fresh cursor replaying every added morsel, in order. Call after all
    /// add()s; may be called more than once. An Io error when the run cannot
    /// be closed or opened.
    dstd::Result<dstd::UniquePtr<Cursor>> reader();
    /// A cursor replaying every added morsel from the last to the first; the
    /// rows of one morsel stay in their order. The same rules as reader().
    dstd::Result<dstd::UniquePtr<Cursor>> reverse_reader();
    /// Move the morsels held in memory to disk, and every later one with them,
    /// and return the bytes freed. Does nothing once the spool writes to disk
    /// or after a reader. The Io errors of @ref add.
    dstd::Result<std::uint64_t> spill();
    std::uint64_t memory_bytes() const noexcept { return bytes_; }
    /// Whether the overflow went to disk.
    bool spilled() const noexcept { return spilling_; }
    /// The bytes of every morsel added, in memory or on disk.
    std::uint64_t total_bytes() const noexcept { return total_bytes_; }

   private:
    dstd::Result<void> open_run();
    // Closes the run and opens a reader on it, or none when nothing spilled.
    dstd::Result<dstd::UniquePtr<Reader>> open_disk();

    std::uint64_t budget_;
    std::size_t bytes_ = 0;
    std::uint64_t total_bytes_ = 0;
    bool spilling_ = false;
    bool read_ = false;  // reader() ran: the spool no longer takes morsels
    dstd::Vector<Morsel> mem_;
    dstd::Vector<std::uint64_t> offsets_;  // where each spilled morsel starts
    dstd::SharedPtr<Dir> dir_;
    dstd::String run_;
    dstd::UniquePtr<Writer> writer_;
};

}  // namespace dftracer::utils::dataframe::spill

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_SPILL_H
