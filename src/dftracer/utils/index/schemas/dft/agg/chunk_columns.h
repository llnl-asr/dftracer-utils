#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_CHUNK_COLUMNS_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_CHUNK_COLUMNS_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <limits>

// The column block of one (file, chunk) of the aggregation tier: the rows of
// the chunk's records that the partials keep, in file order, as exact columns.
// A block holds the core columns of every row (ts, dur, pid, tid, phase, cat,
// name, host) and one numeric column per (arg path, value kind) that the
// chunk's records hold. Strings are indexes into a block-local table of the
// index dictionary ids the block names, so a sharded merge remaps a block by
// rewriting that table.
namespace dftracer::utils::index::schemas::dft::agg {

/// The kind of the values of one numeric column: the variant kind of the
/// FoldEvent args it holds.
enum class ColumnKind : std::uint8_t { Int64 = 0, Uint64 = 1, Double = 2 };

/// An encoder with no byte budget drops no column.
inline constexpr std::size_t NO_COLUMN_BUDGET =
    std::numeric_limits<std::size_t>::max();

/// No dictionary id, so a block cannot name it.
inline constexpr std::uint32_t NO_DICTIONARY_ID =
    std::numeric_limits<std::uint32_t>::max();

/// One numeric column of a block: the rows of the block that hold the arg
/// path, ascending, and the value of each one. `bits` holds the value in the
/// form `kind` names: the two's complement of an Int64, a Uint64, or the IEEE
/// bits of a Double.
struct ChunkColumn {
    std::uint32_t path = 0;
    ColumnKind kind = ColumnKind::Double;
    dstd::Vector<std::uint64_t> rows;
    dstd::Vector<std::uint64_t> bits;

    std::int64_t int64_at(std::size_t k) const {
        return static_cast<std::int64_t>(bits[k]);
    }
    std::uint64_t uint64_at(std::size_t k) const { return bits[k]; }
    double double_at(std::size_t k) const { return bits_double(bits[k]); }
};

/// The decoded (or being built) column block of one chunk. Row r of the block
/// is the r-th record of the chunk the partials keep, in file order; the
/// column of row r is `cat[r]`, an index into `ids`.
struct ChunkColumns {
    std::uint64_t chunk = 0;
    std::size_t rows = 0;
    /// The dictionary ids the block names: the texts of cat, name and host and
    /// the arg paths of its columns, in the order the block first used them.
    dstd::Vector<std::uint32_t> ids;
    /// The text of each id of `ids`, resolved through the file's dictionary
    /// entries by the reader; empty when the file holds none.
    dstd::Vector<std::string> texts;
    /// The category of row r as its record spells it, in its own case, not the
    /// lower case the partials group by.
    dstd::Vector<std::uint32_t> cat;
    dstd::Vector<std::uint32_t> name;
    dstd::Vector<std::uint32_t> host;
    dstd::Vector<std::uint64_t> ts;
    dstd::Vector<std::uint64_t> dur;
    /// 1 where the record carried a duration, 0 where it carried none.
    dstd::Vector<std::uint8_t> has_dur;
    dstd::Vector<std::uint64_t> pid;
    dstd::Vector<std::uint64_t> tid;
    dstd::Vector<std::uint8_t> phase;
    /// Every column of the block; a builder keeps unused slots past its live
    /// columns, which encode_chunk_columns is told the count of.
    dstd::Vector<ChunkColumn> columns;
    /// The arg paths whose columns the byte budget dropped, as indexes into
    /// `ids`; the block holds no column of theirs, so a reader parses the
    /// chunk to read them.
    dstd::Vector<std::uint32_t> dropped;

    /// Empties the block, keeping the storage of its columns for reuse.
    void clear();
};

/// Accumulates one chunk's block event by event, keeping the storage of the
/// previous chunks: no allocation per event once the columns exist. Call
/// add_row once per record, then add_int64/add_uint64/add_double for each
/// numeric arg of that record.
class ChunkColumnBuilder {
   public:
    void clear();

    void add_row(std::uint64_t ts, std::uint64_t dur, bool has_dur,
                 std::uint64_t pid, std::uint64_t tid, std::uint8_t phase,
                 std::uint32_t cat, std::uint32_t name, std::uint32_t host);
    /// Adds the value of `path` on the row added last. `path` is the index
    /// dictionary id of the arg path.
    void add_int64(std::uint32_t path, std::int64_t v);
    void add_uint64(std::uint32_t path, std::uint64_t v);
    void add_double(std::uint32_t path, double v);

    bool empty() const { return block_.rows == 0; }

    /// Appends the encoded block to `out`, dropping whole arg path columns,
    /// those with the fewest rows first, until the block fits `budget` bytes.
    /// Returns false, appending nothing, when the block holds no row.
    bool encode_into(dstd::Vector<std::uint8_t>& out, std::size_t budget);

    /// The dictionary ids the block names, in `ids` order; each needs a
    /// dictionary entry in the file it is written under.
    const dstd::Vector<std::uint32_t>& ids() const { return block_.ids; }

   private:
    std::uint32_t local_id(std::uint32_t id);
    /// The local id of `id`, from the pair of the row before when it repeats.
    std::uint32_t row_local_id(std::uint32_t id, std::uint32_t& last,
                               std::uint32_t& last_local);
    ChunkColumn& column_of(std::uint32_t path, ColumnKind kind);

    ChunkColumns block_;
    dstd::HashMap<std::uint32_t, std::uint32_t> local_of_;
    dstd::HashMap<std::uint64_t, std::uint32_t> column_of_;
    std::uint32_t live_columns_ = 0;
    /// The dictionary id and local id the row before named as cat, name and
    /// host, so a run of equal ids skips the hash lookup.
    std::uint32_t last_cat_ = NO_DICTIONARY_ID;
    std::uint32_t last_cat_local_ = 0;
    std::uint32_t last_name_ = NO_DICTIONARY_ID;
    std::uint32_t last_name_local_ = 0;
    std::uint32_t last_host_ = NO_DICTIONARY_ID;
    std::uint32_t last_host_local_ = 0;
};

/// Appends the encoded block to `out`: its first `column_count` columns, with
/// the paths of `block.dropped` left out and listed. `budget` caps the block's
/// bytes, NO_COLUMN_BUDGET meaning no cap; over it, the block drops whole arg
/// path columns, those with the fewest rows first, and lists their path ids
/// with those already in `block.dropped`. The core columns are kept whatever
/// the budget. Re-encoding a decoded block with NO_COLUMN_BUDGET gives back the
/// same block, its dropped paths included, with the ids the block names.
void encode_chunk_columns(const ChunkColumns& block, std::size_t column_count,
                          dstd::Vector<std::uint8_t>& out, std::size_t budget);

/// The block of `bytes`, the payload of one chunk's column record, with its
/// `ids`, `cat`, `name`, `host`, `ts`, `dur`, `has_dur`, `pid`, `tid`,
/// `phase`, `columns` and `dropped` filled. `texts` stays empty and `chunk`
/// stays 0: neither is in the bytes. Each value equals the one the record
/// held: an Int64 or Uint64 value is the same integer of the same variant
/// kind, and a Double value is the same double bit for bit (-0.0 and NaN
/// payloads included). A block with a format this code does not know, a
/// truncated body or a value out of range is ErrorCode::Parse.
dstd::Expected<ChunkColumns, ErrorCode> decode_chunk_columns(
    dstd::Span<const std::uint8_t> bytes);

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_CHUNK_COLUMNS_H
