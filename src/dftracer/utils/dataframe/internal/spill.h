#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_SPILL_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_SPILL_H

#include <dftracer/utils/core/common/spill_dir.h>
#include <dftracer/utils/core/common/spill_file.h>
#include <dftracer/utils/dataframe/agg.h>        // AggState, AggStatePtr
#include <dftracer/utils/dataframe/lazyframe.h>  // Cursor, Morsel
#include <dftracer/utils/dataframe/series.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

// On-disk spill for bounded-memory pipeline breakers. Serializes FLAT columns
// to a temp file and reads them back through the same Cursor pull interface, so
// a spilled run and a live input are interchangeable. Same-machine, native
// endianness (temp files never move between hosts).
namespace dftracer::utils::dataframe::spill {

/// Append a Series to `out` (materialized FLAT first). Supports fixed-width,
/// String and Binary columns and list, large list, fixed-size list, map and
/// struct columns of those, and keeps a column's JSON flag, time unit and time
/// zone.
void put_series(std::string& out, const Series& s);

/// Read one Series written by put_series, advancing `p` toward `end`.
Series get_series(const std::uint8_t*& p, const std::uint8_t* end);

/// Approximate in-memory bytes of FLAT columns.
std::size_t columns_bytes(const std::vector<Series>& cols);
/// Bytes of the buffers of `cols` not already in `seen`, which it records:
/// the memory a frame part adds when parts share buffers. The data buffers a
/// view column points into are not counted.
std::size_t new_buffer_bytes(const std::vector<Series>& cols,
                             std::unordered_set<const void*>& seen);

/// One collect's spill file. The file is unlinked as soon as it is created, so
/// nothing stays in the spill directory; the open descriptor and the mappings
/// keep its data. Each spilled part is a page-aligned region mapped read-only.
class PartFile {
   public:
    /// Throws DFTUtilsException when spill_dir() fails.
    PartFile();
    ~PartFile();
    PartFile(const PartFile&) = delete;
    PartFile& operator=(const PartFile&) = delete;
    /// `cols` as FLAT columns whose buffers are windows over a mapped region
    /// of the file; a region is unmapped when the last window is released.
    /// Nested columns are returned as shared copies and stay in memory.
    /// Throws std::runtime_error naming the spill directory on a write or
    /// mapping failure.
    std::vector<Series> append(const std::vector<Series>& cols);

   private:
    std::unique_ptr<SpillFile> file_;
};

/// A self-cleaning spill directory holding one query's spill runs.
class Dir {
   public:
    Dir();
    ~Dir();
    Dir(const Dir&) = delete;
    Dir& operator=(const Dir&) = delete;
    /// Path of run file `id` within this directory.
    std::string run_path(int id) const;
    /// A run id no other caller of this directory has taken. Thread-safe.
    int next_run();

   private:
    ScopedSpillSubdir dir_;
    std::atomic<int> runs_{0};
};

/// Appends morsels (columns + row count) to one run file.
class Writer {
   public:
    explicit Writer(const std::string& path);
    Writer(Writer&&) noexcept;
    Writer& operator=(Writer&&) noexcept;
    ~Writer();
    void write(const std::vector<Series>& cols, std::int64_t rows);
    void close();
    /// The bytes written so far: where the next morsel starts.
    std::uint64_t bytes() const noexcept { return written_; }

   private:
    struct Codec;
    std::ofstream os_;
    std::uint64_t written_ = 0;
    std::string blob_;
    std::unique_ptr<Codec> codec_;
};

/// Writes `cols` (`rows` rows each) to `w` in morsels of at most
/// `rows_per_morsel` rows (at least 1). Returns the bytes of the largest morsel
/// written.
std::uint64_t write_run(Writer& w, const std::vector<Series>& cols,
                        std::int64_t rows, std::int64_t rows_per_morsel);

/// The run files of a hash fan-out: `parts` files of one Dir, a Writer on each,
/// and the bytes and rows written to each, so the owner can split a partition
/// again that came out too big.
class HashPartitions {
   public:
    HashPartitions(Dir& dir, std::size_t parts);
    std::size_t size() const noexcept { return paths_.size(); }
    /// Appends one morsel to partition `p`.
    void write(std::size_t p, const std::vector<Series>& cols,
               std::int64_t rows);
    /// Flushes and closes every partition's file.
    void close();
    const std::string& path(std::size_t p) const { return paths_[p]; }
    std::uint64_t bytes(std::size_t p) const { return bytes_[p]; }
    std::int64_t rows(std::size_t p) const { return rows_[p]; }

   private:
    std::vector<std::string> paths_;
    std::vector<Writer> writers_;
    std::vector<std::uint64_t> bytes_;
    std::vector<std::int64_t> rows_;
};

/// Reads morsels back from a run file. The max_rows hint is ignored: chunks
/// come back exactly as written.
class Reader : public Cursor {
   public:
    explicit Reader(const std::string& path);
    ~Reader() override;
    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override;
    /// Moves to `pos`, the start of a morsel (a value of Writer::bytes()), so
    /// the next call of next() reads that morsel.
    void seek(std::uint64_t pos);

   private:
    struct Codec;
    std::ifstream is_;
    std::uint64_t size_ = 0;
    std::unique_ptr<char[]> raw_;
    std::size_t raw_cap_ = 0;
    std::unique_ptr<char[]> stored_;
    std::size_t stored_cap_ = 0;
    std::unique_ptr<Codec> codec_;
};

/// One aggregation run file: the groups of an AggState sorted by composite
/// key, each `agg_extract_group` + `agg_serialize` blob written with a u32
/// length prefix. Both spill drivers (the lazy group-by breaker and the
/// long-lived AggSpiller) read and write this one format, so it cannot drift
/// between them. Throws std::runtime_error if `path` cannot be opened - a
/// silently empty run loses the groups it was meant to hold.
void write_agg_run(AggState& state, const std::string& path);

/// Streams one run's single-group states back in key order.
class AggRunReader {
   public:
    explicit AggRunReader(const std::string& path);
    bool valid() const { return valid_; }
    const AggState& state() const { return *cur_; }
    void advance();

   private:
    std::ifstream is_;
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
    explicit Spool(std::uint64_t budget, std::shared_ptr<Dir> dir = nullptr);
    ~Spool();
    Spool(const Spool&) = delete;
    Spool& operator=(const Spool&) = delete;
    /// Take ownership of one morsel (it may go to memory or disk).
    void add(std::vector<Series> columns, std::int64_t rows);
    /// A fresh cursor replaying every added morsel, in order. Call after all
    /// add()s; may be called more than once.
    std::unique_ptr<Cursor> reader();
    /// A cursor replaying every added morsel from the last to the first; the
    /// rows of one morsel stay in their order. The same rules as reader().
    std::unique_ptr<Cursor> reverse_reader();
    /// Whether the overflow went to disk.
    bool spilled() const noexcept { return spilling_; }
    /// The bytes of every morsel added, in memory or on disk.
    std::uint64_t total_bytes() const noexcept { return total_bytes_; }

   private:
    std::uint64_t budget_;
    std::size_t bytes_ = 0;
    std::uint64_t total_bytes_ = 0;
    bool spilling_ = false;
    bool read_ = false;  // reader() ran: the spool no longer takes morsels
    std::vector<Morsel> mem_;
    std::vector<std::uint64_t> offsets_;  // where each spilled morsel starts
    std::shared_ptr<Dir> dir_;
    std::string run_;
    std::unique_ptr<Writer> writer_;
};

}  // namespace dftracer::utils::dataframe::spill

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_SPILL_H
