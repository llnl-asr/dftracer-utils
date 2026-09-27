#ifndef DFTRACER_UTILS_DATAFRAME_JOIN_H
#define DFTRACER_UTILS_DATAFRAME_JOIN_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/types.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
    std::vector<std::size_t> keep;
    std::vector<std::string> names;
    std::vector<std::int64_t> fills;
};
JoinRightLayout join_right_layout(const std::vector<std::string>& left_names,
                                  const std::vector<std::string>& right_names,
                                  const std::vector<std::string>& left_on,
                                  const std::vector<std::string>& right_on,
                                  JoinHow how, const std::string& suffix);

/// Output column names of a join: `left_names`, then join_right_layout's.
/// Throws std::out_of_range if a key is absent from its side.
std::vector<std::string> join_out_names(
    const std::vector<std::string>& left_names,
    const std::vector<std::string>& right_names,
    const std::vector<std::string>& left_on,
    const std::vector<std::string>& right_on, JoinHow how,
    const std::string& suffix);

/// Output fields of a join: join_out_names zipped with the left / right column
/// types, every field nullable.
std::vector<Field> join_out_fields(const std::vector<Field>& left,
                                   const std::vector<Field>& right,
                                   const std::vector<std::string>& left_on,
                                   const std::vector<std::string>& right_on,
                                   JoinHow how, const std::string& suffix);

/// The build side of a hash join, probed by one or more left chunks. The
/// right frame is hashed once on `right_on`; each probe() joins one left chunk
/// against it and returns that chunk's output rows in left order. A
/// right-preserving join (Right / Outer) remembers which right rows matched
/// across probes and emits the rest from flush() once every left chunk has
/// been probed. The eager DataFrame join is one probe plus one flush.
class HashJoin {
   public:
    /// Validates `how`, the key arity and the right key columns; a left key is
    /// validated by the first probe. Throws std::invalid_argument /
    /// std::out_of_range naming the offending key.
    HashJoin(DataFrame right, std::vector<std::string> left_on,
             std::vector<std::string> right_on, JoinHow how,
             std::string suffix);

    /// join_out_names over this build side's column names.
    std::vector<std::string> out_names(
        const std::vector<std::string>& left_names) const;

    /// Join one left chunk. Throws std::out_of_range if a left key is absent;
    /// std::invalid_argument if a key pair's types differ (not for Lookup or
    /// Nest), or for a Lookup key whose right rows differ or a filled left
    /// cell that holds a value.
    DataFrame probe(const DataFrame& left);

    /// The unmatched right rows of a Right / Outer join: a left column is
    /// null, except a key sharing its right key's name, which carries the
    /// right value. `left_templates` supplies one Series per left column,
    /// read only for its type (any length). Empty for every other `how`, and
    /// empty again after the first call.
    DataFrame flush(const std::vector<std::string>& left_names,
                    const std::vector<Series>& left_templates);

    JoinHow how() const noexcept { return how_; }
    const DataFrame& right() const noexcept { return right_; }

   private:
    std::vector<std::int64_t> left_key_indices(
        const std::vector<std::string>& left_names) const;
    /// Lookup and Nest: the probe, rows matched by value.
    DataFrame probe_values(const DataFrame& left,
                           const std::vector<std::int64_t>& key_idx);
    /// The first right row of each group of equal keys, per left row, -1
    /// when none; by the typed index when every key pair shares an exact
    /// type, else by value keys.
    std::vector<std::int64_t> heads(const std::vector<Series>& left_keys,
                                    bool typed);
    /// The rows of the group whose first row is `head`.
    void members(std::int64_t head, bool typed,
                 std::vector<std::int64_t>& out) const;
    void build_value_index();
    /// Lookup: per first row of a group of equal keys, -1, or a value column
    /// in which some row of the group differs from the first.
    std::vector<std::int64_t> conflicts(bool typed) const;
    /// The key position whose right key shares the name of left column `li`,
    /// or -1.
    std::int64_t shared_key_of(const std::vector<std::string>& left_names,
                               std::size_t li) const;

    DataFrame right_;
    std::vector<std::string> left_on_;
    std::vector<std::string> right_on_;
    JoinHow how_;
    std::string suffix_;
    std::vector<Series> right_keys_;
    // The 64-bit key hash of each right row to the first row with it; rows
    // sharing a hash chain through `next_` (ascending), and a probe compares
    // the key cells exactly along the chain, so a hash collision costs a
    // compare, never a wrong match.
    ankerl::unordered_dense::map<std::uint64_t, std::int64_t> first_;
    // In place of `first_` for a single integer key over a dense range: the
    // first row of each key value, indexed by value - direct_base_.
    std::vector<std::int64_t> direct_;
    std::uint64_t direct_base_ = 0;
    std::vector<std::int64_t> next_;
    // Lookup and Nest by value: the rows of each value key, in order.
    StringViewMap<std::vector<std::int64_t>> by_value_;
    bool value_index_ = false;
    std::vector<std::int64_t> typed_conflicts_;
    std::vector<std::int64_t> value_conflicts_;
    std::unique_ptr<std::atomic<std::uint8_t>[]> right_matched_;
    std::size_t right_matched_n_ = 0;
    bool flushed_ = false;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_JOIN_H
