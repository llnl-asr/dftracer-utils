#include <dftracer/utils/core/common/field_ref.h>
#include <dftracer/utils/core/coro/async_semaphore.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/expr_handle.h>  // expr_fingerprint
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/query/builder.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/indexing/chunk_pruner_utility.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/stream_row_fold.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/trace/views/view_source.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::views {

bool ViewCursor::has_nested_column() const {
    if (!nested_) {
        nested_ = std::any_of(
            buf_->columns.begin(), buf_->columns.end(),
            [](const dftracer::utils::dataframe::Series& c) {
                return c.type() == dftracer::utils::dataframe::TypeId::List ||
                       c.type() == dftracer::utils::dataframe::TypeId::Struct;
            });
    }
    return *nested_;
}

coro::CoroTask<std::optional<dftracer::utils::dataframe::Morsel>>
ViewCursor::next(std::int64_t max_rows) {
    const std::int64_t nrows = buf_->num_rows();
    if (offset_ >= nrows) co_return std::nullopt;

    if (has_nested_column() || max_rows <= 0) {
        dftracer::utils::dataframe::Morsel m;
        m.rows = nrows;
        m.columns.reserve(buf_->columns.size());
        for (const dftracer::utils::dataframe::Series& c : buf_->columns)
            m.columns.push_back(c.share());
        offset_ = nrows;
        m.batch_index = next_index_++;
        m.ordering = dftracer::utils::dataframe::Ordering::Sequence;
        co_return m;
    }

    dftracer::utils::dataframe::DataFrame part = buf_->slice(offset_, max_rows);
    dftracer::utils::dataframe::Morsel m;
    m.rows = part.num_rows();
    m.columns = std::move(part.columns);
    offset_ += m.rows;
    m.batch_index = next_index_++;
    m.ordering = dftracer::utils::dataframe::Ordering::Sequence;
    co_return m;
}

namespace {

namespace df = dftracer::utils::dataframe;
namespace q = dftracer::utils::query;

// What a source can push for one predicate: the query, plus whether it is the
// predicate exactly or merely a superset of it (a superset prunes I/O but must
// be re-applied by the engine).
struct Pushable {
    q::Expr expr;
    bool exact;
};

// The query field a predicate over LazyFrame column `ci` names, or nullopt
// when it cannot be pushed: an out-of-range column, or ts/dur under a
// time_scale (the streamed morsel is scaled, the query matches the raw field).
std::optional<std::string> pushable_field(
    std::int32_t ci, const std::vector<std::string>& fnames,
    double time_scale) {
    if (ci < 0 || static_cast<std::size_t>(ci) >= fnames.size())
        return std::nullopt;
    const std::string& col = fnames[static_cast<std::size_t>(ci)];
    if ((col == "ts" || col == "dur") && time_scale != 1.0) return std::nullopt;
    // The query DSL names a nested arg field by its bare key; a top-level field
    // is unchanged.
    return std::string(dftracer::utils::strip_args_prefix(col));
}

// A `col <str pred> pattern` leaf. LIKE is the same glob on both sides, so it
// pushes exactly; the DSL has no case-sensitive substring form, so Contains
// pushes as ICONTAINS, a superset the engine re-applies; StartsWith and
// EndsWith push as an escaped LIKE affix. Matches (whole-string) and Search
// stay with the engine: the DSL regex searches, and rewriting anchors is not
// worth a wrong answer.
std::optional<Pushable> translate_str_pred(
    const df::Expr& e, const std::vector<std::string>& fnames,
    double time_scale) {
    std::int32_t ci = -1;
    df::StrPredOp op{};
    std::string_view pattern;
    if (!df::expr_as_col_str_pred(e, &ci, &op, &pattern)) return std::nullopt;
    std::optional<std::string> field = pushable_field(ci, fnames, time_scale);
    if (!field) return std::nullopt;
    auto escape_like = [](std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '%' || c == '_' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    };
    switch (op) {
        case df::StrPredOp::Contains:
            return Pushable{
                q::field_match(*field, q::MatchOp::ICONTAINS, pattern), false};
        case df::StrPredOp::StartsWith:
            return Pushable{q::field_match(*field, q::MatchOp::LIKE,
                                           escape_like(pattern) + "%"),
                            true};
        case df::StrPredOp::EndsWith:
            return Pushable{q::field_match(*field, q::MatchOp::LIKE,
                                           "%" + escape_like(pattern)),
                            true};
        case df::StrPredOp::Like:
            return Pushable{q::field_match(*field, q::MatchOp::LIKE, pattern),
                            true};
        case df::StrPredOp::Matches:
        case df::StrPredOp::Search:
            return std::nullopt;
    }
    return std::nullopt;
}

// The integer value set of `values` as int64, for any integer width; nullopt
// for a non-integer type or a Uint64 the int64 domain cannot hold.
std::optional<std::vector<std::int64_t>> int_values(const df::Series& values) {
    switch (values.type()) {
        case df::TypeId::Int8:
        case df::TypeId::Int16:
        case df::TypeId::Int32:
        case df::TypeId::Int64:
        case df::TypeId::Uint8:
        case df::TypeId::Uint16:
        case df::TypeId::Uint32:
            break;
        case df::TypeId::Uint64: {
            const std::uint64_t* u = values.data<std::uint64_t>();
            for (std::int64_t i = 0; i < values.length(); ++i)
                if (u[i] > static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max()))
                    return std::nullopt;
            break;
        }
        case df::TypeId::Unknown:
        case df::TypeId::Bool:
        case df::TypeId::Float16:
        case df::TypeId::Float32:
        case df::TypeId::Float64:
        case df::TypeId::String:
        case df::TypeId::LargeString:
        case df::TypeId::Binary:
        case df::TypeId::LargeBinary:
        case df::TypeId::FixedSizeBinary:
        case df::TypeId::Date32:
        case df::TypeId::Date64:
        case df::TypeId::Timestamp:
        case df::TypeId::Time32:
        case df::TypeId::Time64:
        case df::TypeId::Duration:
        case df::TypeId::Decimal128:
        case df::TypeId::Decimal256:
        case df::TypeId::List:
        case df::TypeId::LargeList:
        case df::TypeId::Struct:
        case df::TypeId::FixedSizeList:
        case df::TypeId::Map:
            return std::nullopt;
    }
    df::Series as64 = values.type() == df::TypeId::Int64
                          ? values.share()
                          : values.cast(df::TypeId::Int64);
    if (!as64.valid() || as64.null_count() != 0) return std::nullopt;
    return std::vector<std::int64_t>(as64.data<std::int64_t>(),
                                     as64.data<std::int64_t>() + as64.length());
}

// A `col is_in values` leaf: exact for an integer or String value set.
std::optional<Pushable> translate_is_in(const df::Expr& e,
                                        const std::vector<std::string>& fnames,
                                        double time_scale) {
    std::int32_t ci = -1;
    df::Series values;
    if (!df::expr_as_col_is_in(e, &ci, &values)) return std::nullopt;
    std::optional<std::string> field = pushable_field(ci, fnames, time_scale);
    if (!field) return std::nullopt;
    if (values.null_count() != 0) return std::nullopt;
    if (std::optional<std::vector<std::int64_t>> v = int_values(values))
        return Pushable{q::field_in(*field, *v), true};
    if (values.type() == df::TypeId::String) {
        std::vector<std::string> v;
        v.reserve(static_cast<std::size_t>(values.length()));
        for (std::int64_t i = 0; i < values.length(); ++i)
            v.emplace_back(values.string_at(i));
        return Pushable{q::field_in(*field, v), true};
    }
    return std::nullopt;
}

// Translate a `col <cmp> scalar` LazyFrame predicate into an index-pushable
// query on the named event field. nullopt for anything else (col-vs-col,
// arithmetic), which the engine applies itself.
std::optional<q::Expr> translate_leaf(const df::Expr& e,
                                      const std::vector<std::string>& fnames,
                                      double time_scale) {
    std::int32_t ci = -1;
    df::CmpOp op{};
    df::Scalar rhs;
    if (!df::expr_as_col_cmp(e, &ci, &op, &rhs)) return std::nullopt;
    std::optional<std::string> pushed = pushable_field(ci, fnames, time_scale);
    if (!pushed) return std::nullopt;
    const std::string field = std::move(*pushed);

    q::CompareOp qop;
    switch (op) {
        case df::CmpOp::Gt:
            qop = q::CompareOp::GT;
            break;
        case df::CmpOp::Ge:
            qop = q::CompareOp::GE;
            break;
        case df::CmpOp::Lt:
            qop = q::CompareOp::LT;
            break;
        case df::CmpOp::Le:
            qop = q::CompareOp::LE;
            break;
        case df::CmpOp::Eq:
            qop = q::CompareOp::EQ;
            break;
        case df::CmpOp::Ne:
            qop = q::CompareOp::NE;
            break;
    }

    q::LiteralNode lit;
    switch (rhs.tag()) {
        case DFTU_SCALAR_TAG_I64:
            lit = q::LiteralNode{rhs.i64()};
            break;
        case DFTU_SCALAR_TAG_U64:
            lit = q::LiteralNode{rhs.u64()};
            break;
        case DFTU_SCALAR_TAG_STR:
            lit = q::LiteralNode{std::string(rhs.str())};
            break;
        case DFTU_SCALAR_TAG_F64:
            lit = q::LiteralNode{rhs.f64()};
            break;
        default:
            return std::nullopt;
    }

    return q::field_cmp(field, qop, std::move(lit));
}

// The whole predicate tree, pushing as much of it as is sound:
//
//   AND - one untranslatable side does not sink the other; pushing just the
//         translatable conjunct selects a SUPERSET, which prunes I/O and is
//         re-applied by the engine (Inexact).
//   OR  - both sides must translate. Pushing one alone would drop rows the
//         other side keeps.
//   NOT - the operand must translate EXACTLY. Negating a superset yields a
//         subset, which drops rows.
std::optional<Pushable> translate_pred(const df::Expr& e,
                                       const std::vector<std::string>& fnames,
                                       double time_scale) {
    df::LogicalOp lop{};
    df::Expr lhs, rhs;
    if (df::expr_as_logical(e, &lop, &lhs, &rhs)) {
        std::optional<Pushable> a = translate_pred(lhs, fnames, time_scale);
        std::optional<Pushable> b = translate_pred(rhs, fnames, time_scale);
        if (lop == df::LogicalOp::And) {
            if (a && b)
                return Pushable{
                    q::all_of(std::move(a->expr), std::move(b->expr)),
                    a->exact && b->exact};
            if (a) return Pushable{std::move(a->expr), false};
            if (b) return Pushable{std::move(b->expr), false};
            return std::nullopt;
        }
        if (!a || !b) return std::nullopt;
        return Pushable{q::any_of(std::move(a->expr), std::move(b->expr)),
                        a->exact && b->exact};
    }

    df::Expr inner;
    if (df::expr_as_not(e, &inner)) {
        std::optional<Pushable> a = translate_pred(inner, fnames, time_scale);
        if (!a || !a->exact) return std::nullopt;
        return Pushable{q::negate(std::move(a->expr)), true};
    }

    if (auto sp = translate_str_pred(e, fnames, time_scale)) return sp;
    if (auto in = translate_is_in(e, fnames, time_scale)) return in;
    std::optional<q::Expr> leaf = translate_leaf(e, fnames, time_scale);
    if (!leaf) return std::nullopt;
    return Pushable{std::move(*leaf), true};
}

coro::Coro run_detached(coro::CoroTask<void> task,
                        std::shared_ptr<std::promise<void>> done) {
    try {
        co_await std::move(task);
        done->set_value();
    } catch (...) {
        done->set_exception(std::current_exception());
    }
}

// Enqueues onto Executor::current() rather than default_runtime(), so the
// producer shares whatever is pulling the cursor - a real pool, or the ad hoc
// RunLoop a bare CoroTask::get() stands up, which cannot wait across pools.
std::shared_future<void> spawn_on_current_executor(coro::CoroTask<void> task) {
    dftracer::utils::Executor* exec = dftracer::utils::Executor::current();
    if (!exec) exec = dftracer::utils::default_runtime().executor();
    if (task.handle()) task.handle().promise().set_executor(exec);
    auto done = std::make_shared<std::promise<void>>();
    std::shared_future<void> fut = done->get_future().share();
    coro::Coro c = run_detached(std::move(task), std::move(done));
    exec->enqueue(c.release());
    return fut;
}

// Rows [offset, offset+n) of `m`, sharing `m`'s schema (name_ids/intern) since
// a row slice does not change it.
dftracer::utils::dataframe::Morsel slice_morsel(
    const dftracer::utils::dataframe::Morsel& m, std::int64_t offset,
    std::int64_t n) {
    dftracer::utils::dataframe::DataFrame tmp;
    tmp.names.assign(m.columns.size() + m.dyn_columns.size(), std::string());
    for (const dftracer::utils::dataframe::Series& c : m.columns)
        tmp.columns.push_back(c.share());
    for (const dftracer::utils::dataframe::Series& c : m.dyn_columns)
        tmp.columns.push_back(c.share());
    dftracer::utils::dataframe::DataFrame s = tmp.slice(offset, n);

    dftracer::utils::dataframe::Morsel out;
    out.rows = n;
    out.name_ids = m.name_ids;
    out.intern = m.intern;
    out.dyn_names = m.dyn_names;
    const std::size_t nc = m.columns.size();
    out.columns.assign(
        std::make_move_iterator(s.columns.begin()),
        std::make_move_iterator(s.columns.begin() +
                                static_cast<std::ptrdiff_t>(nc)));
    out.dyn_columns.assign(
        std::make_move_iterator(s.columns.begin() +
                                static_cast<std::ptrdiff_t>(nc)),
        std::make_move_iterator(s.columns.end()));
    return out;
}

// Pulls morsels off the channel, releasing each one's share of `budget_` back
// to the fold's producers once it is handed off; surfaces the producer's
// exception, if any, once the channel drains. A caller-supplied `max_rows`
// re-chunks a received morsel into <=max_rows-row sub-morsels instead of
// handing the whole (one-per-scan-batch) morsel out at once.
class StreamViewCursor : public dftracer::utils::dataframe::Cursor {
   public:
    StreamViewCursor(
        std::shared_ptr<coro::Channel<dftracer::utils::dataframe::Morsel>>
            channel,
        std::shared_ptr<coro::CoroSemaphore> budget,
        std::shared_future<void> producer,
        std::shared_ptr<std::atomic<bool>> stop,
        std::shared_ptr<detail::DynamicPrune> dyn_prune,
        std::vector<ViewFile> files, double time_scale,
        std::vector<std::string> fnames,
        dftracer::utils::trace::indexing::BloomFilterCache* bloom_cache)
        : channel_(std::move(channel)),
          budget_(std::move(budget)),
          producer_(std::move(producer)),
          stop_(std::move(stop)),
          dyn_prune_(std::move(dyn_prune)),
          files_(std::move(files)),
          time_scale_(time_scale),
          fnames_(std::move(fnames)),
          bloom_cache_(bloom_cache) {}

    // Abandoning the cursor must stop the scan behind it: the producer holds
    // its own channel registration, so nothing else ends it, and the byte
    // budget is released only here - a producer that outlives the cursor parks
    // in acquire() forever. Setting the flag alone cannot wake a parked
    // producer (the semaphore has no shutdown), so release enough permits for
    // it to run to its next is_cancelled() check and unwind.
    ~StreamViewCursor() override {
        stop_->store(true, std::memory_order_relaxed);
        budget_->release(std::numeric_limits<std::uint32_t>::max());
    }

    // The fold behind `channel_` fans out across scan workers (see
    // ViewSource::open_stream / run_folds), so morsels can land here out of
    // the order the underlying data was produced in. batch_index is still
    // stamped (this cursor's own receive-order counter, for diagnostics) but
    // ordering stays Unordered - claiming Sequence here would be a lie a
    // windowed consumer could act on.
    coro::CoroTask<std::optional<dftracer::utils::dataframe::Morsel>> next(
        std::int64_t max_rows) override {
        if (max_rows <= 0) {
            auto item = co_await channel_->receive();
            if (item) {
                budget_->release(detail::morsel_bytes(*item));
                item->batch_index = next_index_++;
                item->ordering =
                    dftracer::utils::dataframe::Ordering::Unordered;
            } else {
                producer_.get();
            }
            co_return item;
        }

        if (!pending_ || offset_ >= pending_->rows) {
            auto item = co_await channel_->receive();
            if (!item) {
                producer_.get();
                co_return std::nullopt;
            }
            pending_bytes_ = detail::morsel_bytes(*item);
            pending_ = std::move(item);
            offset_ = 0;
        }

        const std::int64_t n = std::min(max_rows, pending_->rows - offset_);
        dftracer::utils::dataframe::Morsel out =
            slice_morsel(*pending_, offset_, n);
        out.batch_index = next_index_++;
        out.ordering = dftracer::utils::dataframe::Ordering::Unordered;
        offset_ += n;
        if (offset_ >= pending_->rows) {
            budget_->release(pending_bytes_);
            pending_.reset();
        }
        co_return out;
    }

    // ADVISORY, like every other pushdown here: translate_pred decides what
    // is even worth pruning with, and a translated predicate only ever
    // shrinks the candidate checkpoint set the same way the static prune in
    // scan() does - it never marks a checkpoint excluded on ambiguous
    // pruner output (a failed lookup, an unindexed file), so a narrow() the
    // engine misapplies can only cost extra I/O, never a wrong row: every
    // row this cursor still yields is filtered again by the engine.
    coro::CoroTask<bool> narrow(
        const dftracer::utils::dataframe::Expr& predicate) override {
        if (!dyn_prune_ || files_.empty()) co_return false;
        std::optional<Pushable> pushed =
            translate_pred(predicate, fnames_, time_scale_);
        if (!pushed) co_return false;
        auto built = std::move(pushed->expr).build();
        if (!built.has_value()) co_return false;

        bool any = false;
        for (const ViewFile& f : files_) {
            dftracer::utils::trace::indexing::ChunkPrunerInput pin{
                f.index_path, f.file_path, built.value(), bloom_cache_};
            dftracer::utils::trace::indexing::ChunkPrunerUtility pruner;
            auto out = co_await pruner(pin);
            if (!out.success) continue;  // ambiguous - leave every unit as is
            if (!out.file_may_match) {
                // A definite bloom/dictionary miss: the file provably has no
                // matching event, regardless of chunk count.
                dyn_prune_->exclude_file(f.file_path);
                any = true;
                continue;
            }
            if (out.total_checkpoints == 0) continue;  // nothing to prune by
            std::unordered_set<std::uint64_t> keep(
                out.candidate_checkpoints.begin(),
                out.candidate_checkpoints.end());
            std::vector<std::uint64_t> excluded;
            for (std::uint64_t c = 0; c < out.total_checkpoints; ++c)
                if (!keep.count(c)) excluded.push_back(c);
            if (!excluded.empty()) {
                dyn_prune_->exclude_checkpoints(f.file_path,
                                                std::move(excluded));
                any = true;
            }
        }
        co_return any;
    }

   private:
    std::shared_ptr<coro::Channel<dftracer::utils::dataframe::Morsel>> channel_;
    std::shared_ptr<coro::CoroSemaphore> budget_;
    std::shared_future<void> producer_;
    std::shared_ptr<std::atomic<bool>> stop_;
    std::shared_ptr<detail::DynamicPrune> dyn_prune_;
    std::vector<ViewFile> files_;
    double time_scale_ = 1.0;
    std::vector<std::string> fnames_;
    dftracer::utils::trace::indexing::BloomFilterCache* bloom_cache_ = nullptr;
    std::optional<dftracer::utils::dataframe::Morsel> pending_;
    std::int64_t offset_ = 0;
    std::uint64_t pending_bytes_ = 0;
    std::int64_t next_index_ = 0;
};

// A group key the view folds natively with the raw value, named as its output
// column. cat is left out: the view lowercases it, the engine does not.
std::optional<GroupKey> fixed_group_key(const std::string& name) {
    if (name == "name") return GroupKey::name();
    if (name == "pid") return GroupKey::pid();
    if (name == "tid") return GroupKey::tid();
    if (name == "fhash") return GroupKey::fhash();
    if (name == "hhash") return GroupKey::hhash();
    return std::nullopt;
}

// Numeric event fields every event carries, so the view's per-event fold and
// the engine's column reduction see the same values.
bool is_fixed_numeric_field(const std::string& name) {
    return name == "ts" || name == "dur" || name == "pid" || name == "tid";
}

std::optional<AggOp> simple_agg_op(df::Agg op) {
    switch (op) {
        case df::Agg::Count:
            return AggOp::Count;
        case df::Agg::Sum:
            return AggOp::Sum;
        case df::Agg::Min:
            return AggOp::Min;
        case df::Agg::Max:
            return AggOp::Max;
        case df::Agg::Mean:
            return AggOp::Mean;
        default:
            return std::nullopt;
    }
}

// The source column a bare column reference reads, or nullopt.
std::optional<std::string> column_of(const df::Expr& e,
                                     const std::vector<std::string>& names) {
    const std::int32_t ci = df::expr_col_index(e);
    if (ci < 0 || static_cast<std::size_t>(ci) >= names.size())
        return std::nullopt;
    return names[static_cast<std::size_t>(ci)];
}

}  // namespace

