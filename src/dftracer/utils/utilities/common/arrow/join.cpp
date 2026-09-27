#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/error.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/utilities/common/arrow/array_view.h>
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <dftracer/utils/utilities/common/arrow/join.h>
#include <dftracer/utils/utilities/common/arrow/join_internal.h>
#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::utilities::common::arrow {

namespace {

// Compare two ts cells of the same numeric KeyKind; returns <0, 0, >0.
int ts_cmp(const ArrowArrayView* a, std::int64_t ia, const ArrowArrayView* b,
           std::int64_t ib, KeyKind k) {
    switch (k) {
        case KeyKind::UNSIGNED: {
            std::uint64_t x = ArrowArrayViewGetUIntUnsafe(a, ia);
            std::uint64_t y = ArrowArrayViewGetUIntUnsafe(b, ib);
            return x < y ? -1 : (x > y ? 1 : 0);
        }
        case KeyKind::FLOAT: {
            double x = ArrowArrayViewGetDoubleUnsafe(a, ia);
            double y = ArrowArrayViewGetDoubleUnsafe(b, ib);
            return x < y ? -1 : (x > y ? 1 : 0);
        }
        default: {  // SIGNED
            std::int64_t x = ArrowArrayViewGetIntUnsafe(a, ia);
            std::int64_t y = ArrowArrayViewGetIntUnsafe(b, ib);
            return x < y ? -1 : (x > y ? 1 : 0);
        }
    }
}

// |left - right| <= tol, in the ts column's native units. tol < 0 rejects.
bool ts_within_tol(const ArrowArrayView* l, std::int64_t il,
                   const ArrowArrayView* r, std::int64_t ir, KeyKind k,
                   std::int64_t tol) {
    switch (k) {
        case KeyKind::UNSIGNED: {
            std::uint64_t x = ArrowArrayViewGetUIntUnsafe(l, il);
            std::uint64_t y = ArrowArrayViewGetUIntUnsafe(r, ir);
            std::uint64_t d = x >= y ? x - y : y - x;
            return tol >= 0 && d <= static_cast<std::uint64_t>(tol);
        }
        case KeyKind::FLOAT: {
            double d = ArrowArrayViewGetDoubleUnsafe(l, il) -
                       ArrowArrayViewGetDoubleUnsafe(r, ir);
            if (d < 0) d = -d;
            return tol >= 0 && d <= static_cast<double>(tol);
        }
        default: {  // SIGNED
            std::int64_t x = ArrowArrayViewGetIntUnsafe(l, il);
            std::int64_t y = ArrowArrayViewGetIntUnsafe(r, ir);
            std::uint64_t d = x >= y ? static_cast<std::uint64_t>(x) -
                                           static_cast<std::uint64_t>(y)
                                     : static_cast<std::uint64_t>(y) -
                                           static_cast<std::uint64_t>(x);
            return tol >= 0 && d <= static_cast<std::uint64_t>(tol);
        }
    }
}

// |left - r[a]| <= |left - r[b]| (both candidates share the right view). Used
// by NEAREST to prefer the closer row, with a tie resolving to the first
// argument.
bool ts_le_dist(const ArrowArrayView* l, std::int64_t il,
                const ArrowArrayView* r, std::int64_t ia, std::int64_t ib,
                KeyKind k) {
    switch (k) {
        case KeyKind::UNSIGNED: {
            std::uint64_t x = ArrowArrayViewGetUIntUnsafe(l, il);
            std::uint64_t a = ArrowArrayViewGetUIntUnsafe(r, ia);
            std::uint64_t b = ArrowArrayViewGetUIntUnsafe(r, ib);
            std::uint64_t da = x >= a ? x - a : a - x;
            std::uint64_t db = x >= b ? x - b : b - x;
            return da <= db;
        }
        case KeyKind::FLOAT: {
            double x = ArrowArrayViewGetDoubleUnsafe(l, il);
            double da = x - ArrowArrayViewGetDoubleUnsafe(r, ia);
            double db = x - ArrowArrayViewGetDoubleUnsafe(r, ib);
            if (da < 0) da = -da;
            if (db < 0) db = -db;
            return da <= db;
        }
        default: {  // SIGNED
            std::int64_t x = ArrowArrayViewGetIntUnsafe(l, il);
            std::int64_t a = ArrowArrayViewGetIntUnsafe(r, ia);
            std::int64_t b = ArrowArrayViewGetIntUnsafe(r, ib);
            std::uint64_t da = x >= a ? static_cast<std::uint64_t>(x) -
                                            static_cast<std::uint64_t>(a)
                                      : static_cast<std::uint64_t>(a) -
                                            static_cast<std::uint64_t>(x);
            std::uint64_t db = x >= b ? static_cast<std::uint64_t>(x) -
                                            static_cast<std::uint64_t>(b)
                                      : static_cast<std::uint64_t>(b) -
                                            static_cast<std::uint64_t>(x);
            return da <= db;
        }
    }
}

}  // namespace

