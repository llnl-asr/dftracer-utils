#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/error.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/utilities/common/arrow/array_view.h>
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <dftracer/utils/utilities/common/arrow/join_internal.h>
#include <dftracer/utils/utilities/common/arrow/window.h>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <deque>
#include <limits>
#include <numeric>
#include <vector>

namespace dftracer::utils::utilities::common::arrow {

namespace {

bool consumes_value(WindowFunc f) {
    switch (f) {
        case WindowFunc::ROW_NUMBER:
        case WindowFunc::RANK:
        case WindowFunc::DENSE_RANK:
        case WindowFunc::RUNNING_COUNT:
        case WindowFunc::NTILE:
        case WindowFunc::PERCENT_RANK:
        case WindowFunc::CUME_DIST:
            return false;
        default:
            return true;
    }
}

struct WState {
    WindowFunc func;
    std::size_t out_col;
    const ArrowArrayView* vview = nullptr;
    const ValueCol* vc = nullptr;
    KeyKind vkind = KeyKind::SIGNED;
    std::int64_t offset = 0;
    bool sum_double = false;
    std::int64_t sum_i = 0;
    double sum_d = 0.0;
    double prod_d = 1.0;
    std::int64_t ext_row = -1;
    std::int64_t rank_val = 0;
    const ArrowArrayView* tview = nullptr;
    double threshold = 0.0;
    bool counter = false;
    std::int64_t session_id = 0;
    std::int64_t frame_preceding = 0;
    std::int64_t frame_following = 0;
    std::int64_t win_lo = 0;
    std::int64_t win_hi = -1;
    std::int64_t frame_cnt = 0;
    std::int64_t peer_end = 0;
    bool range_mode = false;
    const ArrowArrayView* oview = nullptr;
    std::deque<std::int64_t> dq;
};

}  // namespace

ArrowExportResult window(const ArrowSchema* s, const ArrowArray* a,
                         const std::uint32_t* partition_cols,
                         std::uint32_t n_part, const std::uint32_t* order_cols,
                         std::uint32_t n_order, const WindowSpec* specs,
                         std::uint32_t n_spec) {
    ArrowArrayView av;
    if (init_array_view(av, const_cast<ArrowSchema*>(s),
                        const_cast<ArrowArray*>(a)) != NANOARROW_OK) {
        throw DFTUtilsException(ErrorCode::INTERNAL,
                                "window: failed to view batch");
    }
    struct Guard {
        ArrowArrayView* v;
        ~Guard() { ArrowArrayViewReset(v); }
    } guard{&av};

    const std::int64_t ncols = av.n_children;

    auto make_keys = [&](const std::uint32_t* cols, std::uint32_t n) {
        KeyViews keys(n);
        std::vector<KeyKind> kinds(n);
        for (std::uint32_t k = 0; k < n; ++k) {
            if (cols[k] >= static_cast<std::uint32_t>(ncols)) {
                throw DFTUtilsException(
                    ErrorCode::INVALID_ARGUMENT,
                    "window: partition/order column index out of range");
            }
            const ArrowArrayView* v = av.children[cols[k]];
            auto kk = key_kind_from_storage(v->storage_type);
            if (!kk) {
                throw DFTUtilsException(
                    ErrorCode::INVALID_ARGUMENT,
                    "window: unsupported partition/order column type");
            }
            keys[k] = v;
            kinds[k] = *kk;
        }
        return std::make_pair(std::move(keys), std::move(kinds));
    };
    auto [pkeys, pkinds] = make_keys(partition_cols, n_part);
    auto [okeys, okinds] = make_keys(order_cols, n_order);

    std::vector<ColumnSpec> out_specs;
    std::vector<ValueCol> lvals;
    for (std::int64_t c = 0; c < ncols; ++c) {
        ColumnSpec spec;
        lvals.push_back(plan_value_col(av.children[c], s->children[c],
                                       s->children[c]->name, spec));
        out_specs.push_back(std::move(spec));
    }

    const std::size_t n_input = lvals.size();
    std::vector<WState> states(n_spec);
    for (std::uint32_t j = 0; j < n_spec; ++j) {
        const WindowSpec& ws = specs[j];
        WState& st = states[j];
        st.func = ws.func;
        st.out_col = n_input + j;
        st.offset = ws.offset;

        if (ws.func == WindowFunc::DELTA || ws.func == WindowFunc::RATE ||
            ws.func == WindowFunc::SESSIONIZE) {
            st.counter = ws.counter;
            st.threshold = ws.threshold;
            if (ws.func != WindowFunc::SESSIONIZE) {
                if (ws.value_col >= static_cast<std::uint32_t>(ncols)) {
                    throw DFTUtilsException(
                        ErrorCode::INVALID_ARGUMENT,
                        "window: value column index out of range");
                }
                st.vview = av.children[ws.value_col];
                st.vc = &lvals[ws.value_col];
                auto vk = key_kind_from_storage(st.vview->storage_type);
                if (!vk || *vk == KeyKind::BYTES) {
                    throw DFTUtilsException(
                        ErrorCode::INVALID_ARGUMENT,
                        "window: DELTA/RATE over a non-numeric column");
                }
                st.vkind = *vk;
            }
            if (ws.func != WindowFunc::DELTA) {
                if (ws.time_col >= static_cast<std::uint32_t>(ncols)) {
                    throw DFTUtilsException(
                        ErrorCode::INVALID_ARGUMENT,
                        "window: time column index out of range");
                }
                st.tview = av.children[ws.time_col];
                auto tk = key_kind_from_storage(st.tview->storage_type);
                if (!tk || *tk == KeyKind::BYTES) {
                    throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                            "window: RATE/SESSIONIZE over a "
                                            "non-numeric time column");
                }
            }
            if (ws.func == WindowFunc::DELTA) {
                out_specs.push_back({ws.name, st.vkind == KeyKind::FLOAT
                                                  ? ColumnType::DOUBLE
                                                  : ColumnType::INT64});
            } else if (ws.func == WindowFunc::RATE) {
                out_specs.push_back({ws.name, ColumnType::DOUBLE});
            } else {
                out_specs.push_back({ws.name, ColumnType::INT64});
            }
            continue;
        }

        if (ws.func == WindowFunc::PERCENT_RANK ||
            ws.func == WindowFunc::CUME_DIST) {
            out_specs.push_back({ws.name, ColumnType::DOUBLE});
            continue;
        }
        if (!consumes_value(ws.func)) {
            out_specs.push_back({ws.name, ColumnType::INT64});
            continue;
        }
        if (ws.value_col >= static_cast<std::uint32_t>(ncols)) {
            throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                    "window: value column index out of range");
        }
        st.vview = av.children[ws.value_col];
        st.vc = &lvals[ws.value_col];