std::optional<df::SourceApplication> ViewSource::apply_filter(
    const df::Expr& predicate) const {
    if (!can_stream_rows()) return std::nullopt;
    const std::uint64_t fp = df::expr_fingerprint(predicate);
    if (std::find(applied_filters_.begin(), applied_filters_.end(), fp) !=
        applied_filters_.end())
        return std::nullopt;
    std::optional<Pushable> pushed =
        translate_pred(predicate, names(), view_.plan_->time_scale);
    if (!pushed) return std::nullopt;
    auto built = std::move(pushed->expr).build();
    if (!built.has_value()) return std::nullopt;
    auto next = std::make_shared<ViewSource>(
        view_.filter(std::move(built.value())), emit_dyn_);
    next->applied_filters_ = applied_filters_;
    next->applied_filters_.push_back(fp);
    next->absorb_aggregation_ = absorb_aggregation_;
    return df::SourceApplication{
        std::move(next),
        pushed->exact ? df::ApplyStatus::Exact : df::ApplyStatus::Inexact};
}

std::optional<df::SourceApplication> ViewSource::apply_projection(
    const std::vector<df::NamedExpr>& exprs) const {
    if (!can_stream_rows()) return std::nullopt;
    const std::vector<std::string> current = names();
    std::vector<std::string> cols;
    cols.reserve(exprs.size());
    for (const df::NamedExpr& e : exprs) {
        std::optional<std::string> c = column_of(e.expr, current);
        if (!c || *c != e.name) return std::nullopt;
        cols.push_back(std::move(*c));
    }
    if (cols == current) return std::nullopt;
    auto next = std::make_shared<ViewSource>(view_.select(cols), emit_dyn_);
    next->applied_filters_ = applied_filters_;
    next->absorb_aggregation_ = absorb_aggregation_;
    if (next->names() != cols) return std::nullopt;
    return df::SourceApplication{std::move(next), df::ApplyStatus::Exact};
}