ArrowExportResult join(const ArrowSchema* left_s, const ArrowArray* left_a,
                       const std::uint32_t* left_key_cols,
                       const ArrowSchema* right_s, const ArrowArray* right_a,
                       const std::uint32_t* right_key_cols, std::uint32_t nkey,
                       JoinType type) {
    if (nkey == 0) {
        throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                "join: nkey must be >= 1");
    }

    ArrowArrayView lav;
    ArrowArrayView rav;
    if (init_array_view(lav, const_cast<ArrowSchema*>(left_s),
                        const_cast<ArrowArray*>(left_a)) != NANOARROW_OK) {
        throw DFTUtilsException(ErrorCode::INTERNAL,
                                "join: failed to view left batch");
    }
    struct LGuard {
        ArrowArrayView* v;
        ~LGuard() { ArrowArrayViewReset(v); }
    } lg{&lav};
    if (init_array_view(rav, const_cast<ArrowSchema*>(right_s),
                        const_cast<ArrowArray*>(right_a)) != NANOARROW_OK) {
        throw DFTUtilsException(ErrorCode::INTERNAL,
                                "join: failed to view right batch");
    }
    struct RGuard {
        ArrowArrayView* v;
        ~RGuard() { ArrowArrayViewReset(v); }
    } rg{&rav};

    const std::int64_t lcols = lav.n_children;
    const std::int64_t rcols = rav.n_children;

    KeyViews lkeys(nkey);
    KeyViews rkeys(nkey);
    std::vector<KeyKind> kinds(nkey);
    std::unordered_set<std::size_t> left_key_set;
    std::unordered_set<std::size_t> right_key_set;
    for (std::uint32_t k = 0; k < nkey; ++k) {
        if (left_key_cols[k] >= static_cast<std::uint32_t>(lcols) ||
            right_key_cols[k] >= static_cast<std::uint32_t>(rcols)) {
            throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                    "join: key column index out of range");
        }
        const ArrowArrayView* lv = lav.children[left_key_cols[k]];
        const ArrowArrayView* rv = rav.children[right_key_cols[k]];
        if (lv->storage_type != rv->storage_type) {
            throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                    "join: key column types differ");
        }
        auto kk = key_kind_from_storage(lv->storage_type);
        if (!kk) {
            throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                    "join: unsupported key column type");
        }
        lkeys[k] = lv;
        rkeys[k] = rv;
        kinds[k] = *kk;
        left_key_set.insert(left_key_cols[k]);
        right_key_set.insert(right_key_cols[k]);
    }

    const bool left_only =
        type == JoinType::LEFT_SEMI || type == JoinType::LEFT_ANTI;

    // Output schema: left keys, left values, right values; a colliding right
    // value name gets a "_right" suffix. SEMI/ANTI emit a left-only schema
    // (no right value columns).
    std::vector<ColumnSpec> specs;
    std::vector<ValueCol> lvals;
    std::vector<ValueCol> rvals;
    std::unordered_set<std::string> names;

    for (std::uint32_t k = 0; k < nkey; ++k) {
        auto kt = scalar_type_from_storage(lkeys[k]->storage_type);
        const char* nm = left_s->children[left_key_cols[k]]->name;
        std::string name = nm ? nm : "";
        specs.push_back({name, *kt});
        names.insert(name);
    }
    for (std::int64_t c = 0; c < lcols; ++c) {
        if (left_key_set.count(static_cast<std::size_t>(c))) continue;
        ColumnSpec spec;
        lvals.push_back(plan_value_col(lav.children[c], left_s->children[c],
                                       left_s->children[c]->name, spec));
        names.insert(spec.name);
        specs.push_back(std::move(spec));
    }
    for (std::int64_t c = 0; !left_only && c < rcols; ++c) {
        if (right_key_set.count(static_cast<std::size_t>(c))) continue;
        ColumnSpec spec;
        rvals.push_back(plan_value_col(rav.children[c], right_s->children[c],
                                       right_s->children[c]->name, spec));
        if (names.count(spec.name)) {
            spec.name += "_right";
            if (names.count(spec.name)) {
                throw DFTUtilsException(
                    ErrorCode::INVALID_ARGUMENT,
                    std::string("join: output column name collision: ")
                        .append(spec.name));
            }
        }
        names.insert(spec.name);
        specs.push_back(std::move(spec));
    }

    const std::size_t n_left_val = lvals.size();
    const std::size_t key_base = 0;
    const std::size_t lval_base = nkey;
    const std::size_t rval_base = nkey + n_left_val;

    RecordBatchBuilder b;
    b.declare_schema(specs);

    const std::int64_t L = lav.length;
    const std::int64_t R = rav.length;

    // Sort each side by key; null-keyed rows sort last, ties broken by original
    // row index for a stable, deterministic order.
    std::vector<std::int64_t> lorder =
        order_rows(lkeys, kinds, nullptr, KeyKind::SIGNED, L);
    std::vector<std::int64_t> rorder =
        order_rows(rkeys, kinds, nullptr, KeyKind::SIGNED, R);

    const bool semi = type == JoinType::LEFT_SEMI;
    const bool want_left = type == JoinType::LEFT || type == JoinType::FULL ||
                           type == JoinType::LEFT_ANTI;
    const bool want_right = type == JoinType::RIGHT || type == JoinType::FULL;

    auto append_key = [&](std::size_t out, ColumnType ct,
                          const ArrowArrayView* v, std::int64_t row) {
        if (ArrowArrayViewIsNull(v, row))
            b.append_null(out);
        else
            append_scalar(b, out, ct, v, row);
    };
    auto emit_left_only = [&](std::int64_t lr) {
        for (std::uint32_t k = 0; k < nkey; ++k)
            append_key(key_base + k, specs[k].type, lkeys[k], lr);
        for (std::size_t i = 0; i < lvals.size(); ++i)
            append_value(b, lval_base + i, lvals[i], lr);
        for (std::size_t i = 0; i < rvals.size(); ++i)
            b.append_null(rval_base + i);
        b.end_row();
    };
    auto emit_right_only = [&](std::int64_t rr) {
        for (std::uint32_t k = 0; k < nkey; ++k)
            append_key(key_base + k, specs[k].type, rkeys[k], rr);
        for (std::size_t i = 0; i < lvals.size(); ++i)
            b.append_null(lval_base + i);
        for (std::size_t i = 0; i < rvals.size(); ++i)
            append_value(b, rval_base + i, rvals[i], rr);
        b.end_row();
    };
    auto emit_matched = [&](std::int64_t lr, std::int64_t rr) {
        for (std::uint32_t k = 0; k < nkey; ++k)
            append_scalar(b, key_base + k, specs[k].type, lkeys[k], lr);
        for (std::size_t i = 0; i < lvals.size(); ++i)
            append_value(b, lval_base + i, lvals[i], lr);
        for (std::size_t i = 0; i < rvals.size(); ++i)
            append_value(b, rval_base + i, rvals[i], rr);
        b.end_row();
    };

    std::int64_t li = 0;
    std::int64_t ri = 0;
    while (li < L && ri < R) {
        const std::int64_t lr = lorder[static_cast<std::size_t>(li)];
        const std::int64_t rr = rorder[static_cast<std::size_t>(ri)];
        // A null key never matches (SQL semantics): treat the null-keyed row as
        // an outer-only row on its own side and advance past it.
        if (row_key_null(lkeys, lr)) {
            if (want_left) emit_left_only(lr);
            ++li;
            continue;
        }
        if (row_key_null(rkeys, rr)) {
            if (want_right) emit_right_only(rr);
            ++ri;
            continue;
        }
        int c = key_cmp(lkeys, lr, rkeys, rr, kinds);
        if (c < 0) {
            if (want_left) emit_left_only(lr);
            ++li;
        } else if (c > 0) {
            if (want_right) emit_right_only(rr);
            ++ri;
        } else {
            // Emit the full N:M cross-product of both equal-key groups,
            // materialized in memory.
            std::int64_t le = equi_partition_end(lorder, lkeys, kinds, li, L);
            std::int64_t re = equi_partition_end(rorder, rkeys, kinds, ri, R);
            if (semi) {
                for (std::int64_t a = li; a < le; ++a)
                    emit_left_only(lorder[static_cast<std::size_t>(a)]);
            } else if (!left_only) {
                for (std::int64_t a = li; a < le; ++a)
                    for (std::int64_t bb = ri; bb < re; ++bb)
                        emit_matched(lorder[static_cast<std::size_t>(a)],
                                     rorder[static_cast<std::size_t>(bb)]);
            }
            li = le;
            ri = re;
        }
    }
    for (; li < L; ++li) {
        if (want_left) emit_left_only(lorder[static_cast<std::size_t>(li)]);
    }
    for (; ri < R; ++ri) {
        if (want_right) emit_right_only(rorder[static_cast<std::size_t>(ri)]);
    }

    return b.finish();
}

