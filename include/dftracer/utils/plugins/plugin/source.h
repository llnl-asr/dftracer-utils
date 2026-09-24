#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_SOURCE_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_SOURCE_H

#include <dftracer/utils/dataframe/abi.h>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/* A LazyFrame source written as a C++ class. source_vtable<S>() adapts S to
 * dftu_source_vt; register it with PluginBuilder::source<S>() (or
 * dftu_provider_register). Only C handles and C calls cross the boundary:
 * exceptions are caught at every callback, and every object the host receives
 * is freed through its own vtable.
 *
 * S provides:
 *   std::vector<std::string> names() const;
 *   ScanResult<C> scan(const ScanView&) const;
 * where C is a cursor type with
 *   std::optional<OwnedFrame> next(std::int64_t max_rows);
 * and optionally, each wired only when present,
 *   bool narrow(ExprView predicate);          // advisory, see dftu_cursor_vt
 *   std::uint64_t resident_bytes();
 *   std::uint64_t reclaim(std::uint64_t want);
 * and optionally
 *   void schema(SchemaBuilder&) const;               // column types
 *   std::optional<Applied<S>> apply_filter(ExprView) const;
 *   std::optional<Applied<S>> apply_projection(const std::vector<NamedExpr>&)
 * const; std::optional<Applied<S>> apply_aggregation(const AggregationView&)
 * const; std::optional<Applied<S>> apply_sort(const SortView&) const;
 *   std::optional<Applied<S>> apply_topn(const SortView&, std::int64_t k)
 * const; std::optional<Applied<S>> apply_limit(std::int64_t offset,
 * std::int64_t n) const; std::optional<Applied<S>> apply_tail(std::int64_t n)
 * const; std::optional<Applied<S>> apply_join(const JoinView<S>&) const; The
 * apply_* contract is dftu_source_vt::apply's: std::nullopt for unsupported or
 * no change, a new S otherwise, projection / aggregation / join only as
 * Apply::Exact. A cursor error is a thrown std::exception; a throwing apply_*
 * reads as no change and a throwing scan() as a failed scan. */

namespace dftracer::utils::plugins {

class OwnedFrame {
   public:
    OwnedFrame() = default;
    explicit OwnedFrame(dftu_dataframe* df) noexcept : df_(df) {}
    OwnedFrame(OwnedFrame&& o) noexcept : df_(std::exchange(o.df_, nullptr)) {}
    OwnedFrame& operator=(OwnedFrame&& o) noexcept {
        if (this != &o) {
            reset();
            df_ = std::exchange(o.df_, nullptr);
        }
        return *this;
    }
    OwnedFrame(const OwnedFrame&) = delete;
    OwnedFrame& operator=(const OwnedFrame&) = delete;
    ~OwnedFrame() { reset(); }

    dftu_dataframe* get() const noexcept { return df_; }
    dftu_dataframe* release() noexcept { return std::exchange(df_, nullptr); }
    explicit operator bool() const noexcept { return df_ != nullptr; }

   private:
    void reset() noexcept {
        if (df_) dftu_dataframe_free(df_);
        df_ = nullptr;
    }
    dftu_dataframe* df_ = nullptr;
};

class OwnedSeries {
   public:
    OwnedSeries() = default;
    explicit OwnedSeries(dftu_series* s) noexcept : s_(s) {}
    OwnedSeries(OwnedSeries&& o) noexcept : s_(std::exchange(o.s_, nullptr)) {}
    OwnedSeries& operator=(OwnedSeries&& o) noexcept {
        if (this != &o) {
            reset();
            s_ = std::exchange(o.s_, nullptr);
        }
        return *this;
    }
    OwnedSeries(const OwnedSeries&) = delete;
    OwnedSeries& operator=(const OwnedSeries&) = delete;
    ~OwnedSeries() { reset(); }

    dftu_series* get() const noexcept { return s_; }
    dftu_series* release() noexcept { return std::exchange(s_, nullptr); }
    explicit operator bool() const noexcept { return s_ != nullptr; }

   private:
    void reset() noexcept {
        if (s_) dftu_series_free(s_);
        s_ = nullptr;
    }
    dftu_series* s_ = nullptr;
};

class OwnedExpr;

/// A borrowed expression, positional against the source's schema().
class ExprView {
   public:
    ExprView() = default;
    explicit ExprView(const dftu_expr* e) noexcept : e_(e) {}

