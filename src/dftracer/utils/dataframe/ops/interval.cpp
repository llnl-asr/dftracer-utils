#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/kernels/order.h>

#include <algorithm>
#include <stdexcept>

#include "ops_common.h"

namespace dftracer::utils::dataframe {

namespace {

struct RightPartition {
    std::int64_t rep;
    std::int64_t start;
    std::int64_t lo_end;  // the first null-lo row, which sorts last
};

}  // namespace

DataFrame interval(const DataFrame& left, const DataFrame& right,
                   const std::string& point, const std::string& lo,
                   const std::string& hi, const std::vector<std::string>& by,
                   bool outer) {
    const std::size_t p_at = ops::column_named(left, point, "interval");
    const std::size_t lo_at = ops::column_named(right, lo, "interval");
    const std::size_t hi_at = ops::column_named(right, hi, "interval");
    const TypeId type = left.columns[p_at].type();
    if (type != right.columns[lo_at].type() ||
        type != right.columns[hi_at].type())
        throw std::invalid_argument("interval: point/lo/hi types differ");
    if (!ColumnView::supports(type) ||
        ColumnView(left.columns[p_at]).is_bytes())
        throw std::invalid_argument(
            "interval: point/lo/hi columns must be numeric");
    std::vector<std::size_t> skip{lo_at, hi_at};
    for (const std::string& n : by) {
        const std::size_t l = ops::column_named(left, n, "interval");
        const std::size_t r = ops::column_named(right, n, "interval");
        if (left.columns[l].type() != right.columns[r].type())
            throw std::invalid_argument("interval: equi column types differ");
        skip.push_back(r);
    }

    std::vector<ColumnView> lequi = ops::views_of(left, by, "interval");
    std::vector<ColumnView> requi = ops::views_of(right, by, "interval");
    std::vector<ColumnView> lkeys = lequi;
    std::vector<ColumnView> rkeys = requi;
    lkeys.emplace_back(left.columns[p_at]);
    rkeys.emplace_back(right.columns[lo_at]);
    const ColumnView& points = lkeys.back();
    const ColumnView& los = rkeys.back();
    const ColumnView his(right.columns[hi_at]);

    const std::int64_t L = left.num_rows();
    const std::int64_t R = right.num_rows();
    const std::vector<std::int64_t> lorder = order_rows(lkeys, L);
    const std::vector<std::int64_t> rorder = order_rows(rkeys, R);

    std::vector<RightPartition> rparts;
    for (std::int64_t i = 0; i < R;) {
        const std::int64_t j = run_end(requi, rorder, i);
        std::int64_t le = i;
        while (le < j && !los.is_missing(rorder[static_cast<std::size_t>(le)]))
            ++le;
        rparts.push_back({rorder[static_cast<std::size_t>(i)], i, le});
        i = j;
    }

    std::vector<std::int64_t> left_rows;
    std::vector<std::int64_t> right_rows;
    // A min-heap on hi holds the intervals that started at or before the
    // point; those that ended before it are dropped, so the rest contain it.
    const auto hi_greater = [&](std::int64_t x, std::int64_t y) {
        return his.compare(x, y) > 0;
    };
    std::vector<std::int64_t> active;
    std::vector<std::int64_t> ordered;
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
