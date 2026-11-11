// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/internal/errors.h>
#include <dftracer/utils/dataframe/kernels/order.h>
#include <dftracer/utils/dataframe/ops/ops_common.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <utility>

namespace dftracer::utils::dataframe {

namespace {

struct RightPartition {
    std::int64_t rep;
    std::int64_t start;
    std::int64_t lo_end;  // the first null-lo row, which sorts last
};

}  // namespace

dstd::Result<DataFrame> interval(const DataFrame& left, const DataFrame& right,
                                 const dstd::String& point,
                                 const dstd::String& lo, const dstd::String& hi,
                                 const dstd::Vector<dstd::String>& by,
                                 bool outer) {
    const dstd::Result<std::size_t> p =
        ops::column_named(left, point, "interval");
    if (!p) return p.error();
    const dstd::Result<std::size_t> lo_col =
        ops::column_named(right, lo, "interval");
    if (!lo_col) return lo_col.error();
    const dstd::Result<std::size_t> hi_col =
        ops::column_named(right, hi, "interval");
    if (!hi_col) return hi_col.error();
    const std::size_t p_at = *p;
    const std::size_t lo_at = *lo_col;
    const std::size_t hi_at = *hi_col;
    const TypeId type = left.columns[p_at].type();
    if (type != right.columns[lo_at].type() ||
        type != right.columns[hi_at].type())
        return invalid_argument<"interval: point/lo/hi types differ">();
    if (!ColumnView::supports(type) ||
        ColumnView(left.columns[p_at]).is_bytes())
        return invalid_argument<
            "interval: point/lo/hi columns must be numeric">();
    dstd::Vector<std::size_t> skip{lo_at, hi_at};
    for (const dstd::String& n : by) {
        const dstd::Result<std::size_t> lc =
            ops::column_named(left, n, "interval");
        if (!lc) return lc.error();
        const dstd::Result<std::size_t> rc =
            ops::column_named(right, n, "interval");
        if (!rc) return rc.error();
        if (left.columns[*lc].type() != right.columns[*rc].type())
            return invalid_argument<"interval: equi column types differ">();
        skip.push_back(*rc);
    }

    dstd::Result<dstd::Vector<ColumnView>> lviews =
        ops::views_of(left, by, "interval");
    if (!lviews) return lviews.error();
    dstd::Result<dstd::Vector<ColumnView>> rviews =
        ops::views_of(right, by, "interval");
    if (!rviews) return rviews.error();
    dstd::Vector<ColumnView> lequi = std::move(*lviews);
    dstd::Vector<ColumnView> requi = std::move(*rviews);
    dstd::Vector<ColumnView> lkeys = lequi;
    dstd::Vector<ColumnView> rkeys = requi;
    lkeys.emplace_back(left.columns[p_at]);
    rkeys.emplace_back(right.columns[lo_at]);
    const ColumnView& points = lkeys.back();
    const ColumnView& los = rkeys.back();
    const ColumnView his(right.columns[hi_at]);

    const std::int64_t L = left.num_rows();
    const std::int64_t R = right.num_rows();
    const dstd::Vector<std::int64_t> lorder = order_rows(lkeys, L);
    const dstd::Vector<std::int64_t> rorder = order_rows(rkeys, R);

    dstd::Vector<RightPartition> rparts;
    for (std::int64_t i = 0; i < R;) {
        const std::int64_t j = run_end(requi, rorder, i);
        std::int64_t le = i;
        while (le < j && !los.is_missing(rorder[static_cast<std::size_t>(le)]))
            ++le;
        rparts.push_back({rorder[static_cast<std::size_t>(i)], i, le});
        i = j;
    }

    dstd::Vector<std::int64_t> left_rows;
    dstd::Vector<std::int64_t> right_rows;
    // A min-heap on hi holds the intervals that started at or before the
    // point; those that ended before it are dropped, so the rest contain it.
    const auto hi_greater = [&](std::int64_t x, std::int64_t y) {
        return his.compare(x, y) > 0;
    };
    dstd::Vector<std::int64_t> active;
    dstd::Vector<std::int64_t> ordered;
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
        const std::int64_t rloend = matched ? rparts[rp].lo_end : 0;

        active.clear();
        bool dirty = true;
        std::int64_t add = rstart;
        for (std::int64_t a = li; a < lj; ++a) {
            const std::int64_t lr = lorder[static_cast<std::size_t>(a)];
            const bool has_point = matched && !points.is_missing(lr);
            if (has_point) {
                while (add < rloend &&
                       los.compare(rorder[static_cast<std::size_t>(add)],
                                   points, lr) <= 0) {
                    const std::int64_t rr =
                        rorder[static_cast<std::size_t>(add)];
                    if (!his.is_missing(rr)) {
                        active.push_back(rr);
                        std::push_heap(active.begin(), active.end(),
                                       hi_greater);
                        dirty = true;
                    }
                    ++add;
                }
                while (!active.empty() &&
                       his.compare(active.front(), points, lr) < 0) {
                    std::pop_heap(active.begin(), active.end(), hi_greater);
                    active.pop_back();
                    dirty = true;
                }
            }
            if (!has_point || active.empty()) {
                if (outer) {
                    left_rows.push_back(lr);
                    right_rows.push_back(-1);
                }
                continue;
            }
            if (dirty) {
                ordered.assign(active.begin(), active.end());
                std::sort(ordered.begin(), ordered.end(),
                          [&](std::int64_t x, std::int64_t y) {
                              int c = los.compare(x, y);
                              if (c != 0) return c < 0;
                              c = his.compare(x, y);
                              if (c != 0) return c < 0;
                              return x < y;
                          });
                dirty = false;
            }
            for (const std::int64_t rr : ordered) {
                left_rows.push_back(lr);
                right_rows.push_back(rr);
            }
        }
        li = lj;
    }
    return ops::join_columns(left, right, skip, left_rows, right_rows,
                             "interval");
}

}  // namespace dftracer::utils::dataframe