    const dftu_expr* raw() const noexcept { return e_; }
    explicit operator bool() const noexcept { return e_ != nullptr; }

    /// The column when this is a bare column reference.
    std::optional<std::int32_t> column() const {
        const std::int32_t c = e_ ? dftu_expr_col_index(e_) : -1;
        if (c < 0) return std::nullopt;
        return c;
    }

    struct Compare {
        std::int32_t column;
        dftu_cmp_op op;
        dftu_scalar rhs;  ///< a string borrows from this expression
    };
    std::optional<Compare> as_compare() const {
        std::int32_t col = -1, op = 0;
        dftu_scalar rhs{};
        if (!e_ || !dftu_expr_as_col_cmp(e_, &col, &op, &rhs))
            return std::nullopt;
        return Compare{col, static_cast<dftu_cmp_op>(op), rhs};
    }

    struct Logical;
    std::optional<Logical> as_logical() const;
    std::optional<OwnedExpr> as_not() const;

    struct StrPred {
        std::int32_t column;
        dftu_str_pred_op op;
        std::string_view pattern;  ///< borrows from this expression
    };
    std::optional<StrPred> as_str_pred() const {
        std::int32_t col = -1, op = 0, len = 0;
        const char* pat = nullptr;
        if (!e_ || !dftu_expr_as_col_str_pred(e_, &col, &op, &pat, &len))
            return std::nullopt;
        return StrPred{col, static_cast<dftu_str_pred_op>(op),
                       std::string_view(pat, static_cast<std::size_t>(len))};
    }

    struct IsIn {
        std::int32_t column;
        OwnedSeries values;
    };
    std::optional<IsIn> as_is_in() const {
        std::int32_t col = -1;
        dftu_series* values = nullptr;
        if (!e_ || !dftu_expr_as_col_is_in(e_, &col, &values))
            return std::nullopt;
        return IsIn{col, OwnedSeries(values)};
    }

    /// Evaluate over `inputs`; an empty result on a malformed expression.
    OwnedSeries eval(const std::vector<const dftu_series*>& inputs) const {
        if (!e_) return {};
        return OwnedSeries(dftu_expr_eval(
            e_, inputs.data(), static_cast<std::int32_t>(inputs.size())));
    }

