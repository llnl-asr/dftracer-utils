// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_SOURCE_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_SOURCE_H

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin/frame.h>
#include <dftracer/utils/plugins/plugin/map.h>
#include <dftracer/utils/plugins/plugin/types.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <utility>

/* A LazyFrame source written as a C++ class. source_vtable<S>() adapts S to
 * dftu_source_vt; register it with PluginBuilder::source<S>(). Only C handles
 * and C calls cross the boundary: a callback reports failure as a dstd::Result,
 * and every object the host receives is freed through its own vtable.
 *
 * S provides:
 *   dstd::Vector<dstd::String> names() const;
 *   dstd::Result<ScanResult<C>> scan(const ScanView&, const Host&) const;
 * where C is a cursor type with
 *   dstd::Result<dstd::Optional<OwnedDataFrame>> next(std::int64_t max_rows);
 * and optionally, each wired only when present,
 *   bool narrow(ExprView predicate);          // advisory, see dftu_cursor_vt
 * and optionally
 *   void schema(SchemaBuilder&) const;               // column types; or
 *                                                    // dstd::Result<void>
 *   dstd::Result<dstd::Optional<Applied<S>>> apply_filter(ExprView) const;
 *   apply_projection(const dstd::Vector<NamedExpr>&) const;
 *   apply_aggregation(const AggregationView&) const;
 *   apply_sort(const SortView&) const;
 *   apply_topn(const SortView&, std::int64_t k) const;
 *   apply_limit(std::int64_t offset, std::int64_t n) const;
 *   apply_tail(std::int64_t n) const;
 *   apply_join(const JoinView<S>&) const;
 * each of the apply_* returning the same dstd::Result of an Optional of
 * Applied<S>. The apply_* contract is dftu_source_vt::apply's: dstd::nullopt
 * for unsupported or no change, a new S otherwise, projection / aggregation /
 * join only as Apply::Exact. A cursor error is the dstd::Error its next()
 * returns, which fails the call with its status and message; so does an error
 * from apply_*, scan() or schema().
 *
 * The Host given to scan is that of the cursor, valid for that call: reserve
 * memory through Host::memory() there and keep the reservations in C. A
 * cursor that must grow a reservation while it runs takes
 *   next(std::int64_t max_rows, const Host& host)
 * in place of next(max_rows), and gets a Host that is good for that call. */

namespace dftracer::utils::plugins {

namespace detail {
struct ExprAccess;
struct SchemaAccess;
}  // namespace detail

/// The query-language enums of the dataframe library, spelled as themselves: a
/// plugin writes @ref CmpOp "CmpOp::Gt" and @ref Pushed "Pushed::No", never a
/// status code.
using dataframe::CmpOp;
using dataframe::JoinHow;
using dataframe::LogicalOp;
using dataframe::Pushed;
using dataframe::ScalarKind;
using dataframe::StrPredOp;
using dataframe::TimeUnit;

class OwnedExpr;

/// A borrowed expression, positional against the source's schema().
class ExprView {
   public:
    ExprView() = default;
    explicit ExprView(const dftu_expr* e) noexcept : e_(e) {}
    explicit operator bool() const noexcept { return e_ != nullptr; }

    /// The column when this is a bare column reference.
    dstd::Optional<std::int32_t> column() const {
        const std::int32_t c = e_ ? dftu_expr_col_index(e_) : -1;
        if (c < 0) return dstd::nullopt;
        return c;
    }

    /// `column <op> literal`.
    struct Compare {
        std::int32_t column = -1;
        CmpOp op = CmpOp::Eq;

        /// The literal, read in its own domain. It borrows this Compare, so it
        /// is valid only while this object lives.
        ScalarView rhs() const noexcept { return ScalarView(&literal_); }

       private:
        friend class ExprView;
        dftu_scalar literal_{};  ///< a string borrows from the expression
    };
    dstd::Optional<Compare> as_compare() const {
        std::int32_t col = -1, op = 0;
        dftu_scalar rhs{};
        if (!e_ || !dftu_expr_as_col_cmp(e_, &col, &op, &rhs))
            return dstd::nullopt;
        Compare cmp;
        cmp.column = col;
        cmp.op = static_cast<CmpOp>(op);
        cmp.literal_ = rhs;
        return cmp;
    }