        if (ws.func == WindowFunc::LAG || ws.func == WindowFunc::LEAD ||
            ws.func == WindowFunc::FIRST_VALUE ||
            ws.func == WindowFunc::LAST_VALUE ||
            ws.func == WindowFunc::NTH_VALUE ||
            ws.func == WindowFunc::FILL_FORWARD) {
            ColumnSpec copy = out_specs[ws.value_col];
            copy.name = ws.name;
            out_specs.push_back(std::move(copy));
            continue;
        }

        if (ws.func == WindowFunc::FRAME_SUM ||
            ws.func == WindowFunc::FRAME_MIN ||
            ws.func == WindowFunc::FRAME_MAX ||
            ws.func == WindowFunc::FRAME_COUNT ||
            ws.func == WindowFunc::FRAME_MEAN) {
            st.frame_preceding = ws.frame_preceding;
            st.frame_following = ws.frame_following;
            if (ws.frame_mode == WindowFrameMode::RANGE) {
                if (n_order != 1 || okinds[0] == KeyKind::BYTES) {
                    throw DFTUtilsException(
                        ErrorCode::INVALID_ARGUMENT,
                        "window: RANGE frame needs exactly one numeric order "
                        "column");
                }
                st.range_mode = true;
                st.oview = okeys[0];
            }
            if (ws.func == WindowFunc::FRAME_COUNT) {
                out_specs.push_back({ws.name, ColumnType::INT64});
                continue;
            }
            auto fk = key_kind_from_storage(st.vview->storage_type);
            if (!fk) {
                throw DFTUtilsException(
                    ErrorCode::INVALID_ARGUMENT,
                    "window: frame aggregate over a non-numeric column");
            }
            st.vkind = *fk;
            if (ws.func == WindowFunc::FRAME_MEAN) {
                if (*fk == KeyKind::BYTES) {
                    throw DFTUtilsException(
                        ErrorCode::INVALID_ARGUMENT,
                        "window: FRAME_MEAN over a non-numeric column");
                }
                out_specs.push_back({ws.name, ColumnType::DOUBLE});
            } else if (ws.func == WindowFunc::FRAME_SUM) {
                if (*fk == KeyKind::BYTES) {
                    throw DFTUtilsException(
                        ErrorCode::INVALID_ARGUMENT,
                        "window: FRAME_SUM over a non-numeric column");
                }
                st.sum_double = *fk == KeyKind::FLOAT;
                out_specs.push_back({ws.name, st.sum_double
                                                  ? ColumnType::DOUBLE
                                                  : ColumnType::INT64});
            } else {  // FRAME_MIN / FRAME_MAX
                out_specs.push_back({ws.name, out_specs[ws.value_col].type});
            }
            continue;
        }