ArrowExportResult asof_join(const ArrowSchema* left_s, const ArrowArray* left_a,
                            std::uint32_t left_ts_col,
                            const std::uint32_t* left_equi_cols,
                            const ArrowSchema* right_s,
                            const ArrowArray* right_a,
                            std::uint32_t right_ts_col,
                            const std::uint32_t* right_equi_cols,
                            std::uint32_t n_equi, AsofDirection dir,
                            bool has_tol, std::int64_t tol, bool allow_exact) {
    ArrowArrayView lav;
    ArrowArrayView rav;
    if (init_array_view(lav, const_cast<ArrowSchema*>(left_s),
                        const_cast<ArrowArray*>(left_a)) != NANOARROW_OK) {
        throw DFTUtilsException(ErrorCode::INTERNAL,
                                "asof_join: failed to view left batch");
    }
    struct LGuard {
        ArrowArrayView* v;
        ~LGuard() { ArrowArrayViewReset(v); }
    } lg{&lav};
    if (init_array_view(rav, const_cast<ArrowSchema*>(right_s),
                        const_cast<ArrowArray*>(right_a)) != NANOARROW_OK) {
        throw DFTUtilsException(ErrorCode::INTERNAL,
                                "asof_join: failed to view right batch");
    }
    struct RGuard {
        ArrowArrayView* v;
        ~RGuard() { ArrowArrayViewReset(v); }
    } rg{&rav};

    const std::int64_t lcols = lav.n_children;
    const std::int64_t rcols = rav.n_children;

    if (left_ts_col >= static_cast<std::uint32_t>(lcols) ||
        right_ts_col >= static_cast<std::uint32_t>(rcols)) {
        throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                "asof_join: ts column index out of range");
    }
    const ArrowArrayView* lts = lav.children[left_ts_col];
    const ArrowArrayView* rts = rav.children[right_ts_col];
    if (lts->storage_type != rts->storage_type) {
        throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                "asof_join: ts column types differ");
    }
    auto tsk = key_kind_from_storage(lts->storage_type);
    if (!tsk || *tsk == KeyKind::BYTES) {
        throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                "asof_join: ts column must be numeric");
    }
    const KeyKind ts_kind = *tsk;

    KeyViews lekeys(n_equi);
    KeyViews rekeys(n_equi);
    std::vector<KeyKind> ekinds(n_equi);
    std::unordered_set<std::size_t> right_skip;
    right_skip.insert(right_ts_col);
    for (std::uint32_t k = 0; k < n_equi; ++k) {
        if (left_equi_cols[k] >= static_cast<std::uint32_t>(lcols) ||
            right_equi_cols[k] >= static_cast<std::uint32_t>(rcols)) {
            throw DFTUtilsException(
                ErrorCode::INVALID_ARGUMENT,
                "asof_join: equi column index out of range");
        }
        const ArrowArrayView* lv = lav.children[left_equi_cols[k]];
        const ArrowArrayView* rv = rav.children[right_equi_cols[k]];
        if (lv->storage_type != rv->storage_type) {
            throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                    "asof_join: equi column types differ");
        }
        auto kk = key_kind_from_storage(lv->storage_type);
        if (!kk) {
            throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                    "asof_join: unsupported equi column type");
        }
        lekeys[k] = lv;
        rekeys[k] = rv;
        ekinds[k] = *kk;
        right_skip.insert(right_equi_cols[k]);
    }

    // Output schema: all left columns, then right value columns other than the
    // ts and equi columns; a colliding right name gets a "_right" suffix.
    std::vector<ColumnSpec> specs;
    std::vector<ValueCol> lvals;
    std::vector<ValueCol> rvals;
    std::unordered_set<std::string> names;
    for (std::int64_t c = 0; c < lcols; ++c) {
        ColumnSpec spec;
        lvals.push_back(plan_value_col(lav.children[c], left_s->children[c],
                                       left_s->children[c]->name, spec));
        names.insert(spec.name);
        specs.push_back(std::move(spec));
    }
    for (std::int64_t c = 0; c < rcols; ++c) {
        if (right_skip.count(static_cast<std::size_t>(c))) continue;
        ColumnSpec spec;
        rvals.push_back(plan_value_col(rav.children[c], right_s->children[c],
                                       right_s->children[c]->name, spec));
        if (names.count(spec.name)) {
            spec.name += "_right";
            if (names.count(spec.name)) {
                throw DFTUtilsException(
                    ErrorCode::INVALID_ARGUMENT,
                    std::string("asof_join: output column name collision: ")
                        .append(spec.name));
            }
        }
        names.insert(spec.name);
        specs.push_back(std::move(spec));
    }

    const std::size_t rval_base = lvals.size();

    RecordBatchBuilder b;
    b.declare_schema(specs);

    const std::int64_t L = lav.length;
    const std::int64_t R = rav.length;

    // Sort by (equi cols, ts). A null-equi row sorts into its own trailing
    // partition; within a partition a null ts sorts last so it can be skipped
    // without reading a null cell. Ties break by original row index.
    std::vector<std::int64_t> lorder =
        order_rows(lekeys, ekinds, lts, ts_kind, L);
    std::vector<std::int64_t> rorder =
        order_rows(rekeys, ekinds, rts, ts_kind, R);

    // Right partitions in sorted order. `ts_end` is the first null-ts row
    // (which sorts last), so [start, ts_end) are the real, sorted candidate
    // rows.
    struct RPart {
        bool null_equi;
        std::int64_t rep;
        std::int64_t start;
        std::int64_t ts_end;
        std::int64_t end;
    };
    std::vector<RPart> rparts;
    for (std::int64_t i = 0; i < R;) {
        std::int64_t rep = rorder[static_cast<std::size_t>(i)];
        bool rn = row_key_null(rekeys, rep);
        std::int64_t j = equi_partition_end(rorder, rekeys, ekinds, i, R);
        std::int64_t te = i;
        while (te < j &&
               !ArrowArrayViewIsNull(rts, rorder[static_cast<std::size_t>(te)]))
            ++te;
        rparts.push_back({rn, rep, i, te, j});
        i = j;
    }

    // Left partition vs a right partition by equi key (null-equi sorts last).
    auto part_cmp = [&](bool lnull, std::int64_t lrep, const RPart& rp) {
        if (lnull && rp.null_equi) return 0;
        if (lnull) return 1;
        if (rp.null_equi) return -1;
        return key_cmp(lekeys, lrep, rekeys, rp.rep, ekinds);
    };

    auto emit_left = [&](std::int64_t lr) {
        for (std::size_t i = 0; i < lvals.size(); ++i)
            append_value(b, i, lvals[i], lr);
    };
    auto emit_right = [&](std::int64_t rr) {
        for (std::size_t i = 0; i < rvals.size(); ++i)
            append_value(b, rval_base + i, rvals[i], rr);
    };
    auto emit_right_null = [&]() {
        for (std::size_t i = 0; i < rvals.size(); ++i)
            b.append_null(rval_base + i);
    };

    std::size_t rp = 0;
    std::int64_t li = 0;
    while (li < L) {
        std::int64_t lrep = lorder[static_cast<std::size_t>(li)];
        bool lnull = row_key_null(lekeys, lrep);
        std::int64_t lj = equi_partition_end(lorder, lekeys, ekinds, li, L);

        while (rp < rparts.size() && part_cmp(lnull, lrep, rparts[rp]) > 0)
            ++rp;
        bool matched =
            rp < rparts.size() && part_cmp(lnull, lrep, rparts[rp]) == 0;
        std::int64_t rstart = matched ? rparts[rp].start : 0;
        std::int64_t rtsend = matched ? rparts[rp].ts_end : 0;

        // Cursors advance monotonically because left rows are ts-ascending
        // within the partition. `cur` chases the backward/forward boundary;
        // `ge` and `gt` are the first right ts >= and > left ts for the
        // nearest split.
        std::int64_t cur = rstart;
        std::int64_t ge = rstart;
        std::int64_t gt = rstart;
        for (std::int64_t a = li; a < lj; ++a) {
            std::int64_t lr = lorder[static_cast<std::size_t>(a)];
            emit_left(lr);
            if (!matched || ArrowArrayViewIsNull(lts, lr)) {
                emit_right_null();
                b.end_row();
                continue;
            }
            std::int64_t cand = -1;
            if (dir == AsofDirection::BACKWARD) {
                // allow_exact keeps ts == left.ts (<= 0); otherwise strict (<
                // 0).
                const int thresh = allow_exact ? 1 : 0;
                while (cur < rtsend &&
                       ts_cmp(rts, rorder[static_cast<std::size_t>(cur)], lts,
                              lr, ts_kind) < thresh)
                    ++cur;
                if (cur - 1 >= rstart) cand = cur - 1;
            } else if (dir == AsofDirection::FORWARD) {
                // allow_exact keeps ts == left.ts (>= 0); otherwise strict (>
                // 0).
                const int thresh = allow_exact ? 0 : 1;
                while (cur < rtsend &&
                       ts_cmp(rts, rorder[static_cast<std::size_t>(cur)], lts,
                              lr, ts_kind) < thresh)
                    ++cur;
                if (cur < rtsend) cand = cur;
            } else {
                while (ge < rtsend &&
                       ts_cmp(rts, rorder[static_cast<std::size_t>(ge)], lts,
                              lr, ts_kind) < 0)
                    ++ge;
                while (gt < rtsend &&
                       ts_cmp(rts, rorder[static_cast<std::size_t>(gt)], lts,
                              lr, ts_kind) <= 0)
                    ++gt;
                const std::int64_t back = gt - 1 >= rstart ? gt - 1 : -1;
                const std::int64_t fwd = ge < rtsend ? ge : -1;
                if (back < 0)
                    cand = fwd;
                else if (fwd < 0)
                    cand = back;
                else
                    cand = ts_le_dist(lts, lr, rts,
                                      rorder[static_cast<std::size_t>(back)],
                                      rorder[static_cast<std::size_t>(fwd)],
                                      ts_kind)
                               ? back
                               : fwd;
            }
            std::int64_t rr =
                cand >= 0 ? rorder[static_cast<std::size_t>(cand)] : -1;
            if (rr >= 0 &&
                (!has_tol || ts_within_tol(lts, lr, rts, rr, ts_kind, tol)))
                emit_right(rr);
            else
                emit_right_null();
            b.end_row();
        }
        li = lj;
    }

    return b.finish();
}

