#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_SPILL_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_SPILL_H

#include <dftracer/utils/dataframe/agg.h>        // AggState, AggStatePtr
#include <dftracer/utils/dataframe/lazyframe.h>  // Cursor, Morsel
#include <dftracer/utils/dataframe/series.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// On-disk spill for bounded-memory pipeline breakers. Serializes FLAT columns
// to a temp file and reads them back through the same Cursor pull interface, so
// a spilled run and a live input are interchangeable. Same-machine, native
// endianness (temp files never move between hosts).
namespace dftracer::utils::dataframe::spill {

/// Append a Series to `out` (materialized FLAT first). Supports fixed-width,
/// String and Binary columns; throws std::invalid_argument on List/Struct.
void put_series(std::string& out, const Series& s);

/// Read one Series written by put_series, advancing `p` toward `end`.
Series get_series(const std::uint8_t*& p, const std::uint8_t* end);

/// A self-cleaning temp directory holding one query's spill runs.
class Dir {
   public:
    Dir();
    ~Dir();
    Dir(const Dir&) = delete;
    Dir& operator=(const Dir&) = delete;
    /// Path of run file `id` within this directory.
    std::string run_path(int id) const;

   private:
    std::string dir_;
};

/// Appends morsels (columns + row count) to one run file.
class Writer {
   public:
    explicit Writer(const std::string& path);
    void write(const std::vector<Series>& cols, std::int64_t rows);
    void close();

   private:
    std::ofstream os_;
};

/// Reads morsels back from a run file. The max_rows hint is ignored: chunks
/// come back exactly as written.
class Reader : public Cursor {
   public:
    explicit Reader(const std::string& path);
    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override;

   private:
    std::ifstream is_;
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
    AggStatePtr cur_;
    bool valid_ = false;
};

/// A replayable copy of a morsel stream: keeps morsels in memory up to `budget`
/// bytes, spilling the overflow to a temp run. Lets a two-pass sink read its
/// upstream once (feed via add) and re-scan it (reader) with bounded memory,
/// regardless of how expensive the source is to reproduce.
class Spool {
   public:
    explicit Spool(std::uint64_t budget);
    ~Spool();
    Spool(const Spool&) = delete;
    Spool& operator=(const Spool&) = delete;
    /// Take ownership of one morsel (it may go to memory or disk).
    void add(std::vector<Series> columns, std::int64_t rows);
    /// A fresh cursor replaying every added morsel, in order. Call after all
    /// add()s; may be called more than once.
    std::unique_ptr<Cursor> reader();

   private:
    std::uint64_t budget_;
    std::size_t bytes_ = 0;
    bool spilling_ = false;
    std::vector<Morsel> mem_;
    std::unique_ptr<Dir> dir_;
    std::unique_ptr<Writer> writer_;
};

}  // namespace dftracer::utils::dataframe::spill

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_SPILL_H