namespace {

// `e`, positional against `current`, as a computed column over the columns it
// reads, or std::nullopt when it reads none it can name.
std::optional<detail::ComputedColumn> computed_from(
    std::string name, const df::Expr& e,
    const std::vector<std::string>& current) {
    detail::ComputedColumn c;
    c.name = std::move(name);
    std::vector<std::int32_t> at(current.size(), -1);
    for (std::size_t i = 0; i < current.size(); ++i) {
        if (!df::expr_references(e, static_cast<std::int32_t>(i))) continue;
        at[i] = static_cast<std::int32_t>(c.inputs.size());
        c.inputs.push_back(current[i]);
    }
    if (df::expr_max_col(e) >= static_cast<std::int32_t>(current.size()))
        return std::nullopt;
    c.expr = df::expr_remap_cols(e, at);
    return c;
}

}  // namespace

std::optional<df::SourceApplication> ViewSource::apply_aggregation(
    const df::AggregateSpec& spec) const {
    const detail::ViewPlan& p = *view_.plan_;
    if (output_ != TraceOutput::Events || !absorb_aggregation_ ||
        !view_.is_row_query() || p.auto_numeric_metrics)
        return std::nullopt;
    const std::vector<std::string> current = names();
    std::vector<GroupKey> keys;
    std::vector<detail::ComputedColumn> computed;
    std::vector<std::string> raw_reads;
    std::vector<std::string> expected;
    for (const df::NamedExpr& k : spec.keys) {
        std::optional<std::string> c = column_of(k.expr, current);
        if (c && *c == k.name)
            if (std::optional<GroupKey> key = fixed_group_key(*c)) {
                keys.push_back(std::move(*key));
                raw_reads.push_back(*c);
                expected.push_back(k.name);
                continue;
            }
        std::optional<detail::ComputedColumn> col =
            computed_from(k.name, k.expr, current);
        if (!col) return std::nullopt;
        keys.push_back({GroupKey::Kind::Expr, k.name});
        computed.push_back(std::move(*col));
        expected.push_back(k.name);
    }
    std::vector<AggSpec> aggs;
    for (std::size_t i = 0; i < spec.aggs.size(); ++i) {
        const df::AggregateExpr& a = spec.aggs[i];
        std::optional<AggOp> op = simple_agg_op(a.op);
        if (!op || a.by.valid() || a.out.empty()) return std::nullopt;
        std::string field;
        if (*op != AggOp::Count) {
            std::optional<std::string> c = column_of(a.input, current);
            if (c && is_fixed_numeric_field(*c)) {
                field = *c;
                raw_reads.push_back(*c);
            } else {
                field = "__view_agg_in_" + std::to_string(i);
                std::optional<detail::ComputedColumn> col =
                    computed_from(field, a.input, current);
                if (!col) return std::nullopt;
                computed.push_back(std::move(*col));
            }
        }
        aggs.push_back(AggSpec(*op, std::move(field), a.out));
        expected.push_back(a.out);
    }
    if (!computed.empty()) {
        // The view reads ts/dur unscaled for some aggregates; a computed column
        // must see the values the row stream would.
        if (p.time_scale != 1.0) return std::nullopt;
        // A computed column replaces a same-named field for every reader, so
        // it may not shadow one the aggregation reads raw.
        for (const detail::ComputedColumn& c : computed)
            if (std::find(raw_reads.begin(), raw_reads.end(), c.name) !=
                raw_reads.end())
                return std::nullopt;
    }
    // A row select here only narrowed the scan to what the aggregation reads;
    // on an aggregation the view would read it as an output projection.
    View grouped = view_.select({});
    if (!computed.empty()) {
        auto plan = std::make_shared<detail::ViewPlan>(*grouped.plan_);
        plan->computed = std::move(computed);
        plan->schema.reset();
        grouped = View(std::move(plan));
    }
    if (!keys.empty()) grouped = grouped.group_by(std::move(keys));
    grouped = grouped.agg(std::move(aggs));
    auto next = std::make_shared<ViewSource>(std::move(grouped), emit_dyn_);
    if (next->names() != expected) return std::nullopt;
    return df::SourceApplication{std::move(next), df::ApplyStatus::Exact};
}