ArrowExportResult interval_join(
    const ArrowSchema* left_s, const ArrowArray* left_a,
    std::uint32_t left_point_col, const std::uint32_t* left_equi_cols,
    const ArrowSchema* right_s, const ArrowArray* right_a,
    std::uint32_t right_lo_col, std::uint32_t right_hi_col,
    const std::uint32_t* right_equi_cols, std::uint32_t n_equi, bool left_outer,
    bool right_outer) {
    ArrowArrayView lav;
    ArrowArrayView rav;
    if (init_array_view(lav, const_cast<ArrowSchema*>(left_s),
                        const_cast<ArrowArray*>(left_a)) != NANOARROW_OK) {
        throw DFTUtilsException(ErrorCode::INTERNAL,
                                "interval_join: failed to view left batch");
    }
    struct LGuard {
        ArrowArrayView* v;
        ~LGuard() { ArrowArrayViewReset(v); }
    } lg{&lav};
    if (init_array_view(rav, const_cast<ArrowSchema*>(right_s),
                        const_cast<ArrowArray*>(right_a)) != NANOARROW_OK) {
        throw DFTUtilsException(ErrorCode::INTERNAL,
                                "interval_join: failed to view right batch");
    }
    struct RGuard {
        ArrowArrayView* v;
        ~RGuard() { ArrowArrayViewReset(v); }
    } rg{&rav};

    const std::int64_t lcols = lav.n_children;
    const std::int64_t rcols = rav.n_children;

    if (left_point_col >= static_cast<std::uint32_t>(lcols) ||
        right_lo_col >= static_cast<std::uint32_t>(rcols) ||
        right_hi_col >= static_cast<std::uint32_t>(rcols)) {
        throw DFTUtilsException(
            ErrorCode::INVALID_ARGUMENT,
            "interval_join: point/lo/hi column index out of range");
    }
    const ArrowArrayView* lpoint = lav.children[left_point_col];
    const ArrowArrayView* rlo = rav.children[right_lo_col];
    const ArrowArrayView* rhi = rav.children[right_hi_col];
    if (lpoint->storage_type != rlo->storage_type ||
        lpoint->storage_type != rhi->storage_type) {
        throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                "interval_join: point/lo/hi types differ");
    }
    auto pk = key_kind_from_storage(lpoint->storage_type);
    if (!pk || *pk == KeyKind::BYTES) {
        throw DFTUtilsException(
            ErrorCode::INVALID_ARGUMENT,
            "interval_join: point/lo/hi columns must be numeric");
    }
    const KeyKind pkind = *pk;

    KeyViews lekeys(n_equi);
    KeyViews rekeys(n_equi);
    std::vector<KeyKind> ekinds(n_equi);
    std::unordered_set<std::size_t> right_skip;
    right_skip.insert(right_lo_col);
    right_skip.insert(right_hi_col);
    for (std::uint32_t k = 0; k < n_equi; ++k) {
        if (left_equi_cols[k] >= static_cast<std::uint32_t>(lcols) ||
            right_equi_cols[k] >= static_cast<std::uint32_t>(rcols)) {
            throw DFTUtilsException(
                ErrorCode::INVALID_ARGUMENT,
                "interval_join: equi column index out of range");
        }
        const ArrowArrayView* lv = lav.children[left_equi_cols[k]];
        const ArrowArrayView* rv = rav.children[right_equi_cols[k]];
        if (lv->storage_type != rv->storage_type) {
            throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                    "interval_join: equi column types differ");
        }
        auto kk = key_kind_from_storage(lv->storage_type);
        if (!kk) {
            throw DFTUtilsException(
                ErrorCode::INVALID_ARGUMENT,
                "interval_join: unsupported equi column type");
        }
        lekeys[k] = lv;
        rekeys[k] = rv;
        ekinds[k] = *kk;
        right_skip.insert(right_equi_cols[k]);
    }

    std::vector<ColumnSpec> specs;
    std::vector<ValueCol> lvals;
    std::vector<ValueCol> rvals;
    std::unordered_set<std::string> names;
    for (std::int64_t c = 0; c < lcols; ++c) {
        ColumnSpec spec;
        lvals.push_back(plan_value_col(lav.children[c], left_s->children[c],
                                       left_s->children[c]->name, spec));
        names.insert(spec.name);
        specs.push_back(std::move(spec));
    }
    for (std::int64_t c = 0; c < rcols; ++c) {
        if (right_skip.count(static_cast<std::size_t>(c))) continue;
        ColumnSpec spec;
        rvals.push_back(plan_value_col(rav.children[c], right_s->children[c],
                                       right_s->children[c]->name, spec));
        if (names.count(spec.name)) {
            spec.name += "_right";
            if (names.count(spec.name)) {
                throw DFTUtilsException(
                    ErrorCode::INVALID_ARGUMENT,
                    std::string("interval_join: output column name collision: ")
                        .append(spec.name));
            }
        }
        names.insert(spec.name);
        specs.push_back(std::move(spec));
    }

    const std::size_t rval_base = lvals.size();

    RecordBatchBuilder b;
    b.declare_schema(specs);

    const std::int64_t L = lav.length;
    const std::int64_t R = rav.length;

    // Sort left by (equi, point) and right by (equi, lo); a null-equi row sorts
    // into its own trailing partition, and within a partition a null point/lo
    // row sorts last so it can be skipped without reading a null cell. Ties
    // break by original row index for determinism.
    std::vector<std::int64_t> lorder =
        order_rows(lekeys, ekinds, lpoint, pkind, L);
    std::vector<std::int64_t> rorder =
        order_rows(rekeys, ekinds, rlo, pkind, R);

    // Right partitions in sorted order. `lo_end` is the first null-lo row
    // (which sorts last), so [start, lo_end) are the real, lo-sorted candidate
    // rows.
    struct RPart {
        bool null_equi;
        std::int64_t rep;
        std::int64_t start;
        std::int64_t lo_end;
        std::int64_t end;
    };
    std::vector<RPart> rparts;
    for (std::int64_t i = 0; i < R;) {
        std::int64_t rep = rorder[static_cast<std::size_t>(i)];
        bool rn = row_key_null(rekeys, rep);
        std::int64_t j = equi_partition_end(rorder, rekeys, ekinds, i, R);
        std::int64_t le = i;
        while (le < j &&
               !ArrowArrayViewIsNull(rlo, rorder[static_cast<std::size_t>(le)]))
            ++le;
        rparts.push_back({rn, rep, i, le, j});
        i = j;
    }

    auto part_cmp = [&](bool lnull, std::int64_t lrep, const RPart& rp) {
        if (lnull && rp.null_equi) return 0;
        if (lnull) return 1;
        if (rp.null_equi) return -1;
        return key_cmp(lekeys, lrep, rekeys, rp.rep, ekinds);
    };

    auto emit_left = [&](std::int64_t lr) {
        for (std::size_t i = 0; i < lvals.size(); ++i)
            append_value(b, i, lvals[i], lr);
    };
    auto emit_right = [&](std::int64_t rr) {
        for (std::size_t i = 0; i < rvals.size(); ++i)
            append_value(b, rval_base + i, rvals[i], rr);
    };
    auto emit_right_null = [&]() {
        for (std::size_t i = 0; i < rvals.size(); ++i)
            b.append_null(rval_base + i);
    };
    auto emit_left_null = [&]() {
        for (std::size_t i = 0; i < lvals.size(); ++i) b.append_null(i);
    };

    // right_outer: right rows that never cover a point, emitted null-left after
    // the matched rows.
    std::vector<char> right_matched;
    if (right_outer) right_matched.assign(static_cast<std::size_t>(R), 0);

    // Min-heap on hi over the active intervals (front = smallest hi).
    auto hi_greater = [&](std::int64_t x, std::int64_t y) {
        return ts_cmp(rhi, x, rhi, y, pkind) > 0;
    };

    std::vector<std::int64_t> active;
    std::vector<std::int64_t> ordered;
    std::size_t rp = 0;
    std::int64_t li = 0;
    while (li < L) {
        std::int64_t lrep = lorder[static_cast<std::size_t>(li)];
        bool lnull = row_key_null(lekeys, lrep);
        std::int64_t lj = equi_partition_end(lorder, lekeys, ekinds, li, L);

        while (rp < rparts.size() && part_cmp(lnull, lrep, rparts[rp]) > 0)
            ++rp;
        bool matched =
            rp < rparts.size() && part_cmp(lnull, lrep, rparts[rp]) == 0;
        std::int64_t rstart = matched ? rparts[rp].start : 0;
        std::int64_t rloend = matched ? rparts[rp].lo_end : 0;

        active.clear();
        bool active_dirty = true;
        std::int64_t add = rstart;
        for (std::int64_t a = li; a < lj; ++a) {
            std::int64_t lr = lorder[static_cast<std::size_t>(a)];
            bool has_point = matched && !ArrowArrayViewIsNull(lpoint, lr);
            if (has_point) {
                while (add < rloend &&
                       ts_cmp(rlo, rorder[static_cast<std::size_t>(add)],
                              lpoint, lr, pkind) <= 0) {
                    std::int64_t rr = rorder[static_cast<std::size_t>(add)];
                    if (!ArrowArrayViewIsNull(rhi, rr)) {
                        active.push_back(rr);
                        std::push_heap(active.begin(), active.end(),
                                       hi_greater);
                        active_dirty = true;
                    }
                    ++add;
                }
                // Sweep-line invariant: intervals enter on lo <= point and are
                // expired here on hi < point, so every remaining active row
                // satisfies lo <= point <= hi and covers this closed interval.
                while (!active.empty() &&
                       ts_cmp(rhi, active.front(), lpoint, lr, pkind) < 0) {
                    std::pop_heap(active.begin(), active.end(), hi_greater);
                    active.pop_back();
                    active_dirty = true;
                }
            }
            if (!has_point || active.empty()) {
                if (left_outer) {
                    emit_left(lr);
                    emit_right_null();
                    b.end_row();
                }
                continue;
            }
            // Reuse the last sorted order when the active set is unchanged.
            if (active_dirty) {
                ordered.assign(active.begin(), active.end());
                std::sort(ordered.begin(), ordered.end(),
                          [&](std::int64_t x, std::int64_t y) {
                              int c = ts_cmp(rlo, x, rlo, y, pkind);
                              if (c != 0) return c < 0;
                              c = ts_cmp(rhi, x, rhi, y, pkind);
                              if (c != 0) return c < 0;
                              return x < y;
                          });
                active_dirty = false;
            }
            for (std::int64_t rr : ordered) {
                emit_left(lr);
                emit_right(rr);
                if (right_outer)
                    right_matched[static_cast<std::size_t>(rr)] = 1;
                b.end_row();
            }
        }
        li = lj;
    }

    if (right_outer) {
        for (std::int64_t i = 0; i < R; ++i) {
            std::int64_t rr = rorder[static_cast<std::size_t>(i)];
            if (right_matched[static_cast<std::size_t>(rr)]) continue;
            emit_left_null();
            emit_right(rr);
            b.end_row();
        }
    }

    return b.finish();
}

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
