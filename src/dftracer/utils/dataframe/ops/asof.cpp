#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/kernels/order.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "ops_common.h"

namespace dftracer::utils::dataframe {

namespace {

// |left - right| <= tol in the column's own units; a negative tol rejects.
bool within(const ColumnView& l, std::int64_t il, const ColumnView& r,
            std::int64_t ir, std::int64_t tol) {
    if (tol < 0) return false;
    switch (l.kind()) {
        case ColumnView::Kind::Unsigned: {
            const std::uint64_t x = l.get_uint(il), y = r.get_uint(ir);
            return (x >= y ? x - y : y - x) <= static_cast<std::uint64_t>(tol);
        }
        case ColumnView::Kind::Float:
            return std::fabs(l.get_double(il) - r.get_double(ir)) <=
                   static_cast<double>(tol);
        default: {
            const std::int64_t x = l.get_int(il), y = r.get_int(ir);
            const std::uint64_t d = x >= y ? static_cast<std::uint64_t>(x) -
                                                 static_cast<std::uint64_t>(y)
                                           : static_cast<std::uint64_t>(y) -
                                                 static_cast<std::uint64_t>(x);
            return d <= static_cast<std::uint64_t>(tol);
        }
    }
}

// |left - r[a]| <= |left - r[b]|.
bool closer_or_equal(const ColumnView& l, std::int64_t il, const ColumnView& r,
                     std::int64_t a, std::int64_t b) {
    switch (l.kind()) {
        case ColumnView::Kind::Unsigned: {
            const std::uint64_t x = l.get_uint(il);
            const std::uint64_t ya = r.get_uint(a), yb = r.get_uint(b);
            return (x >= ya ? x - ya : ya - x) <= (x >= yb ? x - yb : yb - x);
        }
        case ColumnView::Kind::Float: {
            const double x = l.get_double(il);
            return std::fabs(x - r.get_double(a)) <=
                   std::fabs(x - r.get_double(b));
        }
        default: {
            const std::int64_t x = l.get_int(il);
            const std::int64_t ya = r.get_int(a), yb = r.get_int(b);
            const auto d = [&](std::int64_t y) {
                return x >= y ? static_cast<std::uint64_t>(x) -
                                    static_cast<std::uint64_t>(y)
                              : static_cast<std::uint64_t>(y) -
                                    static_cast<std::uint64_t>(x);
            };
            return d(ya) <= d(yb);
        }
    }
}

struct RightPartition {
    std::int64_t rep;
    std::int64_t start;
    std::int64_t time_end;  // the first null-time row, which sorts last
};

}  // namespace

DataFrame asof(const DataFrame& left, const DataFrame& right,
               const std::string& on, const std::vector<std::string>& by,
               AsofDirection direction, std::optional<std::int64_t> tolerance) {
    const std::size_t lt_at = ops::column_named(left, on, "asof");
    const std::size_t rt_at = ops::column_named(right, on, "asof");
    if (left.columns[lt_at].type() != right.columns[rt_at].type())
        throw std::invalid_argument("asof: ts column types differ");
    std::vector<std::size_t> skip{rt_at};
    for (const std::string& n : by) {
        const std::size_t l = ops::column_named(left, n, "asof");
        const std::size_t r = ops::column_named(right, n, "asof");
        if (left.columns[l].type() != right.columns[r].type())
            throw std::invalid_argument("asof: equi column types differ");
        skip.push_back(r);
    }
    if (!ColumnView::supports(left.columns[lt_at].type()))
        throw std::invalid_argument("asof: ts column must be numeric");

    std::vector<ColumnView> lequi = ops::views_of(left, by, "asof");
    std::vector<ColumnView> requi = ops::views_of(right, by, "asof");
    std::vector<ColumnView> lkeys = lequi;
    std::vector<ColumnView> rkeys = requi;
    lkeys.emplace_back(left.columns[lt_at]);
    rkeys.emplace_back(right.columns[rt_at]);
    const ColumnView& lts = lkeys.back();
    const ColumnView& rts = rkeys.back();
    if (lts.is_bytes())
        throw std::invalid_argument("asof: ts column must be numeric");

    const std::int64_t L = left.num_rows();
    const std::int64_t R = right.num_rows();
    const std::vector<std::int64_t> lorder = order_rows(lkeys, L);
    const std::vector<std::int64_t> rorder = order_rows(rkeys, R);

    std::vector<RightPartition> rparts;
    for (std::int64_t i = 0; i < R;) {
        const std::int64_t j = run_end(requi, rorder, i);
        std::int64_t te = i;
        while (te < j && !rts.is_missing(rorder[static_cast<std::size_t>(te)]))
            ++te;
        rparts.push_back({rorder[static_cast<std::size_t>(i)], i, te});
        i = j;
    }

    std::vector<std::int64_t> match(static_cast<std::size_t>(L), -1);
    const bool has_tol = tolerance.has_value();
    std::size_t rp = 0;
    for (std::int64_t li = 0; li < L;) {
        const std::int64_t lrep = lorder[static_cast<std::size_t>(li)];
        const std::int64_t lj = run_end(lequi, lorder, li);
        while (rp < rparts.size() &&
               ops::compare_keys(lequi, lrep, requi, rparts[rp].rep) > 0)
            ++rp;
        const bool matched =
            rp < rparts.size() &&
            ops::compare_keys(lequi, lrep, requi, rparts[rp].rep) == 0;
        const std::int64_t rstart = matched ? rparts[rp].start : 0;
        const std::int64_t rend = matched ? rparts[rp].time_end : 0;

        // Left rows are time-ascending within the partition, so the cursors
        // only move forward. `cur` chases the Backward/Forward boundary, and
        // `ge` and `gt` are the first right time >= and > the left time.
        std::int64_t cur = rstart, ge = rstart, gt = rstart;
        const auto right_vs_left = [&](std::int64_t pos, std::int64_t lr) {
            return rts.compare(rorder[static_cast<std::size_t>(pos)], lts, lr);
        };
        for (std::int64_t a = li; a < lj; ++a) {
            const std::int64_t lr = lorder[static_cast<std::size_t>(a)];
            if (!matched || lts.is_missing(lr)) continue;
            std::int64_t cand = -1;
            if (direction == AsofDirection::Backward) {
                while (cur < rend && right_vs_left(cur, lr) < 1) ++cur;
                if (cur - 1 >= rstart) cand = cur - 1;
            } else if (direction == AsofDirection::Forward) {
                while (cur < rend && right_vs_left(cur, lr) < 0) ++cur;
                if (cur < rend) cand = cur;
            } else {
                while (ge < rend && right_vs_left(ge, lr) < 0) ++ge;
                while (gt < rend && right_vs_left(gt, lr) <= 0) ++gt;
                const std::int64_t back = gt - 1 >= rstart ? gt - 1 : -1;
                const std::int64_t fwd = ge < rend ? ge : -1;
                if (back < 0)
                    cand = fwd;
                else if (fwd < 0)
                    cand = back;
                else
                    cand =
                        closer_or_equal(lts, lr, rts,
                                        rorder[static_cast<std::size_t>(back)],
                                        rorder[static_cast<std::size_t>(fwd)])
                            ? back
                            : fwd;
            }
            if (cand < 0) continue;
            const std::int64_t rr = rorder[static_cast<std::size_t>(cand)];
            if (!has_tol || within(lts, lr, rts, rr, *tolerance))
                match[static_cast<std::size_t>(lr)] = rr;
        }
        li = lj;
    }

    std::vector<std::int64_t> right_rows(static_cast<std::size_t>(L));
    for (std::size_t i = 0; i < right_rows.size(); ++i)
        right_rows[i] = match[static_cast<std::size_t>(lorder[i])];
    return ops::join_columns(left, right, skip, lorder, right_rows, "asof");
}

}  // namespace dftracer::utils::dataframe