std::optional<std::string> ViewSource::batch_key() const {
    const detail::ViewPlan& p = *view_.plan_;
    if (emit_dyn_ || !absorb_aggregation_ || p.cancelled || p.materialize ||
        p.auto_numeric_metrics || !p.sort_col.empty() || !p.topk_col.empty() ||
        p.limit || p.offset)
        return std::nullopt;
    if (output_ == TraceOutput::Events && view_.is_row_query() &&
        detail::select_needs_resolver(p.select))
        return std::nullopt;
    // The session's export branch writes whole events.
    if (output_ == TraceOutput::ExportJson && !p.select.empty())
        return std::nullopt;
    // A branch sees the whole shared scan, so only an unfiltered one shares.
    if (output_ == TraceOutput::Branch && (p.query || p.phase != Phase::Any))
        return std::nullopt;
    std::string key;
    for (const ViewFile& f : p.files) {
        key += f.file_path;
        key += '\0';
        key += f.index_path;
        key += '\0';
    }
    key += std::to_string(reinterpret_cast<std::uintptr_t>(p.bloom_cache));
    key += '\0';
    if (p.time_range)
        key += std::to_string(p.time_range->first) + ',' +
               std::to_string(p.time_range->second);
    key += '\0';
    key += std::to_string(p.time_scale) + '\0' +
           std::to_string(p.include_metadata) +
           std::to_string(p.emit_all_metadata) + '\0' + p.rollup_root + '\0' +
           p.views_root + '\0' + std::to_string(p.memory_budget);
    return key;
}

