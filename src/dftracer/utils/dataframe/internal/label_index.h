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

    const Table& table(std::size_t prefix) const;
    std::int64_t find(const Table& t, std::uint64_t hash,
                      dstd::Span<const CellColumn> key, std::int64_t key_row,
                      std::size_t prefix) const noexcept;
    void order() const;
    dstd::Result<std::int64_t> bound(dstd::Span<const Series> key,
                                     dstd::StringView which) const;

    std::int64_t rows_ = 0;
    dstd::Vector<CellColumn> levels_;
    dstd::Vector<Series> sources_;
    mutable dstd::Vector<dstd::UniquePtr<Slot>> tables_;
    mutable std::once_flag order_once_;
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
dstd::Result<LabelSpan> locate_label_range(LabelCache* cache,
                                           dstd::Span<const Series> levels,
                                           std::int64_t rows,
                                           dstd::Span<const Series> start,
                                           dstd::Span<const Series> stop);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_LABEL_INDEX_H
