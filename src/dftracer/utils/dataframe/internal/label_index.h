// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_LABEL_INDEX_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_LABEL_INDEX_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/label_cache.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <mutex>

namespace dftracer::utils::dataframe {

/// What one label cell is, after numbers of every type are folded together: an
/// integer, a float or an unsigned integer too big for Int64 each have one
/// spelling, so equal values have equal kind and bits whatever their column
/// type. A Bool is its own kind: 1 and True are different labels, as in
/// pandas. Null stands for a null and for a NaN, the pandas NaN label. A type
/// with no number or text form (a date, a decimal, ...) is Tagged plus its
/// TypeId, so only its own type matches.
enum class CellKind : std::uint8_t {
    Null = 0,
    Int,
    Uint,
    Float,
    Text,
    Blob,
    Bool,
    Tagged = 16,
};

/// One label column as canonical cells: a kind and 64 bits per row (a number's
/// bits, or the hash of a text's bytes, whose bytes stay in the column).
class CellColumn {
   public:
    /// A column of a numeric or byte type, in any encoding. An InvalidArgument
    /// error for a List or Struct.
    static dstd::Result<CellColumn> make(const Series& column);

    std::int64_t size() const noexcept {
        return static_cast<std::int64_t>(kind_.size());
    }
    CellKind kind(std::int64_t i) const noexcept {
        return static_cast<CellKind>(kind_[static_cast<std::size_t>(i)]);
    }
    std::uint64_t bits(std::int64_t i) const noexcept {
        return bits_[static_cast<std::size_t>(i)];
    }
    /// The bytes of a text, blob or tagged byte cell.
    dstd::StringView bytes(std::int64_t i) const;
    /// Whether cell `i` is a number or a temporal value that subtracts from
    /// another (a null, a text and a decimal do not), and its value as a
    /// double (a temporal value counts its ticks).
    bool is_number(std::int64_t i) const noexcept;
    double number(std::int64_t i) const noexcept;
    /// The type of the column the cells came from, and for a Timestamp,
    /// Duration or Time column the unit its ticks count.
    TypeId type() const noexcept { return type_; }
    bool has_unit() const noexcept { return has_unit_; }
    TimeUnit unit() const noexcept { return unit_; }
    /// The same cells (sharing the column's buffers).
    CellColumn copy() const;
    /// The cells of a column with a unit, counted in the finer unit `to`
    /// (exact: every tick is multiplied up). An InvalidArgument error when a
    /// value does not fit 64 bits in that unit.
    dstd::Result<CellColumn> in_unit(TimeUnit to) const;
    /// The hash of a cell; equal cells (@ref equal) hash equal.
    std::uint64_t hash(std::int64_t i) const noexcept;

    /// Whether cell `i` of `a` and cell `j` of `b` are the same label: a null
    /// is the same as a null.
    static bool equal(const CellColumn& a, std::int64_t i, const CellColumn& b,
                      std::int64_t j) noexcept;
    /// -1, 0 or 1 as cell `i` of `a` orders before, with or after cell `j` of
    /// `b`; `comparable` is false (and the result 0) when the two have no
    /// order: a null, a text against a number, two types.
    static int compare(const CellColumn& a, std::int64_t i, const CellColumn& b,
                       std::int64_t j, bool& comparable) noexcept;

   private:
    bool is_bytes(CellKind k) const noexcept;

    Series column_;  // flat
    dstd::Vector<std::uint64_t> bits_;
    dstd::Vector<std::uint8_t> kind_;
    TypeId type_ = TypeId::Unknown;
    bool has_unit_ = false;
    TimeUnit unit_ = TimeUnit::Nano;
};

/// The lookup structure of a set of label columns: for each prefix of the
/// levels (the full key, or the first level alone for a partial MultiIndex
/// key) a hash from the label tuple to its rows, built on the first lookup of
/// that prefix, and whether the labels are sorted. Immutable once built; the
/// lazy parts are built under a once_flag, so any number of threads may look
/// up at once.
class LabelIndex {
   public:
    LabelIndex() = default;

    /// Canonicalizes `levels` (one column per level, all as long as the rows).
    /// An InvalidArgument error for no level, levels of different lengths, a
    /// List or Struct, or more rows than a uint32 counts.
    static dstd::Result<dstd::SharedPtr<const LabelIndex>> make(
        dstd::Span<const Series> levels);

    /// Whether `levels` are the columns this index was built from (the same
    /// buffers), so a cached index is never used for other data.
    bool matches(dstd::Span<const Series> levels) const noexcept;