namespace {

View session_base(const detail::ViewPlan& p) {
    View base = View::from_files(p.files, p.bloom_cache)
                    .metadata(p.include_metadata)
                    .emit_all_metadata(p.emit_all_metadata)
                    .time_scale(p.time_scale)
                    .rollup_root(p.rollup_root)
                    .views_root(p.views_root)
                    .memory_budget(p.memory_budget);
    if (p.time_range)
        base = base.time_range(p.time_range->first, p.time_range->second);
    return base;
}

// A zero-copy view of `f`, for members that read the same output.
df::DataFrame shared_frame(const df::DataFrame& f) {
    df::DataFrame out;
    out.names = f.names;
    out.columns.reserve(f.columns.size());
    for (const df::Series& c : f.columns) out.columns.push_back(c.share());
    return out;
}

bool is_containment(TraceOutput o) {
    return o == TraceOutput::CallTree || o == TraceOutput::Flamegraph ||
           o == TraceOutput::FlamegraphPartial;
}

}  // namespace

std::vector<std::function<df::DataFrame()>> ViewSource::add_branches(
    ViewSession& session, const std::vector<const ViewSource*>& members,
    const std::vector<bool>& skip) {
    // Containment members over the same events and fields share one buffered
    // fold, whichever of its outputs each one reads.
    struct Tree {
        const View* view;
        const ContainmentArgs* args;
        std::shared_ptr<df::DataFrame> call_tree, flamegraph;
        std::shared_ptr<std::string> partial;
    };
    std::vector<Tree> trees;
    std::vector<std::size_t> tree_of(members.size(), 0);
    for (std::size_t i = 0; i < members.size(); ++i) {
        const ViewSource& m = *members[i];
        if (skip[i] || !is_containment(m.output_)) continue;
        auto it = std::find_if(trees.begin(), trees.end(), [&](const Tree& t) {
            return t.view->plan_ == m.view_.plan_ && *t.args == m.tree_;
        });
        if (it == trees.end()) {
            trees.push_back(
                Tree{&m.view_, &m.tree_, nullptr, nullptr, nullptr});
            it = std::prev(trees.end());
        }
        tree_of[i] = static_cast<std::size_t>(it - trees.begin());
        if (m.output_ == TraceOutput::CallTree && !it->call_tree)
            it->call_tree = std::make_shared<df::DataFrame>();
        if (m.output_ == TraceOutput::Flamegraph && !it->flamegraph)
            it->flamegraph = std::make_shared<df::DataFrame>();
        if (m.output_ == TraceOutput::FlamegraphPartial && !it->partial)
            it->partial = std::make_shared<std::string>();
    }
    for (const Tree& t : trees)
        session.add_containment_branch(*t.view, t.args->partition, t.args->ts,
                                       t.args->dur, t.args->name, t.args->group,
                                       t.call_tree, t.flamegraph, t.partial);

    std::vector<std::function<df::DataFrame()>> out(members.size());
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (skip[i]) continue;
        const ViewSource& m = *members[i];
        switch (m.output_) {
            case TraceOutput::Events: {
                Deferred<df::DataFrame> h =
                    m.view_.is_row_query() ? session.collect_events(m.view_)
                                           : session.collect(m.view_);
                out[i] = [h] { return std::move(h.get()); };
                break;
            }
            case TraceOutput::CallTree:
                out[i] = [f = trees[tree_of[i]].call_tree] {
                    return shared_frame(*f);
                };
                break;
            case TraceOutput::Flamegraph:
                out[i] = [f = trees[tree_of[i]].flamegraph] {
                    return shared_frame(*f);
                };
                break;
            case TraceOutput::FlamegraphPartial:
                out[i] = [p = trees[tree_of[i]].partial] {
                    return detail::partial_frame(*p);
                };
                break;
            case TraceOutput::AggregatePartial: {
                Deferred<std::string> h = session.aggregate_partial(m.view_);
                out[i] = [h] { return detail::partial_frame(h.get()); };
                break;
            }
            case TraceOutput::Branch: {
                std::function<void()> publish = m.branch_(session);
                out[i] = [publish] {
                    publish();
                    return df::DataFrame{};
                };
                break;
            }
            case TraceOutput::ExportJson: {
                std::optional<Query> q =
                    detail::effective_query(*m.view_.plan_);
                Deferred<ExportStats> h =
                    q ? session.export_json(std::move(*q), *m.sink_)
                      : session.export_json(*m.sink_);
                out[i] = [h, sink = m.sink_] {
                    sink->flush();
                    return detail::stats_frame(h.get());
                };
                break;
            }
        }
    }
    return out;
}