    struct Logical;
    dstd::Optional<Logical> as_logical() const;
    dstd::Optional<OwnedExpr> as_not() const;

    /// `column <op> pattern`.
    struct StrPred {
        std::int32_t column = -1;
        StrPredOp op = StrPredOp::Contains;
        dstd::StringView pattern;  ///< borrows from this expression
    };
    dstd::Optional<StrPred> as_str_pred() const {
        std::int32_t col = -1, op = 0, len = 0;
        const char* pat = nullptr;
        if (!e_ || !dftu_expr_as_col_str_pred(e_, &col, &op, &pat, &len))
            return dstd::nullopt;
        return StrPred{col, static_cast<StrPredOp>(op),
                       dstd::StringView(pat, static_cast<std::size_t>(len))};
    }

    /// `column is_in values`.
    struct IsIn {
        std::int32_t column = -1;

        /// The candidate values. They borrow this IsIn, so the view is valid
        /// only while this object lives.
        ColumnView values() const noexcept { return ColumnView(values_.get()); }

       private:
        friend class ExprView;
        Column values_;
    };
    dstd::Optional<IsIn> as_is_in() const {
        std::int32_t col = -1;
        dftu_series* values = nullptr;
        if (!e_ || !dftu_expr_as_col_is_in(e_, &col, &values))
            return dstd::nullopt;
        IsIn in;
        in.column = col;
        in.values_ = Column(values);
        return in;
    }

    /// Evaluate over `inputs`, one column per positional index the expression
    /// reads. An empty column on a malformed expression and on an expression
    /// that reads more columns than it was given.
    Column eval(dstd::Span<const ColumnView> inputs) const {
        if (!e_) return Column();
        const std::size_t n = inputs.size();
        // The C call takes one pointer per column, so the common small frame
        // evaluates without allocating.
        std::array<const dftu_series*, 8> stack{};
        dstd::Vector<const dftu_series*> heap;
        const dftu_series* const* columns = nullptr;
        if (n <= stack.size()) {
            for (std::size_t i = 0; i < n; ++i)
                stack[i] = dataframe::detail::column_series(inputs[i]);
            columns = stack.data();
        } else {
            heap.reserve(n);
            for (const ColumnView& input : inputs)
                heap.push_back(dataframe::detail::column_series(input));
            columns = heap.data();
        }
        return Column(
            dftu_expr_eval(e_, columns, static_cast<std::int32_t>(n)));
    }

   private:
    friend struct detail::ExprAccess;
    const dftu_expr* e_ = nullptr;
};

class OwnedExpr {
   public:
    OwnedExpr() = default;
    explicit OwnedExpr(dftu_expr* e) noexcept : e_(e) {}
    OwnedExpr(OwnedExpr&& o) noexcept : e_(std::exchange(o.e_, nullptr)) {}
    OwnedExpr& operator=(OwnedExpr&& o) noexcept {
        if (this != &o) {
            reset();
            e_ = std::exchange(o.e_, nullptr);
        }
        return *this;
    }
    OwnedExpr(const OwnedExpr&) = delete;
    OwnedExpr& operator=(const OwnedExpr&) = delete;
    ~OwnedExpr() { reset(); }

    ExprView view() const noexcept { return ExprView(e_); }