    /// @ref Series::locate over the labels.
    dstd::Result<Located> locate(dstd::Span<const Series> keys) const;
    /// @ref Series::locate_range over the labels.
    dstd::Result<LabelSpan> locate_range(dstd::Span<const Series> start,
                                         dstd::Span<const Series> stop) const;
    /// @ref Series::reindex over the labels: for each key row, the row of the
    /// label it finds (through `options`), or -1.
    dstd::Result<dstd::Vector<std::int64_t>> reindex(
        dstd::Span<const Series> keys, const ReindexOptions& options) const;

   private:
    struct Table {
        dstd::Vector<std::uint32_t> slots;   // group + 1, 0 = empty
        dstd::Vector<std::uint64_t> hashes;  // per group
        dstd::Vector<std::int64_t> first;    // per group: its first row
        dstd::Vector<std::int64_t> start;    // groups + 1 offsets into rows
        dstd::Vector<std::int64_t> rows;     // row numbers by group, in order
    };
    struct Slot {
        std::once_flag once;
        Table table;
    };

    // One Int64 or Timestamp level of unique labels as a flat open-addressing
    // array of (label, row): one probe, one cache line.
    struct IntSlot {
        std::int64_t label = 0;
        std::uint32_t row_plus_one = 0;
    };
    enum class IntState : std::uint8_t { Unusable, Duplicate, Usable };
    struct IntTable {
        dstd::Vector<IntSlot> slots;
        IntState state = IntState::Unusable;
        std::uint8_t kind = 0;
    };

    const Table& table(std::size_t prefix) const;
    const IntTable& int_table() const;
    // The label hash of the typed table when `keys` are one level of cells of
    // its kind, else null.
    const IntTable* int_table_for(dstd::Span<const CellColumn> keys) const;
    dstd::Result<dstd::SharedPtr<const LabelIndex>> in_units(
        dstd::Span<dstd::Vector<CellColumn>* const> sets) const;
    Located locate_cells(dstd::Span<const CellColumn> keys,
                         std::int64_t count) const;
    dstd::Result<dstd::Vector<std::int64_t>> reindex_cells(
        dstd::Span<const CellColumn> keys, std::int64_t count,
        const ReindexOptions& options) const;
    dstd::Result<std::int64_t> bound_cells(dstd::Span<const CellColumn> keys,
                                           std::int64_t count,
                                           dstd::StringView which) const;
    dstd::Result<LabelSpan> locate_range_cells(
        dstd::Span<const CellColumn> start, std::int64_t start_count,
        dstd::Span<const CellColumn> stop, std::int64_t stop_count) const;
    std::int64_t find(const Table& t, std::uint64_t hash,
                      dstd::Span<const CellColumn> key, std::int64_t key_row,
                      std::size_t prefix) const noexcept;
    void order() const;
    dstd::Result<dstd::Vector<std::int64_t>> reindex_fill(
        dstd::Span<const CellColumn> keys, std::int64_t count,
        const ReindexOptions& options) const;

    std::int64_t rows_ = 0;
    dstd::Vector<CellColumn> levels_;
    dstd::Vector<Series> sources_;
    mutable dstd::Vector<dstd::UniquePtr<Slot>> tables_;
    mutable std::once_flag order_once_;
    mutable std::once_flag int_once_;
    mutable IntTable int_;
    mutable bool increasing_ = false;
    mutable bool decreasing_ = false;
};

/// The number of LabelIndex objects built in this process, for tests that a
/// lookup builds once.
std::uint64_t label_index_builds() noexcept;

/// The lookup of @ref Series::locate and @ref DataFrame::locate: through
/// `cache` when it is given and holds an index of `levels`, else through an
/// index built for the call. `levels` empty means the positions 0 .. rows-1.
dstd::Result<Located> locate_labels(LabelCache* cache,
                                    dstd::Span<const Series> levels,
                                    std::int64_t rows,
                                    dstd::Span<const Series> keys);
/// The lookup of @ref Series::reindex and @ref DataFrame::reindex, through
/// `cache` as @ref locate_labels: the row of `levels` for each key row, or -1.
dstd::Result<dstd::Vector<std::int64_t>> reindex_labels(
    LabelCache* cache, dstd::Span<const Series> levels, std::int64_t rows,
    dstd::Span<const Series> keys, const ReindexOptions& options);
dstd::Result<LabelSpan> locate_label_range(LabelCache* cache,
                                           dstd::Span<const Series> levels,
                                           std::int64_t rows,
                                           dstd::Span<const Series> start,
                                           dstd::Span<const Series> stop);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_LABEL_INDEX_H