coro::CoroTask<ViewSource::Batch> ViewSource::run_batch(
    std::vector<std::shared_ptr<const ViewSource>> members) {
    Batch out;
    if (members.empty()) co_return out;
    ViewSession session = session_base(*members.front()->view_.plan_).session();
    std::vector<const ViewSource*> raw;
    raw.reserve(members.size());
    for (const auto& m : members) raw.push_back(m.get());
    std::vector<std::function<df::DataFrame()>> resolve =
        add_branches(session, raw, std::vector<bool>(raw.size(), false));
    out.stats = co_await session.execute();
    out.frames.reserve(resolve.size());
    for (auto& r : resolve) out.frames.push_back(r());
    co_return out;
}

namespace {

// The shared scan every open_batch cursor reads from. Holding each channel's
// producer slot until it ends means every channel closes, even when the scan
// fails before its folds exist.
coro::CoroTask<void> run_open_batch(
    std::shared_ptr<ViewSession> session,
    std::vector<std::function<df::DataFrame()>> frames,
    std::vector<std::shared_ptr<coro::Channel<df::Morsel>>> channels,
    std::vector<std::shared_ptr<coro::CoroSemaphore>> budgets) {
    std::vector<std::unique_ptr<coro::Channel<df::Morsel>::ProducerGuard>>
        guards;
    guards.reserve(channels.size());
    for (auto& ch : channels)
        guards.push_back(
            std::make_unique<coro::Channel<df::Morsel>::ProducerGuard>(
                ch.get()));
    co_await session->execute();
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (!frames[i]) continue;
        df::DataFrame f = frames[i]();
        df::Morsel m;
        m.rows = f.num_rows();
        m.columns = std::move(f.columns);
        const std::uint64_t bytes = detail::morsel_bytes(m);
        co_await budgets[i]->acquire(bytes);
        co_await channels[i]->send(std::move(m));
    }
}

}  // namespace

std::optional<std::vector<std::unique_ptr<df::Cursor>>> ViewSource::open_batch(
    std::vector<std::shared_ptr<const df::Source>> members,
    std::uint64_t memory_budget) const {
    std::vector<const ViewSource*> views;
    views.reserve(members.size());
    for (const auto& m : members) {
        const auto* v = dynamic_cast<const ViewSource*>(m.get());
        if (!v || (v->output_ == TraceOutput::Events &&
                   v->view_.is_row_query() && !v->can_stream_rows()))
            return std::nullopt;
        views.push_back(v);
    }
    // Streaming only pays for members whose rows it bounds; finished frames
    // hand over directly through collect_batch().
    if (std::none_of(views.begin(), views.end(),
                     [](const ViewSource* v) { return v->can_stream_rows(); }))
        return std::nullopt;

    auto session = std::make_shared<ViewSession>(
        session_base(*views.front()->view_.plan_).session());
    std::vector<std::shared_ptr<coro::Channel<df::Morsel>>> channels;
    std::vector<std::shared_ptr<coro::CoroSemaphore>> budgets;
    std::vector<std::shared_ptr<std::atomic<bool>>> dropped;
    std::vector<bool> streamed(views.size(), false);
    for (std::size_t i = 0; i < views.size(); ++i) {
        channels.push_back(coro::make_channel<df::Morsel>(0));
        budgets.push_back(std::make_shared<coro::CoroSemaphore>(memory_budget));
        dropped.push_back(std::make_shared<std::atomic<bool>>(false));
        const ViewSource& v = *views[i];
        if (v.can_stream_rows()) {
            detail::add_stream_branch(*session->state_, v.view_.plan_,
                                      channels[i], budgets[i], dropped[i]);
            streamed[i] = true;
        }
    }
    std::vector<std::function<df::DataFrame()>> frames =
        add_branches(*session, views, streamed);

    std::shared_future<void> producer = spawn_on_current_executor(
        run_open_batch(session, std::move(frames), channels, budgets));
    std::vector<std::unique_ptr<df::Cursor>> cursors;
    cursors.reserve(views.size());
    for (std::size_t i = 0; i < views.size(); ++i)
        cursors.push_back(std::make_unique<StreamViewCursor>(
            channels[i], budgets[i], producer, dropped[i], nullptr,
            std::vector<ViewFile>{}, 1.0, std::vector<std::string>{}, nullptr));
    return cursors;
}

coro::CoroTask<std::vector<df::DataFrame>> ViewSource::collect_batch(
    std::vector<std::shared_ptr<const df::Source>> members) const {
    std::vector<std::shared_ptr<const ViewSource>> views;
    views.reserve(members.size());
    for (auto& m : members)
        views.push_back(std::static_pointer_cast<const ViewSource>(m));
    Batch b = co_await run_batch(std::move(views));
    co_return std::move(b.frames);
}

bool ViewSource::can_stream_rows() const {
    if (output_ != TraceOutput::Events || !view_.is_row_query()) return false;
    const detail::ViewPlan& p = *view_.plan_;
    // View::collect() always strips sort/topk/offset/limit before building a
    // ViewSource, and select unless it needs the resolver (resolved.*/r.*
    // fields, which the raw stream never computes); these checks stay as a
    // defensive guard for any other caller.
    return p.sort_col.empty() && p.topk_col.empty() && p.offset == 0 &&
           p.limit == 0 && !detail::select_needs_resolver(p.select);
}

// Matches build_row_frame's own empty-select order (fixed top-level fields,
// fhash/hhash if present, then sorted args), from index metadata rather than
// a scan - best-effort, may omit an arg key not yet indexed.
std::vector<std::string> ViewSource::row_schema() const {
    static const char* const TOP_LEVEL[] = {"name", "cat", "pid", "tid",
                                            "ts",   "dur", "ph"};
    std::vector<std::string> out(std::begin(TOP_LEVEL), std::end(TOP_LEVEL));

    std::vector<std::string> cols = view_.columns();  // sorted
    const bool has_fhash =
        std::binary_search(cols.begin(), cols.end(), std::string("fhash"));
    const bool has_hhash =
        std::binary_search(cols.begin(), cols.end(), std::string("hhash"));
    if (has_fhash) out.push_back("fhash");
    if (has_hhash) out.push_back("hhash");

    for (std::string& c : cols) {
        if (c == "pid" || c == "tid" || c == "ts" || c == "dur" ||
            c == "name" || c == "cat" || c == "fhash" || c == "hhash")
            continue;
        if (c.find('.') != std::string::npos) continue;  // nested/resolved.*
        // A flattened arg column: build_row_frame's empty-select branch
        // always names these "args.<key>", so the schemas must match.
        out.push_back(std::string(dftracer::utils::ARGS_PREFIX) + c);
    }
    return out;
}