   private:
    void reset() noexcept {
        if (e_) dftu_expr_free(e_);
        e_ = nullptr;
    }
    dftu_expr* e_ = nullptr;
};

struct ExprView::Logical {
    LogicalOp op = LogicalOp::And;
    OwnedExpr lhs;
    OwnedExpr rhs;
};

inline dstd::Optional<ExprView::Logical> ExprView::as_logical() const {
    std::int32_t op = 0;
    dftu_expr* a = nullptr;
    dftu_expr* b = nullptr;
    if (!e_ || !dftu_expr_as_logical(e_, &op, &a, &b)) return dstd::nullopt;
    return Logical{static_cast<LogicalOp>(op), OwnedExpr(a), OwnedExpr(b)};
}

inline dstd::Optional<OwnedExpr> ExprView::as_not() const {
    dftu_expr* a = nullptr;
    if (!e_ || !dftu_expr_as_not(e_, &a)) return dstd::nullopt;
    return OwnedExpr(a);
}

namespace detail {

/// The C expression a @ref ExprView borrows. SDK plumbing, not plugin surface.
struct ExprAccess {
    static const dftu_expr* raw(const ExprView& e) noexcept { return e.e_; }
};

/// @see ExprAccess
inline const dftu_expr* expr_raw(const ExprView& e) noexcept {
    return ExprAccess::raw(e);
}

}  // namespace detail

struct NamedExpr {
    dstd::StringView name;
    ExprView expr;
};

struct AggView {
    dstd::StringView op;  ///< canonical aggregate name, e.g. "sum"
    ExprView input;       ///< empty for count
    ExprView by;          ///< set only for ops that read a second column
    double param = 0.0;
    dstd::StringView out;
};

struct AggregationView {
    dstd::Vector<NamedExpr> keys;
    dstd::Vector<AggView> aggs;
};

struct SortView {
    dstd::Vector<dstd::StringView> by;
    dstd::Vector<bool> descending;
};

template <class S>
struct JoinView {
    const S& other;  ///< the right side, fully absorbed into its own source
    dstd::Vector<dstd::StringView> left_on;
    dstd::Vector<dstd::StringView> right_on;
    JoinHow how = JoinHow::Inner;
    dstd::StringView suffix;
};

struct ScanView {
    dstd::Vector<dstd::StringView> projection;  ///< empty: every column
    dstd::Vector<ExprView> filters;  ///< positional against the projection
    std::int64_t limit = -1;         ///< -1: none
    std::uint64_t memory_budget = 0;
};

template <class Cursor>
struct ScanResult {
    dstd::UniquePtr<Cursor> cursor;
    /// One entry per ScanView::filters; missing entries read as Pushed::No.
    dstd::Vector<Pushed> filters;
};

enum class Apply { Exact, Inexact };

template <class S>
struct Applied {
    dstd::UniquePtr<S> source;
    Apply status = Apply::Exact;
};

class SchemaBuilder {
   public:
    explicit SchemaBuilder(dftu_schema* s) noexcept : s_(s) {}

    /// Append a column whose time values are microseconds, the unit an ABI
    /// column that declares none reads in. `name` must not be null (@ref
    /// dstd::CStringView); an empty `name` gives an unnamed column.
    std::int32_t add(dstd::CStringView name, TypeId type,
                     bool nullable = true) {
        return add(name, type, TimeUnit::Micro, nullable);
    }
    /// Append a column read in `unit`, which a Timestamp, Time or Duration
    /// column uses and every other type ignores. Returns the field's index, or
    /// -1 for a type cast from outside @ref TypeId; a call that names a
    /// @ref TypeId value cannot fail, so it may ignore the result.
    std::int32_t add(dstd::CStringView name, TypeId type, TimeUnit unit,
                     bool nullable = true) {
        return dftu_schema_add_field(
            s_, name.c_str(), static_cast<dftu_dtype>(type), nullable ? 1 : 0,
            static_cast<dftu_time_unit>(unit), nullptr, 0, 0, 0);
    }