        auto kk = key_kind_from_storage(st.vview->storage_type);
        if (!kk || ((ws.func == WindowFunc::RUNNING_SUM ||
                     ws.func == WindowFunc::RUNNING_PROD) &&
                    *kk == KeyKind::BYTES)) {
            throw DFTUtilsException(
                ErrorCode::INVALID_ARGUMENT,
                "window: running aggregate over a non-numeric column");
        }
        st.vkind = *kk;
        if (ws.func == WindowFunc::RUNNING_SUM) {
            st.sum_double = *kk == KeyKind::FLOAT;
            out_specs.push_back({ws.name, st.sum_double ? ColumnType::DOUBLE
                                                        : ColumnType::INT64});
        } else if (ws.func == WindowFunc::RUNNING_PROD) {
            out_specs.push_back({ws.name, ColumnType::DOUBLE});
        } else {  // RUNNING_MIN / RUNNING_MAX
            out_specs.push_back({ws.name, out_specs[ws.value_col].type});
        }
    }

    RecordBatchBuilder b;
    b.declare_schema(out_specs);

    const std::int64_t N = av.length;
    std::vector<std::int64_t> idx(static_cast<std::size_t>(N));
    std::iota(idx.begin(), idx.end(), 0);

    auto null_last_cmp = [&](const ArrowArrayView* v, KeyKind k, std::int64_t x,
                             std::int64_t y) {
        bool nx = ArrowArrayViewIsNull(v, x);
        bool ny = ArrowArrayViewIsNull(v, y);
        if (nx || ny) return nx && ny ? 0 : (nx ? 1 : -1);
        return cell_cmp(v, x, v, y, k);
    };
    std::sort(idx.begin(), idx.end(), [&](std::int64_t x, std::int64_t y) {
        for (std::uint32_t k = 0; k < n_part; ++k) {
            int c = null_last_cmp(pkeys[k], pkinds[k], x, y);
            if (c != 0) return c < 0;
        }
        for (std::uint32_t k = 0; k < n_order; ++k) {
            int c = null_last_cmp(okeys[k], okinds[k], x, y);
            if (c != 0) return c < 0;
        }
        return x < y;
    });

    auto same_partition = [&](std::int64_t x, std::int64_t y) {
        for (std::uint32_t k = 0; k < n_part; ++k) {
            bool nx = ArrowArrayViewIsNull(pkeys[k], x);
            bool ny = ArrowArrayViewIsNull(pkeys[k], y);
            if (nx != ny) return false;
            if (nx) continue;
            if (cell_cmp(pkeys[k], x, pkeys[k], y, pkinds[k]) != 0)
                return false;
        }
        return true;
    };
    auto order_equal = [&](std::int64_t x, std::int64_t y) {
        for (std::uint32_t k = 0; k < n_order; ++k) {
            bool nx = ArrowArrayViewIsNull(okeys[k], x);
            bool ny = ArrowArrayViewIsNull(okeys[k], y);
            if (nx != ny) return false;
            if (nx) continue;
            if (cell_cmp(okeys[k], x, okeys[k], y, okinds[k]) != 0)
                return false;
        }
        return true;
    };

    auto frame_range = [](const WState& st, std::int64_t local,
                          std::int64_t sz) {
        std::int64_t lo =
            st.frame_preceding == WINDOW_UNBOUNDED
                ? 0
                : std::max<std::int64_t>(0, local - st.frame_preceding);
        std::int64_t hi =
            st.frame_following == WINDOW_UNBOUNDED
                ? sz - 1
                : std::min<std::int64_t>(sz - 1, local + st.frame_following);
        return std::make_pair(lo, hi);
    };

    // `lo`/`hi` are monotonically non-decreasing as the row advances, so each
    // position enters and leaves at most once (O(n) amortized). The accumulator
    // holds the frame's running sum/count; the deque holds positions in value-
    // monotonic order so its front is the non-null MIN/MAX.
    auto frame_acc = [&](WState& st, std::int64_t p, std::int64_t pos,
                         std::int64_t sign) {
        const std::int64_t g = idx[static_cast<std::size_t>(p + pos)];
        if (ArrowArrayViewIsNull(st.vview, g)) return;
        st.frame_cnt += sign;
        if (st.func == WindowFunc::FRAME_COUNT) return;
        if (st.func == WindowFunc::FRAME_MEAN) {
            st.sum_d += static_cast<double>(sign) *
                        ArrowArrayViewGetDoubleUnsafe(st.vview, g);
            return;
        }
        if (st.sum_double)
            st.sum_d += static_cast<double>(sign) *
                        ArrowArrayViewGetDoubleUnsafe(st.vview, g);
        else if (st.vkind == KeyKind::UNSIGNED)
            st.sum_i += sign * static_cast<std::int64_t>(
                                   ArrowArrayViewGetUIntUnsafe(st.vview, g));
        else
            st.sum_i += sign * ArrowArrayViewGetIntUnsafe(st.vview, g);
    };
    auto frame_deque_add = [&](WState& st, std::int64_t p, std::int64_t pos) {
        const std::int64_t g = idx[static_cast<std::size_t>(p + pos)];
        if (ArrowArrayViewIsNull(st.vview, g)) return;
        ++st.frame_cnt;
        while (!st.dq.empty()) {
            const std::int64_t gb =
                idx[static_cast<std::size_t>(p + st.dq.back())];
            int c = cell_cmp(st.vview, g, st.vview, gb, st.vkind);
            if (st.func == WindowFunc::FRAME_MIN ? c <= 0 : c >= 0)
                st.dq.pop_back();
            else
                break;
        }
        st.dq.push_back(pos);
    };

    // RANGE bounds advance the value window over the sorted order column: lo is
    // the first position with order >= lower value, hi the last with order <=
    // upper value (peers inclusive on both ends). Monotonic in value, so the
    // ROWS sliding accumulator/deque applies unchanged.
    auto frame_bounds = [&](WState& st, std::int64_t p, std::int64_t local,
                            std::int64_t sz, std::int64_t g) {
        if (!st.range_mode) return frame_range(st, local, sz);
        const double ov = ArrowArrayViewGetDoubleUnsafe(st.oview, g);
        const double inf = std::numeric_limits<double>::infinity();
        const double lo_val =
            st.frame_preceding == WINDOW_UNBOUNDED
                ? -inf
                : ov - static_cast<double>(st.frame_preceding);
        const double hi_val =
            st.frame_following == WINDOW_UNBOUNDED
                ? inf
                : ov + static_cast<double>(st.frame_following);
        auto od = [&](std::int64_t lp) {
            return ArrowArrayViewGetDoubleUnsafe(
                st.oview, idx[static_cast<std::size_t>(p + lp)]);
        };
        std::int64_t lo = st.win_lo;
        while (lo < sz && od(lo) < lo_val) ++lo;
        std::int64_t hi = std::max(st.win_hi, local);
        while (hi + 1 < sz && od(hi + 1) <= hi_val) ++hi;
        return std::make_pair(lo, hi);
    };

    for (std::int64_t p = 0; p < N;) {
        std::int64_t q = p + 1;
        while (q < N && same_partition(idx[static_cast<std::size_t>(p)],
                                       idx[static_cast<std::size_t>(q)]))
            ++q;

        for (auto& st : states) {
            st.sum_i = 0;
            st.sum_d = 0.0;
            st.prod_d = 1.0;
            st.ext_row = -1;
            st.rank_val = 0;
            st.session_id = 0;
            st.win_lo = 0;
            st.win_hi = -1;
            st.frame_cnt = 0;
            st.peer_end = 0;
            st.dq.clear();
        }

        for (std::int64_t r = p; r < q; ++r) {
            const std::int64_t g = idx[static_cast<std::size_t>(r)];
            const std::int64_t local = r - p;
            for (std::size_t i = 0; i < n_input; ++i)
                append_value(b, i, lvals[i], g);

            for (auto& st : states) {
                switch (st.func) {
                    case WindowFunc::ROW_NUMBER:
                        b.append_int64(st.out_col, local + 1);
                        break;
                    case WindowFunc::RANK: {
                        if (local == 0 ||
                            !order_equal(g,
                                         idx[static_cast<std::size_t>(r - 1)]))
                            st.rank_val = local + 1;
                        b.append_int64(st.out_col, st.rank_val);
                        break;
                    }
                    case WindowFunc::DENSE_RANK: {
                        if (local == 0)
                            st.rank_val = 1;
                        else if (!order_equal(
                                     g, idx[static_cast<std::size_t>(r - 1)]))
                            ++st.rank_val;
                        b.append_int64(st.out_col, st.rank_val);
                        break;
                    }
                    case WindowFunc::PERCENT_RANK: {
                        if (local == 0 ||
                            !order_equal(g,
                                         idx[static_cast<std::size_t>(r - 1)]))
                            st.rank_val = local + 1;
                        const std::int64_t sz = q - p;
                        b.append_double(
                            st.out_col,
                            sz > 1 ? static_cast<double>(st.rank_val - 1) /
                                         static_cast<double>(sz - 1)
                                   : 0.0);
                        break;
                    }
                    case WindowFunc::CUME_DIST: {
                        if (local >= st.peer_end) {
                            std::int64_t e = r + 1;
                            while (e < q &&
                                   order_equal(
                                       g, idx[static_cast<std::size_t>(e)]))
                                ++e;
                            st.peer_end = e - p;
                        }
                        b.append_double(st.out_col,
                                        static_cast<double>(st.peer_end) /
                                            static_cast<double>(q - p));
                        break;
                    }
                    case WindowFunc::RUNNING_COUNT:
                        b.append_int64(st.out_col, local + 1);
                        break;
                    case WindowFunc::LAG:
                    case WindowFunc::LEAD: {
                        std::int64_t src = st.func == WindowFunc::LAG
                                               ? local - st.offset
                                               : local + st.offset;
                        if (src >= 0 && src < q - p)
                            append_value(
                                b, st.out_col, *st.vc,
                                idx[static_cast<std::size_t>(p + src)]);
                        else
                            b.append_null(st.out_col);
                        break;
                    }
                    case WindowFunc::RUNNING_SUM: {
                        if (!ArrowArrayViewIsNull(st.vview, g)) {
                            if (st.sum_double)
                                st.sum_d +=
                                    ArrowArrayViewGetDoubleUnsafe(st.vview, g);
                            else if (st.vkind == KeyKind::UNSIGNED)
                                st.sum_i += static_cast<std::int64_t>(
                                    ArrowArrayViewGetUIntUnsafe(st.vview, g));
                            else
                                st.sum_i +=
                                    ArrowArrayViewGetIntUnsafe(st.vview, g);
                        }
                        if (st.sum_double)
                            b.append_double(st.out_col, st.sum_d);
                        else
                            b.append_int64(st.out_col, st.sum_i);
                        break;
                    }
                    case WindowFunc::RUNNING_PROD:
                        if (!ArrowArrayViewIsNull(st.vview, g))
                            st.prod_d *=
                                ArrowArrayViewGetDoubleUnsafe(st.vview, g);
                        b.append_double(st.out_col, st.prod_d);
                        break;
                    case WindowFunc::RUNNING_MIN:
                    case WindowFunc::RUNNING_MAX: {
                        if (!ArrowArrayViewIsNull(st.vview, g)) {
                            if (st.ext_row < 0) {
                                st.ext_row = g;
                            } else {
                                int c = cell_cmp(st.vview, g, st.vview,
                                                 st.ext_row, st.vkind);
                                if ((st.func == WindowFunc::RUNNING_MIN &&
                                     c < 0) ||
                                    (st.func == WindowFunc::RUNNING_MAX &&
                                     c > 0))
                                    st.ext_row = g;
                            }
                        }
                        if (st.ext_row >= 0)
                            append_value(b, st.out_col, *st.vc, st.ext_row);
                        else
                            b.append_null(st.out_col);
                        break;
                    }
                    case WindowFunc::DELTA: {
                        if (local == 0) {
                            b.append_null(st.out_col);
                            break;
                        }
                        const std::int64_t prev =
                            idx[static_cast<std::size_t>(r - 1)];
                        if (ArrowArrayViewIsNull(st.vview, g) ||
                            ArrowArrayViewIsNull(st.vview, prev)) {
                            b.append_null(st.out_col);
                            break;
                        }
                        if (st.vkind == KeyKind::FLOAT)
                            b.append_double(
                                st.out_col,
                                ArrowArrayViewGetDoubleUnsafe(st.vview, g) -
                                    ArrowArrayViewGetDoubleUnsafe(st.vview,
                                                                  prev));
                        else if (st.vkind == KeyKind::UNSIGNED)
                            b.append_int64(
                                st.out_col,
                                static_cast<std::int64_t>(
                                    ArrowArrayViewGetUIntUnsafe(st.vview, g) -
                                    ArrowArrayViewGetUIntUnsafe(st.vview,
                                                                prev)));
                        else
                            b.append_int64(
                                st.out_col,
                                ArrowArrayViewGetIntUnsafe(st.vview, g) -
                                    ArrowArrayViewGetIntUnsafe(st.vview, prev));
                        break;
                    }
                    case WindowFunc::RATE: {
                        if (local == 0) {
                            b.append_null(st.out_col);
                            break;
                        }
                        const std::int64_t prev =
                            idx[static_cast<std::size_t>(r - 1)];
                        if (ArrowArrayViewIsNull(st.vview, g) ||
                            ArrowArrayViewIsNull(st.vview, prev) ||
                            ArrowArrayViewIsNull(st.tview, g) ||
                            ArrowArrayViewIsNull(st.tview, prev)) {
                            b.append_null(st.out_col);
                            break;
                        }
                        double dt =
                            ArrowArrayViewGetDoubleUnsafe(st.tview, g) -
                            ArrowArrayViewGetDoubleUnsafe(st.tview, prev);
                        if (dt == 0.0) {
                            b.append_null(st.out_col);
                            break;
                        }
                        double cur = ArrowArrayViewGetDoubleUnsafe(st.vview, g);
                        double pv =
                            ArrowArrayViewGetDoubleUnsafe(st.vview, prev);
                        double dv = (st.counter && cur < pv) ? cur : cur - pv;
                        b.append_double(st.out_col, dv / dt);
                        break;
                    }
                    case WindowFunc::SESSIONIZE: {
                        if (local == 0) {
                            st.session_id = 1;
                        } else {
                            const std::int64_t prev =
                                idx[static_cast<std::size_t>(r - 1)];
                            if (!ArrowArrayViewIsNull(st.tview, g) &&
                                !ArrowArrayViewIsNull(st.tview, prev)) {
                                double gap =
                                    ArrowArrayViewGetDoubleUnsafe(st.tview, g) -
                                    ArrowArrayViewGetDoubleUnsafe(st.tview,
                                                                  prev);
                                if (gap > st.threshold) ++st.session_id;
                            }
                        }
                        b.append_int64(st.out_col, st.session_id);
                        break;
                    }
                    case WindowFunc::NTILE: {
                        const std::int64_t sz = q - p;
                        const std::int64_t n = st.offset;
                        if (n <= 0) {
                            b.append_null(st.out_col);
                            break;
                        }
                        const std::int64_t base = sz / n;
                        const std::int64_t rem = sz % n;
                        const std::int64_t big = rem * (base + 1);
                        std::int64_t bucket;
                        if (local < big)
                            bucket = local / (base + 1) + 1;
                        else
                            bucket = rem + (local - big) / base + 1;
                        b.append_int64(st.out_col, bucket);
                        break;
                    }
                    case WindowFunc::FIRST_VALUE:
                        append_value(b, st.out_col, *st.vc,
                                     idx[static_cast<std::size_t>(p)]);
                        break;
                    case WindowFunc::LAST_VALUE:
                        append_value(b, st.out_col, *st.vc,
                                     idx[static_cast<std::size_t>(q - 1)]);
                        break;
                    case WindowFunc::FILL_FORWARD:
                        if (!ArrowArrayViewIsNull(st.vview, g)) st.ext_row = g;
                        if (st.ext_row >= 0)
                            append_value(b, st.out_col, *st.vc, st.ext_row);
                        else
                            b.append_null(st.out_col);
                        break;
                    case WindowFunc::NTH_VALUE: {
                        const std::int64_t k = st.offset;
                        if (k >= 1 && k <= q - p)
                            append_value(
                                b, st.out_col, *st.vc,
                                idx[static_cast<std::size_t>(p + k - 1)]);
                        else
                            b.append_null(st.out_col);
                        break;
                    }
                    case WindowFunc::FRAME_SUM:
                    case WindowFunc::FRAME_MEAN:
                    case WindowFunc::FRAME_COUNT: {
                        auto [lo, hi] = frame_bounds(st, p, local, q - p, g);
                        for (std::int64_t pos = st.win_hi + 1; pos <= hi; ++pos)
                            frame_acc(st, p, pos, 1);
                        for (std::int64_t pos = st.win_lo; pos < lo; ++pos)
                            frame_acc(st, p, pos, -1);
                        st.win_lo = lo;
                        st.win_hi = hi;
                        if (st.func == WindowFunc::FRAME_COUNT)
                            b.append_int64(st.out_col, st.frame_cnt);
                        else if (st.frame_cnt == 0 || st.frame_cnt < st.offset)
                            b.append_null(st.out_col);
                        else if (st.func == WindowFunc::FRAME_MEAN)
                            b.append_double(
                                st.out_col,
                                st.sum_d / static_cast<double>(st.frame_cnt));
                        else if (st.sum_double)
                            b.append_double(st.out_col, st.sum_d);
                        else
                            b.append_int64(st.out_col, st.sum_i);
                        break;
                    }
                    case WindowFunc::FRAME_MIN:
                    case WindowFunc::FRAME_MAX: {
                        auto [lo, hi] = frame_bounds(st, p, local, q - p, g);
                        for (std::int64_t pos = st.win_hi + 1; pos <= hi; ++pos)
                            frame_deque_add(st, p, pos);
                        for (std::int64_t pos = st.win_lo; pos < lo; ++pos)
                            if (!ArrowArrayViewIsNull(
                                    st.vview,
                                    idx[static_cast<std::size_t>(p + pos)]))
                                --st.frame_cnt;
                        while (!st.dq.empty() && st.dq.front() < lo)
                            st.dq.pop_front();
                        st.win_lo = lo;
                        st.win_hi = hi;
                        if (st.dq.empty() || st.frame_cnt < st.offset)
                            b.append_null(st.out_col);
                        else
                            append_value(b, st.out_col, *st.vc,
                                         idx[static_cast<std::size_t>(
                                             p + st.dq.front())]);
                        break;
                    }
                }
            }
            b.end_row();
        }
        p = q;
    }

    return b.finish();
}

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