dftracer::utils::dataframe::Schema ViewSource::schema() const {
    std::lock_guard<std::mutex> lock(schema_mu_);
    if (!schema_) schema_ = compute_schema();
    return *schema_;
}

namespace {

df::Schema fixed_schema(
    std::initializer_list<std::pair<const char*, df::TypeId>> cols) {
    df::Schema s;
    for (const auto& [name, type] : cols)
        s.fields.push_back(df::Field{name, df::scalar(type), true});
    return s;
}

}  // namespace

dftracer::utils::dataframe::Schema ViewSource::compute_schema() const {
    namespace df = dftracer::utils::dataframe;
    using T = df::TypeId;
    switch (output_) {
        case TraceOutput::Events:
            break;
        case TraceOutput::CallTree:
            return fixed_schema({{"pid", T::Int64},
                                 {"tid", T::Int64},
                                 {"ts", T::Int64},
                                 {"dur", T::Int64},
                                 {"name", T::String},
                                 {"level", T::Int64},
                                 {"parent_id", T::Int64}});
        case TraceOutput::Flamegraph:
            return fixed_schema({{"node_id", T::Int64},
                                 {"parent", T::Int64},
                                 {"name", T::String},
                                 {"level", T::Int64},
                                 {"total", T::Float64},
                                 {"self", T::Float64},
                                 {"count", T::Int64}});
        case TraceOutput::FlamegraphPartial:
        case TraceOutput::AggregatePartial:
            return fixed_schema({{"partial", T::Binary}});
        case TraceOutput::Branch:
            return df::Schema{};
        case TraceOutput::ExportJson:
            return fixed_schema({{"events_matched", T::Int64},
                                 {"events_scanned", T::Int64},
                                 {"chunks_scanned", T::Int64},
                                 {"chunks_skipped", T::Int64},
                                 {"chunks_covered", T::Int64},
                                 {"artifacts_committed", T::Bool},
                                 {"truncated", T::Bool},
                                 {"served_from_mv", T::Bool}});
    }
    df::Schema s;
    if (can_stream_rows()) {
        // row_column_type() reports TypeId::Unknown for a flattened arg
        // column, since it is data-dependent and the function has no index
        // access; fill those in from the same harvest columns()/schema() use,
        // so name and type cannot disagree.
        std::unordered_map<std::string, df::TypeId> harvested;
        auto resolve = [&](const std::string& name) {
            df::TypeId id = detail::row_column_type(name);
            if (id != df::TypeId::Unknown) return id;
            if (harvested.empty()) harvested = view_.column_types();
            std::string_view key = name;
            if (key.rfind(dftracer::utils::ARGS_PREFIX, 0) == 0)
                key.remove_prefix(dftracer::utils::ARGS_PREFIX.size());
            auto it = harvested.find(std::string(key));
            return it == harvested.end() ? id : it->second;
        };
        // A non-empty select fixes every streamed morsel's columns to exactly
        // this list (see open_stream()), so the schema must match it, not the
        // broader index-derived row_schema(). Canonicalize each select entry
        // the same way build_row_frame does, so a bare arg name (or one
        // colliding with a top-level field) resolves to the same column name
        // the producer actually emits.
        if (!view_.plan_->select.empty()) {
            s.fields.reserve(view_.plan_->select.size());
            for (const std::string& sel : view_.plan_->select) {
                std::string col_name = detail::canonical_row_column_name(sel);
                s.fields.push_back(
                    df::Field{col_name, df::scalar(resolve(col_name)), true});
            }
        } else {
            std::vector<std::string> names = row_schema();
            s.fields.reserve(names.size());
            for (const std::string& name : names)
                s.fields.push_back(
                    df::Field{name, df::scalar(resolve(name)), true});
        }
        return s;
    }
    // An aggregation's columns follow from its plan, so planning and explain()
    // never run it. A data-dependent (numeric-arg) aggregation, a row query
    // the stream cannot serve, or a plan still carrying an output select read
    // them from the result instead.
    if (!view_.is_row_query() && view_.plan_->select.empty())
        if (std::optional<df::Schema> planned =
                detail::aggregated_output_schema(*view_.plan_))
            return *planned;
    const df::DataFrame& buf = *buffer();
    s.fields.reserve(buf.columns.size());
    for (std::size_t i = 0; i < buf.columns.size(); ++i)
        s.fields.push_back(
            df::Field{buf.names[i], buf.columns[i].data_type(), true});
    return s;
}

coro::CoroTask<df::DataFrame> ViewSource::run_alone() const {
    const ContainmentArgs& t = tree_;
    switch (output_) {
        case TraceOutput::Events:
            break;
        case TraceOutput::CallTree:
            co_return co_await view_.call_tree(t.partition, t.ts, t.dur,
                                               t.name);
        case TraceOutput::Flamegraph:
            co_return co_await view_.flamegraph(t.partition, t.ts, t.dur,
                                                t.name, t.group);
        case TraceOutput::FlamegraphPartial:
            co_return detail::partial_frame(co_await view_.flamegraph_partial(
                t.partition, t.ts, t.dur, t.name, t.group));
        case TraceOutput::AggregatePartial:
            co_return detail::partial_frame(co_await view_.aggregate_partial());
        case TraceOutput::ExportJson: {
            const ExportStats stats = co_await view_.export_json(*sink_);
            sink_->flush();
            co_return detail::stats_frame(stats);
        }
        case TraceOutput::Branch: {
            ViewSession session = view_.session();
            std::function<void()> publish = branch_(session);
            co_await session.execute();
            publish();
            co_return df::DataFrame{};
        }
    }
    co_return co_await view_.collect_frame();
}

const dftracer::utils::dataframe::DataFrame* ViewSource::as_frame() const {
    if (can_stream_rows()) return nullptr;
    return buffer().get();
}

