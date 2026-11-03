// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_JOIN_H
#define DFTRACER_UTILS_DATAFRAME_JOIN_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/dstd/vendor/unordered_dense.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace dftracer::utils::dataframe {

/// Whether `how` is one of the JoinHow enumerators (an int cast from C may
/// not be).
bool valid_join_how(JoinHow how) noexcept;

/// Which right columns a join emits, and under what names: every right column
/// except a key sharing its left key's name, suffixed where it collides with a
/// left name. Empty for Semi / Anti. Lookup keeps every right column but the
/// keys, unsuffixed; `fills` holds the left column each fills, or -1 when it
/// is appended. Nest keeps every right column, inside its one list column.
/// Throws std::invalid_argument on an uneven key list.
struct JoinRightLayout {
    dstd::Vector<std::size_t> keep;
    dstd::Vector<dstd::String> names;
    dstd::Vector<std::int64_t> fills;
};
JoinRightLayout join_right_layout(const dstd::Vector<dstd::String>& left_names,
                                  const dstd::Vector<dstd::String>& right_names,
                                  const dstd::Vector<dstd::String>& left_on,
                                  const dstd::Vector<dstd::String>& right_on,
                                  JoinHow how, const dstd::String& suffix);

/// Output column names of a join: `left_names`, then join_right_layout's.
/// Throws std::out_of_range if a key is absent from its side.
dstd::Vector<dstd::String> join_out_names(
    const dstd::Vector<dstd::String>& left_names,
    const dstd::Vector<dstd::String>& right_names,
    const dstd::Vector<dstd::String>& left_on,
    const dstd::Vector<dstd::String>& right_on, JoinHow how,
    const dstd::String& suffix);

/// Output fields of a join: join_out_names zipped with the left / right column
/// types, every field nullable.
dstd::Vector<Field> join_out_fields(const dstd::Vector<Field>& left,
                                    const dstd::Vector<Field>& right,
                                    const dstd::Vector<dstd::String>& left_on,
                                    const dstd::Vector<dstd::String>& right_on,
                                    JoinHow how, const dstd::String& suffix);

/// The build side of a hash join, probed by one or more left chunks. The
/// right frame is hashed once on `right_on`; each probe() joins one left chunk
/// against it and returns that chunk's output rows in left order. A
/// right-preserving join (Right / Outer) remembers which right rows matched
/// across probes and emits the rest from flush() once every left chunk has
/// been probed. The eager DataFrame join is one probe plus one flush.
class HashJoin {
   public:
    /// Validates `how`, the key arity and the right key columns; a left key is
    /// validated by the first probe. With `nulls_equal` a null key cell matches
    /// another null key cell (never a value); it is refused for a cross, lookup
    /// or nest join. Throws std::invalid_argument / std::out_of_range naming
    /// the offending key.
    HashJoin(DataFrame right, dstd::Vector<dstd::String> left_on,
             dstd::Vector<dstd::String> right_on, JoinHow how,
             dstd::String suffix, bool nulls_equal = false);

    /// join_out_names over this build side's column names.
    dstd::Vector<dstd::String> out_names(
        const dstd::Vector<dstd::String>& left_names) const;

    /// Join one left chunk. Throws std::out_of_range if a left key is absent;
    /// std::invalid_argument if a key pair's types differ (not for Lookup or
    /// Nest), or for a Lookup key whose right rows differ or a filled left
    /// cell that holds a value.
    ///
    /// With `left_matched`, the probe of one block of a right side that is
    /// joined block by block: it sets `(*left_matched)[left_base + i]` for each
    /// left row i that matches, and leaves out the rows that depend on no block
    /// matching (the unmatched left rows of Left, Outer, Anti and Nest, and
    /// every Semi row), which the caller emits once all blocks are probed. Not
    /// for Lookup.
    DataFrame probe(const DataFrame& left,
                    dstd::Vector<std::uint8_t>* left_matched = nullptr,
                    std::int64_t left_base = 0);

    /// Lookup over a right side that holds one row per key (the first right
    /// row of each key): `per_row[i]` is -1, or the value column in which some
    /// right row of the key of row i differs from row i, as conflicts() would
    /// have found over all the right rows. Call before the first probe.
    void set_lookup_conflicts(dstd::Vector<std::int64_t> per_row);

    /// The unmatched right rows of a Right / Outer join: a left column is
    /// null, except a key sharing its right key's name, which carries the
    /// right value. `left_templates` supplies one Series per left column,
    /// read only for its type (any length). Empty for every other `how`, and
    /// empty again after the first call.
    DataFrame flush(const dstd::Vector<dstd::String>& left_names,
                    const dstd::Vector<Series>& left_templates);

    JoinHow how() const noexcept { return how_; }
    const DataFrame& right() const noexcept { return right_; }

   private:
    dstd::Vector<std::int64_t> left_key_indices(
        const dstd::Vector<dstd::String>& left_names) const;
    /// Lookup and Nest: the probe, rows matched by value.
    DataFrame probe_values(const DataFrame& left,
                           const dstd::Vector<std::int64_t>& key_idx,
                           dstd::Vector<std::uint8_t>* left_matched,
                           std::int64_t left_base);
    /// The first right row of each group of equal keys, per left row, -1
    /// when none; by the typed index when every key pair shares an exact
    /// type, else by value keys.
    dstd::Vector<std::int64_t> heads(const dstd::Vector<Series>& left_keys,
                                     bool typed);
    /// The rows of the group whose first row is `head`.
    void members(std::int64_t head, bool typed,
                 dstd::Vector<std::int64_t>& out) const;
    void build_value_index();
    /// Lookup: per first row of a group of equal keys, -1, or a value column
    /// in which some row of the group differs from the first.
    dstd::Vector<std::int64_t> conflicts(bool typed) const;
    /// The key position whose right key shares the name of left column `li`,
    /// or -1.
    std::int64_t shared_key_of(const dstd::Vector<dstd::String>& left_names,
                               std::size_t li) const;

    DataFrame right_;
    dstd::Vector<dstd::String> left_on_;
    dstd::Vector<dstd::String> right_on_;
    JoinHow how_;
    dstd::String suffix_;
    bool nulls_equal_ = false;  // a null key matches a null key
    dstd::Vector<Series> right_keys_;
    // The 64-bit key hash of each right row to the first row with it; rows
    // sharing a hash chain through `next_` (ascending), and a probe compares
    // the key cells exactly along the chain, so a hash collision costs a
    // compare, never a wrong match.
    ankerl::unordered_dense::map<std::uint64_t, std::int64_t> first_;
    // In place of `first_` for a single integer key over a dense range: the
    // first row of each key value, indexed by value - direct_base_.
    dstd::Vector<std::int64_t> direct_;
    std::uint64_t direct_base_ = 0;
    dstd::Vector<std::int64_t> next_;
    // Lookup and Nest by value: the rows of each value key, in order.
    dstd::StringViewMap<dstd::Vector<std::int64_t>> by_value_;
    bool value_index_ = false;
    bool conflicts_given_ = false;
    dstd::Vector<std::int64_t> typed_conflicts_;
    dstd::Vector<std::int64_t> value_conflicts_;
    dstd::UniquePtr<std::atomic<std::uint8_t>[]> right_matched_;
    std::size_t right_matched_n_ = 0;
    bool flushed_ = false;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_JOIN_H
