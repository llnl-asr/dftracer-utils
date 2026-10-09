#ifndef DFTRACER_UTILS_DATAFRAME_GRACE_JOIN_H
#define DFTRACER_UTILS_DATAFRAME_GRACE_JOIN_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/spill.h>
#include <dftracer/utils/dataframe/join.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/types.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::dataframe {

/// `parts` as one frame: vertically when they share names and types, else
/// diagonally.
DataFrame merge_morsels(std::vector<DataFrame>& parts);

/// The right-side rows a cross join has spooled since the process started. For
/// tests: a cross join whose right side is over a quarter of its budget
/// raises it by the right side's row count, one that fits does not.
std::uint64_t cross_join_spooled_rows();

/// The right-side blocks GraceJoin has read block by block since the process
/// started (for a Nest join, the key groups it has joined one pass each). For
/// tests: a partition that one key fills raises it.
std::uint64_t grace_join_blocks();

/// A join whose right side does not fit the memory budget. Both sides are
/// split on the join key into partitions on disk; each partition then joins
/// in memory with a HashJoin, so the rows and columns equal the in-memory
/// join's. Rows with equal keys meet in one partition, including keys of
/// different number types (the partition is a hash of the value key), and a
/// row with a null key part goes to partition 0 on both sides. The output
/// order is by partition. A partition whose right side is still over the
/// budget is split again with another seed, up to a depth limit; one that a
/// split cannot cut (one key holds its rows) is joined block by block: its
/// right side is read in blocks that fit the budget and the whole left
/// partition is probed against each, with the left rows that no block matched
/// emitted once at the end. A Lookup join reads such a partition's right side
/// in blocks too, and keeps per key only its first right row and the first
/// value column in which a later row differs (the conflict), then probes the
/// left partition once against those first rows, so it throws what the
/// in-memory join throws. That state is held in memory: it is one row and the
/// key text per distinct key of the partition, and a partition that no split
/// could cut holds few keys. A Nest join reads the right side once to total
/// the bytes of each key, packs the keys into groups of about a block, and
/// joins one group per pass over the right side: a left row whose key is in
/// the group leaves with its list, the others at the end with an empty list.
/// A list holds the key's right rows in their right order, as in memory. One
/// key whose rows pass the budget makes one list that passes it, since a
/// list cell is as large as its key's rows. A probe takes as many left rows
/// as the output share allows from what the last ones made.
///
/// Call add_right() for every right morsel, add_left() for every left
/// morsel, start(), then next() until it gives nothing. Not thread-safe.
/// Not for a cross join, which has no key. Throws std::invalid_argument for a
/// morsel whose column names differ from the first right morsel's.
class GraceJoin {
   public:
    GraceJoin(std::vector<std::string> left_names,
              std::vector<std::string> right_names,
              std::vector<std::string> left_on,
              std::vector<std::string> right_on, JoinHow how,
              std::string suffix, std::uint64_t budget,
              bool nulls_equal = false);

    void add_right(const DataFrame& right);

    void add_left(const DataFrame& left);

    /// `left_templates` gives one column per left name, read for its type,
    /// for the unmatched right rows of a right or outer join.
    void start(std::vector<Series> left_templates);

    coro::CoroTask<std::optional<DataFrame>> next(std::int64_t max_rows);

   private:
    struct Part {
        Part(std::uint64_t budget, const std::shared_ptr<spill::Dir>& dir)
            : right(budget, dir), left(budget, dir) {}
        spill::Spool right;
        spill::Spool left;
        std::int64_t right_rows = 0;
        std::int64_t left_rows = 0;
    };
    struct Work {
        std::unique_ptr<Part> part;
        int level = 0;
        bool blocks = false;  // a split cannot cut it: join it block by block
    };
    struct Current {
        std::unique_ptr<Part> part;
        std::optional<HashJoin> join;
        std::unique_ptr<Cursor> left;
        std::optional<DataFrame> left_rows;  // the left morsel being probed
        std::int64_t left_at = 0;            // its next row
        std::int64_t left_base = 0;          // its first row in the partition
        double expansion = 1.0;              // output rows per left row seen
        std::uint64_t out_row_bytes = 0;     // bytes per output row seen
        // Block by block only.
        bool blocks = false;
        std::unique_ptr<Cursor> right;      // the right spool, read in blocks
        std::vector<std::uint8_t> matched;  // per left row: a block matched
        // Nest only: the group of each key, and the group joined now.
        StringViewMap<std::uint64_t> group_of;
        std::uint64_t group = 0;
        std::uint64_t groups = 0;
        bool last_pass = false;  // probing done: the deferred left rows leave
    };

    using Parts = std::vector<std::unique_ptr<Part>>;

    Parts make_parts(std::size_t n) const;
    void split(const DataFrame& rows, bool right_side, int level, Parts& into);
    coro::CoroTask<void> split_part(Work work, std::int64_t max_rows);
    coro::CoroTask<DataFrame> right_rows(Part& part, std::int64_t max_rows);
    DataFrame empty_right() const;
    coro::CoroTask<std::optional<DataFrame>> right_block(std::int64_t max_rows);
    coro::CoroTask<DataFrame> lookup_heads(std::vector<std::int64_t>& conflicts,
                                           std::int64_t max_rows);
    coro::CoroTask<void> assign_groups(std::int64_t max_rows);
    coro::CoroTask<std::optional<DataFrame>> next_group(std::int64_t max_rows);
    coro::CoroTask<bool> next_block(std::int64_t max_rows);
    void start_deferred();
    coro::CoroTask<std::optional<DataFrame>> probe_left(std::int64_t max_rows);
    coro::CoroTask<std::optional<DataFrame>> deferred_left(
        std::int64_t max_rows);

    std::vector<std::string> left_names_;
    std::vector<std::string> right_names_;
    std::vector<std::string> left_on_;
    std::vector<std::string> right_on_;
    std::vector<std::size_t> left_key_;
    std::vector<std::size_t> right_key_;
    JoinHow how_;
    std::string suffix_;
    std::uint64_t budget_;
    bool nulls_equal_ = false;
    std::shared_ptr<spill::Dir> dir_;
    Parts top_;
    std::deque<Work> queue_;
    std::optional<Current> cur_;
    std::optional<DataFrame> right_template_;
    std::vector<Series> left_templates_;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_GRACE_JOIN_H