std::unique_ptr<dftracer::utils::dataframe::Cursor> ViewSource::open_stream(
    const View& v, std::uint64_t memory_budget,
    std::vector<std::string> fnames) const {
    // Capacity 0 = an effectively unbounded ring (see Channel's ctor); the
    // shared budget semaphore is the sole backpressure, acquired before send
    // and released once the cursor hands a morsel off.
    auto channel = coro::make_channel<dftracer::utils::dataframe::Morsel>(0);
    auto budget = std::make_shared<coro::CoroSemaphore>(memory_budget);
    auto intern = std::make_shared<dftracer::utils::StringIntern>();
    const double time_scale = v.plan_->time_scale;
    // Backing state for Cursor::narrow(): fuse() polls it per unit for as
    // long as this scan runs, so a narrow() call after the scan has already
    // started can still prune units it has not claimed yet.
    auto dyn_prune = std::make_shared<detail::DynamicPrune>();

    // The cursor's early-out: fuse polls the plan's cancel predicate per unit
    // and per batch, so the flag is composed into it rather than added beside
    // it. Composed, never overwritten, so a caller's own cancel_when survives.
    auto stop = std::make_shared<std::atomic<bool>>(false);
    View scan_view = v.cancel_when([stop, prev = v.plan_->cancelled] {
        return stop->load(std::memory_order_relaxed) || (prev && prev());
    });

    // Empty select: each batch discovers its own columns from the actual
    // scanned events, so morsels can differ batch to batch; name_ids lets
    // drain_to_frame reconcile them. A non-empty select instead fixes every
    // morsel's columns to that exact list (build_row_frame's select branch
    // always emits each one, null-filled where absent), matching schema().
    auto task =
        [](View vv, double ts,
           std::shared_ptr<coro::Channel<dftracer::utils::dataframe::Morsel>>
               ch,
           std::shared_ptr<coro::CoroSemaphore> sem,
           std::shared_ptr<dftracer::utils::StringIntern> iv,
           std::shared_ptr<detail::DynamicPrune> dp,
           bool emit_dyn) -> coro::CoroTask<void> {
        detail::StreamRowFold fold(ch, sem, iv, vv.plan_->select, ts, nullptr,
                                   vv.plan_->phase == Phase::Metadata,
                                   emit_dyn);
        std::array<detail::Fold*, 1> folds{&fold};
        co_await vv.run_folds(folds, *iv, dp.get());
    }(scan_view, time_scale, channel, budget, intern, dyn_prune, emit_dyn_);

    std::shared_future<void> producer =
        spawn_on_current_executor(std::move(task));
    return std::make_unique<StreamViewCursor>(
        std::move(channel), std::move(budget), std::move(producer),
        std::move(stop), std::move(dyn_prune), v.plan_->files, time_scale,
        std::move(fnames), v.plan_->bloom_cache);
}

dftracer::utils::dataframe::ScanResult ViewSource::scan(
    const dftracer::utils::dataframe::ScanRequest& req) const {
    dftracer::utils::dataframe::ScanResult r;
    r.filters.assign(req.filters.size(),
                     dftracer::utils::dataframe::Pushed::No);

    // Aggregated / post-scan-op view: buffer once (the resident fast path uses
    // as_frame()). No filter/query pushdown here; project the buffer to honor
    // the requested column set. The engine applies the residual filters.
    if (!can_stream_rows()) {
        std::shared_ptr<const dftracer::utils::dataframe::DataFrame> buf =
            buffer();
        if (!req.projection.empty())
            buf = std::make_shared<const dftracer::utils::dataframe::DataFrame>(
                buf->select(req.projection));
        r.cursor = std::make_unique<ViewCursor>(std::move(buf));
        return r;
    }

    // Streamable row query: translate each simple predicate into the View's
    // query (Exact) and push projection into View::select so the fold harvests
    // only those columns.
    const std::vector<std::string> fnames =
        req.projection.empty() ? names() : req.projection;
    const double time_scale = view_.plan_->time_scale;
    View v = view_;
    for (std::size_t i = 0; i < req.filters.size(); ++i) {
        std::optional<Pushable> pushed =
            translate_pred(req.filters[i], fnames, time_scale);
        if (!pushed) continue;
        auto built = std::move(pushed->expr).build();
        if (!built.has_value()) continue;
        v = v.filter(std::move(built.value()));
        r.filters[i] = pushed->exact
                           ? dftracer::utils::dataframe::Pushed::Exact
                           : dftracer::utils::dataframe::Pushed::Inexact;
    }
    if (!req.projection.empty()) v = v.select(req.projection);

    r.cursor = open_stream(v, req.memory_budget, fnames);
    return r;
}

namespace detail {

df::DataFrame partial_frame(std::string_view partial) {
    const std::int32_t offsets[2] = {0,
                                     static_cast<std::int32_t>(partial.size())};
    df::DataFrame f;
    f.names = {"partial"};
    f.columns.push_back(df::Series{
        dftu_series_new_string(static_cast<dftu_dtype>(df::TypeId::Binary),
                               offsets, partial.data(), 1, nullptr)});
    return f;
}

std::string partial_of(const df::DataFrame& frame) {
    return std::string(frame.column("partial").materialize().string_at(0));
}

df::DataFrame stats_frame(const ExportStats& stats) {
    df::DataFrame f;
    auto count = [&](const char* name, std::uint64_t v) {
        const auto x = static_cast<std::int64_t>(v);
        f.names.emplace_back(name);
        f.columns.push_back(df::Series::flat_i64(&x, 1));
    };
    auto flag = [&](const char* name, bool v) {
        const std::uint8_t bits = v ? 1 : 0;
        f.names.emplace_back(name);
        f.columns.push_back(df::Series::flat(df::TypeId::Bool, &bits, 1));
    };
    count("events_matched", stats.events_matched);
    count("events_scanned", stats.events_scanned);
    count("chunks_scanned", stats.chunks_scanned);
    count("chunks_skipped", stats.chunks_skipped);
    count("chunks_covered", stats.chunks_covered);
    flag("artifacts_committed", stats.artifacts_committed);
    flag("truncated", stats.truncated);
    flag("served_from_mv", stats.served_from_mv);
    return f;
}

ExportStats stats_of(const df::DataFrame& frame) {
    auto count = [&](const char* name) {
        return static_cast<std::uint64_t>(
            frame.column(name).materialize().data<std::int64_t>()[0]);
    };
    auto flag = [&](const char* name) { return frame.column(name).any(); };
    ExportStats s;
    s.events_matched = count("events_matched");
    s.events_scanned = count("events_scanned");
    s.chunks_scanned = count("chunks_scanned");
    s.chunks_skipped = count("chunks_skipped");
    s.chunks_covered = count("chunks_covered");
    s.artifacts_committed = flag("artifacts_committed");
    s.truncated = flag("truncated");
    s.served_from_mv = flag("served_from_mv");
    return s;
}

}  // namespace detail

}  // namespace dftracer::utils::trace::views
