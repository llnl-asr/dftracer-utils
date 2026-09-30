#ifndef DFTRACER_UTILS_DATAFRAME_GRACE_JOIN_H
#define DFTRACER_UTILS_DATAFRAME_GRACE_JOIN_H

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

/// A join whose right side does not fit the memory budget. Both sides are
/// split on the join key into partitions on disk; each partition then joins
/// in memory with a HashJoin, so the rows and columns equal the in-memory
/// join's. Rows with equal keys meet in one partition, including keys of
/// different number types (the partition is a hash of the value key), and a
/// row with a null key part goes to partition 0 on both sides. The output
/// order is by partition. A partition whose right side is still over the
/// budget is split again with another seed, up to a depth limit.
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
    };
    struct Current {
        std::unique_ptr<Part> part;
        std::optional<HashJoin> join;
        std::unique_ptr<Cursor> left;
    };

    using Parts = std::vector<std::unique_ptr<Part>>;

    Parts make_parts(std::size_t n) const;
    void split(const DataFrame& rows, bool right_side, int level, Parts& into);
    coro::CoroTask<void> split_part(Work work, std::int64_t max_rows);
    coro::CoroTask<DataFrame> right_rows(Part& part, std::int64_t max_rows);

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
