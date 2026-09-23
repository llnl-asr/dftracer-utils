#include <dftracer/utils/core/common/field_ref.h>
#include <dftracer/utils/core/coro/async_semaphore.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/query/builder.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/indexing/chunk_pruner_utility.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/stream_row_fold.h>
#include <dftracer/utils/trace/views/view_plan.h>
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

}  // namespace

bool ViewSource::can_stream_rows() const {
    if (!view_.is_row_query()) return false;
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
    namespace df = dftracer::utils::dataframe;
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
    // Aggregated / post-scan-op view: buffer() already ran the scan, so its
    // columns' real types are known (unlike the row-query branches above).
    const df::DataFrame& buf = *buffer();
    s.fields.reserve(buf.columns.size());
    for (std::size_t i = 0; i < buf.columns.size(); ++i)
        s.fields.push_back(
            df::Field{buf.names[i], buf.columns[i].data_type(), true});
    return s;
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

}  // namespace dftracer::utils::trace::views
