#ifndef DFTRACER_UTILS_INDEX_PLAN_PREFILTER_H
#define DFTRACER_UTILS_INDEX_PLAN_PREFILTER_H

#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/duql/query.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
struct hs_database;
#endif

namespace dftracer::utils::index::plan {

/// Byte tests a line must pass to match a query, run before the line is
/// parsed. A line may match only when each clause has one of its needles in
/// it and each range holds for some number written after its key; with no
/// clauses or ranges every line may match. Needles are values: a JSON string
/// with its quotes, or an integer's digits, from `==` and `in` leaves whose
/// values a writer spells one way (canonical numbers, strings of printable
/// ASCII without `"`, `\` or `/`). Ranges come from `==`, `<`, `<=`, `>` and
/// `>=` leaves on a number under `and` only, for a path whose last segment is a
/// plain name: every value the field reads is written after a key ending in
/// that segment (`"ts"`, or a flat dotted `"args.ts"`), so a line whose
/// numbers after such keys all fail the range cannot match.
class Prefilter {
   public:
    /// No clauses: every line may match.
    Prefilter() = default;
    explicit Prefilter(const duql::Query& q);

    bool empty() const { return clauses_.empty() && ranges_.empty(); }
    /// `line` may be a whole chunk of lines.
    bool may_match(std::string_view line) const;
    /// The per-needle reference path of `may_match`; same result.
    bool may_match_per_needle(std::string_view line) const;
    const std::vector<std::vector<std::string>>& clauses() const {
        return clauses_;
    }

    /// One scan unit's use of a prefilter: after CHECK_WINDOW lines, a pass
    /// rate above MAX_PASS_RATE turns it off for the rest of the unit.
    class Gate {
       public:
        explicit Gate(const Prefilter& p) : p_(&p), on_(!p.empty()) {}
        bool may_match(std::string_view line);

       private:
        const Prefilter* p_;
        bool on_;
        std::size_t checked_ = 0;
        std::size_t passed_ = 0;
    };

    static constexpr std::size_t CHECK_WINDOW = 1024;
    static constexpr double MAX_PASS_RATE = 0.9;
    /// Fewest needles in some clause for `may_match` to use Vectorscan; below
    /// it the per-needle scan is faster.
    static constexpr std::size_t HS_MIN_ALTERNATIVES = 4;

    /// `op` a bound, for one number.
    struct Bound {
        duql::CompareOp op;
        bool integral = true;
        std::int64_t ibound = 0;
        double dbound = 0;
    };
    /// A field's bounds: its one resolved value, written after a key ending
    /// in `segment`, satisfies them all.
    struct Range {
        std::string path;
        std::string segment;
        std::vector<Bound> bounds;
        /// Offset in `segment` of the byte memchr looks for.
        std::size_t pivot = 0;
    };

   private:
    static bool range_may_hold(std::string_view line, const Range& r);

    std::vector<std::vector<std::string>> clauses_;
    std::vector<Range> ranges_;
#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
    /// One literal database over the distinct needles; null with none, or
    /// when no clause has HS_MIN_ALTERNATIVES needles.
    std::shared_ptr<hs_database> db_;
    /// Per clause needle, its id in `db_`.
    std::vector<std::vector<unsigned>> ids_;
    std::size_t distinct_ = 0;
#endif
    /// Per needle, the offset of its rarest byte, which memchr looks for.
    std::vector<std::vector<std::size_t>> pivots_;
};

}  // namespace dftracer::utils::index::plan

#endif  // DFTRACER_UTILS_INDEX_PLAN_PREFILTER_H