   private:
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
    dftu_logical_op op;
    OwnedExpr lhs;
    OwnedExpr rhs;
};

inline std::optional<ExprView::Logical> ExprView::as_logical() const {
    std::int32_t op = 0;
    dftu_expr* a = nullptr;
    dftu_expr* b = nullptr;
    if (!e_ || !dftu_expr_as_logical(e_, &op, &a, &b)) return std::nullopt;
    return Logical{static_cast<dftu_logical_op>(op), OwnedExpr(a),
                   OwnedExpr(b)};
}

inline std::optional<OwnedExpr> ExprView::as_not() const {
    dftu_expr* a = nullptr;
    if (!e_ || !dftu_expr_as_not(e_, &a)) return std::nullopt;
    return OwnedExpr(a);
}

struct NamedExpr {
    std::string_view name;
    ExprView expr;
};

struct AggView {
    std::string_view op;  ///< canonical aggregate name, e.g. "sum"
    ExprView input;       ///< empty for count
    ExprView by;          ///< set only for ops that read a second column
    double param = 0.0;
    std::string_view out;
};

struct AggregationView {
    std::vector<NamedExpr> keys;
    std::vector<AggView> aggs;
};

struct SortView {
    std::vector<std::string_view> by;
    std::vector<bool> descending;
};

template <class S>
struct JoinView {
    const S& other;  ///< the right side, fully absorbed into its own source
    std::vector<std::string_view> left_on;
    std::vector<std::string_view> right_on;
    dftu_join_how how;
    std::string_view suffix;
};

struct ScanView {
    std::vector<std::string_view> projection;  ///< empty: every column
    std::vector<ExprView> filters;  ///< positional against the projection
    std::int64_t limit = -1;        ///< -1: none
    std::uint64_t memory_budget = 0;
};

template <class Cursor>
struct ScanResult {
    std::unique_ptr<Cursor> cursor;
    /// One entry per ScanView::filters; missing entries read as NO.
    std::vector<dftu_pushed> filters;
};

enum class Apply { Exact, Inexact };

template <class S>
struct Applied {
    std::unique_ptr<S> source;
    Apply status = Apply::Exact;
};

class SchemaBuilder {
   public:
    explicit SchemaBuilder(dftu_schema* s) noexcept : s_(s) {}
    /// Appends a column; returns its index, or -1 on a bad argument.
    std::int32_t add(const char* name, dftu_dtype type, bool nullable = true) {
        return dftu_schema_add_field(s_, name, type, nullable ? 1 : 0,
                                     DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
    }
    dftu_schema* raw() const noexcept { return s_; }

   private:
    dftu_schema* s_;
};

namespace detail {

template <class S>
struct SourceBox {
    std::unique_ptr<S> source;
    std::vector<std::string> names;
    std::vector<const char*> name_ptrs;
    bool named = false;
};

template <class C>
struct CursorBox {
    std::unique_ptr<C> cursor;
    std::string error;
};

template <class C>
struct CursorVt {
    static dftu_task* next(void* self, std::int64_t max_rows,
                           dftu_result_frame* out) {
        auto* box = static_cast<CursorBox<C>*>(self);
        try {
            std::optional<OwnedFrame> f = box->cursor->next(max_rows);
            out->ok = 1;
            out->u.value = f ? f->release() : nullptr;
        } catch (const std::exception& e) {
            box->error = e.what();
            out->ok = 0;
            out->u.err =
                dftu_error{0, 0, DFTU_COND_UNKNOWN, box->error.c_str()};
        } catch (...) {
            box->error = "source cursor failed";
            out->ok = 0;
            out->u.err =
                dftu_error{0, 0, DFTU_COND_UNKNOWN, box->error.c_str()};
        }
        return nullptr;
    }
    static void destroy(void* self) { delete static_cast<CursorBox<C>*>(self); }
    static dftu_task* narrow(void* self, const dftu_expr* predicate,
                             std::int32_t* out_applied) {
        auto* box = static_cast<CursorBox<C>*>(self);
        try {
            *out_applied = box->cursor->narrow(ExprView(predicate)) ? 1 : 0;
        } catch (...) {
            *out_applied = 0;
        }
        return nullptr;
    }
    static std::uint64_t bytes(void* self) {
        try {
            return static_cast<CursorBox<C>*>(self)->cursor->resident_bytes();
        } catch (...) {
            return 0;
        }
    }
    static std::uint64_t reclaim(void* self, std::uint64_t want) {
        try {
            return static_cast<CursorBox<C>*>(self)->cursor->reclaim(want);
        } catch (...) {
            return 0;
        }
    }
    static const dftu_cursor_vt* vt() {
        static const dftu_cursor_vt v = [] {
            dftu_cursor_vt t{};
            t.next = next;
            t.destroy = destroy;
            if constexpr (requires(C& c) {
                              { c.narrow(ExprView{}) } -> std::same_as<bool>;
                          })
                t.narrow = narrow;
            if constexpr (requires(C& c) {
                              {
                                  c.resident_bytes()
                              } -> std::same_as<std::uint64_t>;
                          })
                t.bytes = bytes;
            if constexpr (requires(C& c) {
                              {
                                  c.reclaim(std::uint64_t{})
                              } -> std::same_as<std::uint64_t>;
                          })
                t.reclaim = reclaim;
            return t;
        }();
        return &v;
    }
};

template <class S>
concept HasSchema = requires(const S& s, SchemaBuilder& b) { s.schema(b); };

template <class S>
concept HasApply =
    requires(const S& s) { s.apply_filter(ExprView{}); } ||
    requires(const S& s, const std::vector<NamedExpr>& v) {
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

inline std::vector<std::string_view> string_views(const char* const* p,
                                                  std::int32_t n) {
    std::vector<std::string_view> out;
    out.reserve(static_cast<std::size_t>(n > 0 ? n : 0));
    for (std::int32_t i = 0; i < n; ++i) out.emplace_back(p[i] ? p[i] : "");
    return out;
}

template <class S>
struct SourceVt {
    using Box = SourceBox<S>;

    static std::int32_t schema(void* self, const char* const** out_names) {
        auto* box = static_cast<Box*>(self);
        try {
            if (!box->named) {
                box->names = box->source->names();
                box->name_ptrs.clear();
                for (const std::string& n : box->names)
                    box->name_ptrs.push_back(n.c_str());
                box->named = true;
            }
        } catch (...) {
            return -1;
        }
        *out_names = box->name_ptrs.data();
        return static_cast<std::int32_t>(box->name_ptrs.size());
    }

    static void* scan(void* self, const dftu_scan_request* req,
                      std::int32_t* out_pushed, void** out_cursor_self,
                      const dftu_cursor_vt** out_vt) {
        auto* box = static_cast<Box*>(self);
        try {
            ScanView view;
            view.projection = string_views(req->projection, req->n_projection);
            for (std::int32_t i = 0; i < req->n_filters; ++i)
                view.filters.emplace_back(req->filters[i]);
            view.limit = req->limit;
            view.memory_budget = req->memory_budget;
            auto result = box->source->scan(view);
            using C = typename decltype(result.cursor)::element_type;
            if (!result.cursor) return nullptr;
            const std::size_t n =
                std::min<std::size_t>(result.filters.size(),
                                      static_cast<std::size_t>(req->n_filters));
            for (std::size_t i = 0; i < n; ++i)
                out_pushed[i] = static_cast<std::int32_t>(result.filters[i]);
            auto* cbox = new CursorBox<C>{std::move(result.cursor), {}};
            *out_cursor_self = cbox;
            *out_vt = CursorVt<C>::vt();
            return cbox;
        } catch (...) {
            return nullptr;
        }
    }

    static void destroy(void* self) { delete static_cast<Box*>(self); }

    static void schema_types(void* self, dftu_schema* out)
        requires HasSchema<S>
    {
        auto* box = static_cast<Box*>(self);
        try {
            SchemaBuilder b(out);
            box->source->schema(b);
        } catch (...) {
        }
    }

    static std::optional<Applied<S>> dispatch(const S& s,
                                              const dftu_apply_request& req) {
        switch (req.kind) {
            case DFTU_APPLY_FILTER:
                if constexpr (requires { s.apply_filter(ExprView{}); })
                    return s.apply_filter(ExprView(req.u.filter.predicate));
                break;
            case DFTU_APPLY_PROJECTION:
                if constexpr (requires(const std::vector<NamedExpr>& v) {
                                  s.apply_projection(v);
                              }) {
                    std::vector<NamedExpr> exprs;
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
                    JoinView<S> v{other, string_views(j.left_on, j.n_on),
                                  string_views(j.right_on, j.n_on),
                                  static_cast<dftu_join_how>(j.how),
                                  j.suffix ? j.suffix : ""};
                    return s.apply_join(v);
                }
                break;
            default:
                break;
        }
        return std::nullopt;
    }

    static SortView sort_view(const dftu_apply_sort_args& a) {
        SortView v;
        v.by = string_views(a.by, a.n_by);
        for (std::int32_t i = 0; i < a.n_by; ++i)
            v.descending.push_back(a.descending[i] != 0);
        return v;
    }

    static void apply(void* self, const dftu_apply_request* req,
                      dftu_apply_result* out) {
        auto* box = static_cast<Box*>(self);
        try {
            std::optional<Applied<S>> r = dispatch(*box->source, *req);
            if (!r || !r->source) return;
            auto* next = new Box{std::move(r->source), {}, {}, false};
            out->status = r->status == Apply::Exact ? DFTU_APPLY_EXACT
                                                    : DFTU_APPLY_INEXACT;
            out->self = next;
            out->vt = vt();
        } catch (...) {
            out->status = DFTU_APPLY_NO_CHANGE;
        }
    }

    static const dftu_source_vt* vt() {
        static const dftu_source_vt v = [] {
            dftu_source_vt t{};
            t.schema = schema;
            t.scan = scan;
            t.destroy = destroy;
            if constexpr (HasSchema<S>) t.schema_types = schema_types;
            if constexpr (HasApply<S>) t.apply = apply;
            return t;
        }();
        return &v;
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
void* make_source_self(std::unique_ptr<S> source) {
    return new detail::SourceBox<S>{std::move(source), {}, {}, false};
}

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_PLUGIN_SOURCE_H