   private:
    friend struct detail::SchemaAccess;
    dftu_schema* s_ = nullptr;
};

namespace detail {

/// The C schema a @ref SchemaBuilder appends to. SDK plumbing, not plugin
/// surface.
struct SchemaAccess {
    static dftu_schema* raw(const SchemaBuilder& b) noexcept { return b.s_; }
};

/// @see SchemaAccess
inline dftu_schema* schema_raw(const SchemaBuilder& b) noexcept {
    return SchemaAccess::raw(b);
}

}  // namespace detail

namespace detail {

template <class S>
struct SourceBox {
    dstd::UniquePtr<S> source;
    dstd::Vector<dstd::String> names;
    dstd::Vector<const char*> name_ptrs;
    bool named = false;
};

template <class C>
struct CursorBox {
    dstd::UniquePtr<C> cursor;
    const dftu_plugin_host* host = nullptr;
};

template <class C>
struct CursorVt {
    static dftu_status next(dftu_ctx* ctx, void* self, std::int64_t max_rows,
                            dftu_dataframe** out, dftu_task**) noexcept {
        auto* box = static_cast<CursorBox<C>*>(self);
        return to_status(ctx, [&]() -> dftu_status {
            dstd::Result<dstd::Optional<OwnedDataFrame>> next = [&] {
                if constexpr (requires(C& c, const Host& h) {
                                  c.next(std::int64_t{}, h);
                              })
                    return box->cursor->next(
                        max_rows, detail::host_from_raw(box->host, ctx));
                else
                    return box->cursor->next(max_rows);
            }();
            if (!next.has_value()) return report(ctx, next.error());
            dstd::Optional<OwnedDataFrame>& f = *next;
            if (!f) {
                *out = nullptr;
                return DFTU_END;
            }
            *out = f->release();
            return DFTU_OK;
        });
    }
    static void destroy(void* self) { delete static_cast<CursorBox<C>*>(self); }
    static dftu_status narrow(dftu_ctx* ctx, void* self,
                              const dftu_expr* predicate,
                              std::int32_t* out_applied, dftu_task**) noexcept {
        auto* box = static_cast<CursorBox<C>*>(self);
        return to_status(ctx, [&]() -> dftu_status {
            *out_applied = box->cursor->narrow(ExprView(predicate)) ? 1 : 0;
            return DFTU_OK;
        });
    }
    static const dftu_cursor_vt* vt() {
        static const dftu_cursor_vt VT = [] {
            dftu_cursor_vt t{};
            t.struct_size = sizeof(dftu_cursor_vt);
            t.next = next;
            t.destroy = destroy;
            if constexpr (requires(C& c) {
                              { c.narrow(ExprView{}) } -> std::same_as<bool>;
                          })
                t.narrow = narrow;
            return t;
        }();
        return &VT;
    }
};

template <class S>
concept HasSchema = requires(const S& s, SchemaBuilder& b) { s.schema(b); };

template <class S>
concept HasApply =
    requires(const S& s) { s.apply_filter(ExprView{}); } ||
    requires(const S& s, const dstd::Vector<NamedExpr>& v) {
        s.apply_projection(v);
    } ||
    requires(const S& s, const AggregationView& v) {
        s.apply_aggregation(v);
    } || requires(const S& s, const SortView& v) { s.apply_sort(v); } ||
    requires(const S& s, const SortView& v) {
        s.apply_topn(v, std::int64_t{});
    } ||
    requires(const S& s) { s.apply_limit(std::int64_t{}, std::int64_t{}); } ||
    requires(const S& s) { s.apply_tail(std::int64_t{}); } ||
    requires(const S& s, const JoinView<S>& v) { s.apply_join(v); };

inline std::size_t abi_count(std::int32_t n) noexcept {
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

inline dstd::Vector<dstd::StringView> string_views(
    dstd::Span<const char* const> p) {
    dstd::Vector<dstd::StringView> out;
    out.reserve(p.size());
    for (const char* s : p) out.emplace_back(s ? s : "");
    return out;
}

inline ScanView scan_view_of(const dftu_scan_request& req) {
    ScanView view;
    view.projection =
        string_views({req.projection, abi_count(req.n_projection)});
    for (std::int32_t i = 0; i < req.n_filters; ++i)
        view.filters.emplace_back(req.filters[i]);
    view.limit = req.limit;
    view.memory_budget = req.memory_budget;
    return view;
}

/// A source that says what scan() would do for a request:
///   dstd::Result<void> explain(const ScanView&, const ExplainOut&) const;
/// The host then sets dftu_source_vt::explain and prints its lines.
template <class S>
concept HasExplain =
    requires(const S& s, const ScanView& v, const ExplainOut& out) {
        { s.explain(v, out) } -> std::same_as<dstd::Result<void>>;
    };

template <class S>
struct SourceVt {
    using Box = SourceBox<S>;

    static dftu_status explain(dftu_ctx* ctx, void* self,
                               const dftu_scan_request* req,
                               const dftu_explain_out* out) noexcept
        requires HasExplain<S>
    {
        auto* box = static_cast<Box*>(self);
        return to_status(ctx, [&]() {
            return box->source->explain(scan_view_of(*req), ExplainOut(out));
        });
    }

    static dftu_status schema(dftu_ctx* ctx, void* self,
                              const char* const** out_names,
                              std::int32_t* out_count) noexcept {
        auto* box = static_cast<Box*>(self);
        return to_status(ctx, [&]() -> dftu_status {
            if (!box->named) {
                box->names = box->source->names();
                box->name_ptrs.clear();
                for (const dstd::String& n : box->names)
                    box->name_ptrs.push_back(n.c_str());
                box->named = true;
            }
            *out_names = box->name_ptrs.data();
            *out_count = static_cast<std::int32_t>(box->name_ptrs.size());
            return DFTU_OK;
        });
    }

    static dftu_status scan(dftu_ctx* ctx, void* self,
                            const dftu_plugin_host* host,
                            const dftu_scan_request* req,
                            std::int32_t* out_pushed, void** out_cursor_self,
                            const dftu_cursor_vt** out_vt) noexcept {
        auto* box = static_cast<Box*>(self);
        return to_status(ctx, [&]() -> dftu_status {
            ScanView view;
            view.projection =
                string_views({req->projection, abi_count(req->n_projection)});
            for (std::int32_t i = 0; i < req->n_filters; ++i)
                view.filters.emplace_back(req->filters[i]);
            view.limit = req->limit;
            view.memory_budget = req->memory_budget;
            auto scanned =
                box->source->scan(view, detail::host_from_raw(host, ctx));
            if (!scanned.has_value()) return report(ctx, scanned.error());
            auto& result = *scanned;
            using C = typename decltype(result.cursor)::element_type;
            if (!result.cursor)
                return dftu_fail(ctx, DFTU_E_INTERNAL,
                                 "scan returned no cursor");
            const std::size_t n =
                std::min<std::size_t>(result.filters.size(),
                                      static_cast<std::size_t>(req->n_filters));
            for (std::size_t i = 0; i < n; ++i)
                out_pushed[i] = static_cast<std::int32_t>(result.filters[i]);
            auto* cbox = new CursorBox<C>{std::move(result.cursor), host};
            *out_cursor_self = cbox;
            *out_vt = CursorVt<C>::vt();
            return DFTU_OK;
        });
    }

    static void destroy(void* self) { delete static_cast<Box*>(self); }

    static dftu_status schema_types(dftu_ctx* ctx, void* self,
                                    dftu_schema* out) noexcept
        requires HasSchema<S>
    {
        auto* box = static_cast<Box*>(self);
        return to_status(ctx, [&]() {
            SchemaBuilder b(out);
            return box->source->schema(b);
        });
    }

    static dstd::Result<dstd::Optional<Applied<S>>> dispatch(
        const S& s, const dftu_apply_request& req) {
        switch (req.kind) {
            case DFTU_APPLY_FILTER:
                if constexpr (requires { s.apply_filter(ExprView{}); })
                    return s.apply_filter(ExprView(req.u.filter.predicate));
                break;
            case DFTU_APPLY_PROJECTION:
                if constexpr (requires(const dstd::Vector<NamedExpr>& v) {
                                  s.apply_projection(v);
                              }) {
                    dstd::Vector<NamedExpr> exprs;
                    for (std::int32_t i = 0; i < req.u.projection.n_exprs; ++i)
                        exprs.push_back(
                            {req.u.projection.exprs[i].name,
                             ExprView(req.u.projection.exprs[i].expr)});
                    return s.apply_projection(exprs);
                }
                break;
            case DFTU_APPLY_AGGREGATION:
                if constexpr (requires(const AggregationView& v) {
                                  s.apply_aggregation(v);
                              }) {
                    const dftu_apply_aggregation_args& a = req.u.aggregation;
                    AggregationView v;
                    for (std::int32_t i = 0; i < a.n_keys; ++i)
                        v.keys.push_back(
                            {a.keys[i].name, ExprView(a.keys[i].expr)});
                    for (std::int32_t i = 0; i < a.n_aggs; ++i)
                        v.aggs.push_back({a.aggs[i].op,
                                          ExprView(a.aggs[i].input),
                                          ExprView(a.aggs[i].by),
                                          a.aggs[i].param, a.aggs[i].out});
                    return s.apply_aggregation(v);
                }
                break;
            case DFTU_APPLY_SORT:
                if constexpr (requires(const SortView& v) { s.apply_sort(v); })
                    return s.apply_sort(sort_view(req.u.sort));
                break;
            case DFTU_APPLY_TOPN:
                if constexpr (requires(const SortView& v) {
                                  s.apply_topn(v, std::int64_t{});
                              })
                    return s.apply_topn(sort_view(req.u.topn.sort),
                                        req.u.topn.k);
                break;
            case DFTU_APPLY_LIMIT:
                if constexpr (requires {
                                  s.apply_limit(std::int64_t{}, std::int64_t{});
                              })
                    return s.apply_limit(req.u.limit.offset, req.u.limit.n);
                break;
            case DFTU_APPLY_TAIL:
                if constexpr (requires { s.apply_tail(std::int64_t{}); })
                    return s.apply_tail(req.u.tail.n);
                break;
            case DFTU_APPLY_JOIN:
                if constexpr (requires(const JoinView<S>& v) {
                                  s.apply_join(v);
                              }) {
                    const dftu_apply_join_args& j = req.u.join;
                    // Only a source this adapter made can be joined natively.
                    if (!j.other_vt || j.other_vt->scan != &SourceVt<S>::scan)
                        break;
                    const S& other = *static_cast<Box*>(j.other_self)->source;
                    JoinView<S> v{
                        other, string_views({j.left_on, abi_count(j.n_on)}),
                        string_views({j.right_on, abi_count(j.n_on)}),
                        static_cast<JoinHow>(j.how), j.suffix ? j.suffix : ""};
                    return s.apply_join(v);
                }
                break;
            default:
                break;
        }
        return dstd::nullopt;
    }

    static SortView sort_view(const dftu_apply_sort_args& a) {
        SortView v;
        v.by = string_views({a.by, abi_count(a.n_by)});
        for (std::int32_t i = 0; i < a.n_by; ++i)
            v.descending.push_back(a.descending[i] != 0);
        return v;
    }

    static dftu_status apply(dftu_ctx* ctx, void* self,
                             const dftu_apply_request* req,
                             dftu_apply_result* out) noexcept {
        auto* box = static_cast<Box*>(self);
        return to_status(ctx, [&]() -> dftu_status {
            dstd::Result<dstd::Optional<Applied<S>>> applied =
                dispatch(*box->source, *req);
            if (!applied.has_value()) return report(ctx, applied.error());
            dstd::Optional<Applied<S>>& r = *applied;
            if (!r || !r->source) return DFTU_OK;
            auto* next = new Box{std::move(r->source), {}, {}, false};
            out->status = r->status == Apply::Exact ? DFTU_APPLY_EXACT
                                                    : DFTU_APPLY_INEXACT;
            out->self = next;
            out->vt = vt();
            return DFTU_OK;
        });
    }

    static const dftu_source_vt* vt() {
        static const dftu_source_vt VT = [] {
            dftu_source_vt t{};
            t.struct_size = sizeof(dftu_source_vt);
            t.schema = schema;
            t.scan = scan;
            t.destroy = destroy;
            if constexpr (HasSchema<S>) t.schema_types = schema_types;
            if constexpr (HasApply<S>) t.apply = apply;
            if constexpr (HasExplain<S>) t.explain = explain;
            return t;
        }();
        return &VT;
    }
};

}  // namespace detail

/// The C vtable for S. A `self` for it is made by make_source_self().
template <class S>
const dftu_source_vt* source_vtable() {
    return detail::SourceVt<S>::vt();
}

/// Wrap `source` as the `self` its vtable expects. Ownership passes to whoever
/// calls the vtable's destroy() on it.
template <class S>
void* make_source_self(dstd::UniquePtr<S> source) {
    return new detail::SourceBox<S>{std::move(source), {}, {}, false};
}

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_PLUGIN_SOURCE_H
