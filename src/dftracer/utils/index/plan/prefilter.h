// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_PLAN_PREFILTER_H
#define DFTRACER_UTILS_INDEX_PLAN_PREFILTER_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/literal_set.h>
#include <dftracer/utils/duql/query.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::index::plan {

struct PrefilterKernel;

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
    Prefilter();
    explicit Prefilter(const duql::Query& q);

    bool empty() const {
        return parts_.clauses.empty() && parts_.ranges.empty();
    }
    /// `line` may be a whole chunk of lines.
    bool may_match(dstd::StringView line) const;
    /// The per-needle reference path of `may_match`; same result.
    bool may_match_per_needle(dstd::StringView line) const;
    const dstd::Vector<dstd::Vector<dstd::String>>& clauses() const {
        return parts_.clauses;
    }

    /// What a Gate counts; see PrefilterKernel::gate_may_match.
    struct GateState {
        bool on;
        std::size_t checked = 0;
        std::size_t passed = 0;
    };

    /// One scan unit's use of a prefilter: after CHECK_WINDOW lines, a pass
    /// rate above MAX_PASS_RATE turns it off for the rest of the unit.
    class Gate {
       public:
        explicit Gate(const Prefilter& p) : p_(&p), state_{!p.empty()} {}
        bool may_match(dstd::StringView line);

       private:
        const Prefilter* p_;
        GateState state_;
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
        dstd::String path;
        dstd::String segment;
        dstd::Vector<Bound> bounds;
        /// Offset in `segment` of the byte memchr looks for.
        std::size_t pivot = 0;
    };

    /// What the per-line kernels read.
    struct Parts {
        dstd::Vector<dstd::Vector<dstd::String>> clauses;
        dstd::Vector<Range> ranges;
        /// One literal set over the distinct needles; empty with none, or
        /// when no clause has HS_MIN_ALTERNATIVES needles.
        duql::LiteralSet literals;
        /// Per clause needle, its id in `literals`.
        dstd::Vector<dstd::Vector<unsigned>> ids;
        /// Per needle, the offset of its rarest byte, which memchr looks for.
        dstd::Vector<dstd::Vector<std::size_t>> pivots;
    };

    const Parts& parts() const { return parts_; }

   private:
    Parts parts_;
    /// The build of the per-line code this process uses, fixed at
    /// construction.
    const PrefilterKernel* kernel_;
};

}  // namespace dftracer::utils::index::plan

#endif  // DFTRACER_UTILS_INDEX_PLAN_PREFILTER_H
