// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/field_ref.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/expr_handle.h>  // expr_fingerprint
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/future.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/builder.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/stream_row_fold.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/trace/views/view_source.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <utility>

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

coro::CoroTask<dstd::Optional<dftracer::utils::dataframe::Morsel>>
ViewCursor::next(std::int64_t max_rows) {
    const std::int64_t nrows = buf_->num_rows();
    if (offset_ >= nrows) co_return dstd::nullopt;

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

// What a source can push for one predicate: the query, plus whether it is the
// predicate exactly or merely a superset of it (a superset prunes I/O but must
// be re-applied by the engine).
struct Pushable {
    duql::Expr expr;
    bool exact;
};

dstd::Vector<dstd::String> trace_names(
    const dstd::Vector<dstd::String>& names) {
    dstd::Vector<dstd::String> out;
    out.reserve(names.size());
    for (const auto& n : names) out.emplace_back(n);
    return out;
}

bool same_names(const dstd::Vector<dstd::String>& a,
                const dstd::Vector<dstd::String>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](const dstd::String& x, const dstd::String& y) {
                          return dstd::StringView(x) == dstd::StringView(y);
                      });
}

// The query field a predicate over LazyFrame column `ci` names, or nullopt
// when it cannot be pushed: an out-of-range column, or ts/dur under a
// time_scale (the streamed morsel is scaled, the query matches the raw field).
dstd::Optional<dstd::String> pushable_field(
    std::int32_t ci, const dstd::Vector<dstd::String>& fnames,
    double time_scale) {
    if (ci < 0 || static_cast<std::size_t>(ci) >= fnames.size())
        return dstd::nullopt;
    const dstd::String& col = fnames[static_cast<std::size_t>(ci)];
    if ((col == "ts" || col == "dur") && time_scale != 1.0)
        return dstd::nullopt;
    // The query DSL names a nested arg field by its bare key; a top-level field
    // is unchanged.
    return dstd::String(dftracer::utils::common::strip_args_prefix(col));
}

// A `col <str pred> pattern` leaf. LIKE is the same glob on both sides, so it
// pushes exactly; the DSL has no case-sensitive substring form, so Contains
// pushes as ICONTAINS, a superset the engine re-applies; StartsWith and
// EndsWith push as an escaped LIKE affix. Matches (whole-string) and Search
// stay with the engine: the DSL regex searches, and rewriting anchors is not
// worth a wrong answer.
dstd::Optional<Pushable> translate_str_pred(
    const dataframe::Expr& e, const dstd::Vector<dstd::String>& fnames,
    double time_scale) {
    std::int32_t ci = -1;
    dataframe::StrPredOp op{};
    dstd::StringView pattern;
    if (!dataframe::expr_as_col_str_pred(e, &ci, &op, &pattern))
        return dstd::nullopt;
    dstd::Optional<dstd::String> field = pushable_field(ci, fnames, time_scale);
    if (!field) return dstd::nullopt;
    auto escape_like = [](dstd::StringView s) {
        dstd::String out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '%' || c == '_' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    };
    switch (op) {
        case dataframe::StrPredOp::Contains:
            return Pushable{
                duql::field_match(*field, duql::MatchOp::Icontains, pattern),
                false};
        case dataframe::StrPredOp::StartsWith:
            return Pushable{duql::field_match(*field, duql::MatchOp::Like,
                                              escape_like(pattern) + "%"),
                            true};
        case dataframe::StrPredOp::EndsWith:
            return Pushable{duql::field_match(*field, duql::MatchOp::Like,
                                              "%" + escape_like(pattern)),
                            true};
        case dataframe::StrPredOp::Like:
            return Pushable{
                duql::field_match(*field, duql::MatchOp::Like, pattern), true};
        case dataframe::StrPredOp::Matches:
        case dataframe::StrPredOp::Search:
            return dstd::nullopt;
    }
    return dstd::nullopt;
}

// The integer value set of `values` as int64, for any integer width; nullopt
// for a non-integer type or a Uint64 the int64 domain cannot hold.
dstd::Optional<dstd::Vector<std::int64_t>> int_values(
    const dataframe::Series& column) {
    const dataframe::Series values =
        column.is_flat() ? column.share() : column.materialize();
    switch (values.type()) {
        case dataframe::TypeId::Int8:
        case dataframe::TypeId::Int16:
        case dataframe::TypeId::Int32:
        case dataframe::TypeId::Int64:
        case dataframe::TypeId::Uint8:
        case dataframe::TypeId::Uint16:
        case dataframe::TypeId::Uint32:
            break;
        case dataframe::TypeId::Uint64: {
            const std::uint64_t* u = values.data<std::uint64_t>();
            for (std::int64_t i = 0; i < values.length(); ++i)
                if (u[i] > static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max()))
                    return dstd::nullopt;
            break;
        }
        case dataframe::TypeId::Unknown:
        case dataframe::TypeId::Bool:
        case dataframe::TypeId::Float16:
        case dataframe::TypeId::Float32:
        case dataframe::TypeId::Float64:
        case dataframe::TypeId::String:
        case dataframe::TypeId::LargeString:
        case dataframe::TypeId::Binary:
        case dataframe::TypeId::LargeBinary:
        case dataframe::TypeId::FixedSizeBinary:
        case dataframe::TypeId::Date32:
        case dataframe::TypeId::Date64:
        case dataframe::TypeId::Timestamp:
        case dataframe::TypeId::Time32:
        case dataframe::TypeId::Time64:
        case dataframe::TypeId::Duration:
        case dataframe::TypeId::Decimal128:
        case dataframe::TypeId::Decimal256:
        case dataframe::TypeId::List:
        case dataframe::TypeId::LargeList:
        case dataframe::TypeId::Struct:
        case dataframe::TypeId::FixedSizeList:
        case dataframe::TypeId::Map:
            return dstd::nullopt;
    }
    dataframe::Series as64 = values.type() == dataframe::TypeId::Int64
                                 ? values.share()
                                 : values.cast(dataframe::TypeId::Int64);
    if (!as64.valid() || as64.null_count() != 0) return dstd::nullopt;
    return dstd::Vector<std::int64_t>(
        as64.data<std::int64_t>(), as64.data<std::int64_t>() + as64.length());
}

// A `col is_in values` leaf: exact for an integer or String value set.
dstd::Optional<Pushable> translate_is_in(
    const dataframe::Expr& e, const dstd::Vector<dstd::String>& fnames,
    double time_scale) {
    std::int32_t ci = -1;
    dataframe::Series values;
    if (!dataframe::expr_as_col_is_in(e, &ci, &values)) return dstd::nullopt;
    dstd::Optional<dstd::String> field = pushable_field(ci, fnames, time_scale);
    if (!field) return dstd::nullopt;
    if (values.null_count() != 0) return dstd::nullopt;
    if (dstd::Optional<dstd::Vector<std::int64_t>> v = int_values(values))
        return Pushable{duql::field_in(*field, *v), true};
    if (values.type() == dataframe::TypeId::String) {
        dstd::Vector<dstd::String> v;
        v.reserve(static_cast<std::size_t>(values.length()));
        for (std::int64_t i = 0; i < values.length(); ++i)
            v.emplace_back(values.string_at(i));
        return Pushable{duql::field_in(*field, v), true};
    }
    return dstd::nullopt;
}

// Translate a `col <cmp> scalar` LazyFrame predicate into an index-pushable
// query on the named event field. nullopt for anything else (col-vs-col,
// arithmetic), which the engine applies itself.
dstd::Optional<duql::Expr> translate_leaf(
    const dataframe::Expr& e, const dstd::Vector<dstd::String>& fnames,
    double time_scale) {
    std::int32_t ci = -1;
    dataframe::CmpOp op{};
    dataframe::Scalar rhs;
    if (!dataframe::expr_as_col_cmp(e, &ci, &op, &rhs)) return dstd::nullopt;
    dstd::Optional<dstd::String> pushed =
        pushable_field(ci, fnames, time_scale);
    if (!pushed) return dstd::nullopt;
    const dstd::String field = std::move(*pushed);

    duql::CompareOp qop;
    switch (op) {
        case dataframe::CmpOp::Gt:
            qop = duql::CompareOp::Gt;
            break;
        case dataframe::CmpOp::Ge:
            qop = duql::CompareOp::Ge;
            break;
        case dataframe::CmpOp::Lt:
            qop = duql::CompareOp::Lt;
            break;
        case dataframe::CmpOp::Le:
            qop = duql::CompareOp::Le;
            break;
        case dataframe::CmpOp::Eq:
            qop = duql::CompareOp::Eq;
            break;
        case dataframe::CmpOp::Ne:
            qop = duql::CompareOp::Ne;
            break;
    }

    duql::LiteralNode lit;
    switch (rhs.tag()) {
        case DFTU_SCALAR_TAG_I64:
            lit = duql::LiteralNode{rhs.i64()};
            break;
        case DFTU_SCALAR_TAG_U64:
            lit = duql::LiteralNode{rhs.u64()};
            break;
        case DFTU_SCALAR_TAG_STR:
            lit = duql::LiteralNode{dstd::String(rhs.str())};
            break;
        case DFTU_SCALAR_TAG_F64:
            lit = duql::LiteralNode{rhs.f64()};
            break;
        default:
            return dstd::nullopt;
    }

    return duql::field_cmp(field, qop, std::move(lit));
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
dstd::Optional<Pushable> translate_pred(
    const dataframe::Expr& e, const dstd::Vector<dstd::String>& fnames,
    double time_scale) {
    dataframe::LogicalOp lop{};
    dataframe::Expr lhs, rhs;
    if (dataframe::expr_as_logical(e, &lop, &lhs, &rhs)) {
        dstd::Optional<Pushable> a = translate_pred(lhs, fnames, time_scale);
        dstd::Optional<Pushable> b = translate_pred(rhs, fnames, time_scale);
        if (lop == dataframe::LogicalOp::And) {
            if (a && b)
                return Pushable{
                    duql::all_of(std::move(a->expr), std::move(b->expr)),
                    a->exact && b->exact};
            if (a) return Pushable{std::move(a->expr), false};
            if (b) return Pushable{std::move(b->expr), false};
            return dstd::nullopt;
        }
        if (!a || !b) return dstd::nullopt;
        return Pushable{duql::any_of(std::move(a->expr), std::move(b->expr)),
                        a->exact && b->exact};
    }

    dataframe::Expr inner;
    if (dataframe::expr_as_not(e, &inner)) {
        dstd::Optional<Pushable> a = translate_pred(inner, fnames, time_scale);
        if (!a || !a->exact) return dstd::nullopt;
        return Pushable{duql::negate(std::move(a->expr)), true};
    }

    if (auto sp = translate_str_pred(e, fnames, time_scale)) return sp;
    if (auto in = translate_is_in(e, fnames, time_scale)) return in;
    dstd::Optional<duql::Expr> leaf = translate_leaf(e, fnames, time_scale);
    if (!leaf) return dstd::nullopt;
    return Pushable{std::move(*leaf), true};
}

coro::detail::Coro run_detached(
    coro::CoroTask<void> task,
    dstd::SharedPtr<dstd::Promise<dstd::Result<void>>> done,
    dstd::SharedFuture<dstd::Result<void>> fut) {
    auto settled = co_await coro::settle(std::move(task));
    if (settled.exception) {
        done->set_exception(std::move(settled.exception));
    } else if (settled.stopped) {
        dftracer::utils::detail::fulfill(
            *done, fut,
            dstd::Unexpected<dstd::Error>(coro::detail::stopped_error()));
    } else {
        dftracer::utils::detail::fulfill(*done, fut, dstd::Result<void>{});
    }
}

// Enqueues onto Executor::current() rather than default_runtime(), so the
// producer shares whatever is pulling the cursor - a real pool, or the ad hoc
// coro::RunLoop a bare CoroTask::get() stands up, which cannot wait across
// pools.
dstd::SharedFuture<dstd::Result<void>> spawn_on_current_executor(
    coro::CoroTask<void> task) {
    dftracer::utils::coro::Executor* exec =
        dftracer::utils::coro::Executor::current();
    if (!exec) exec = dftracer::utils::default_runtime().executor();
    if (task.handle()) task.handle().promise().set_executor(exec);
    auto done = dstd::make_shared<dstd::Promise<dstd::Result<void>>>();
    dstd::SharedFuture<dstd::Result<void>> fut = done->get_future().share();
    coro::detail::Coro c = run_detached(std::move(task), std::move(done), fut);
    exec->post(c.release());
    return fut;
}

// Rows [offset, offset+n) of `m`, sharing `m`'s schema (name_ids/intern) since
// a row slice does not change it.
dftracer::utils::dataframe::Morsel slice_morsel(
    const dftracer::utils::dataframe::Morsel& m, std::int64_t offset,
    std::int64_t n) {
    dftracer::utils::dataframe::DataFrame tmp;
    tmp.names.assign(m.columns.size(), dstd::String());
    for (const dftracer::utils::dataframe::Series& c : m.columns)
        tmp.columns.push_back(c.share());
    dftracer::utils::dataframe::DataFrame s = tmp.slice(offset, n);

    dftracer::utils::dataframe::Morsel out;
    out.rows = n;
    out.columns = std::move(s.columns);
    if (!m.dyn) return out;
    out.dyn_state().name_ids = m.dyn->name_ids;
    out.dyn->intern = m.dyn->intern;
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
        dstd::SharedPtr<
            coro::UnboundedChannel<dftracer::utils::dataframe::Morsel>>
            channel,
        dstd::SharedPtr<coro::AsyncSemaphore> budget,
        dstd::SharedFuture<dstd::Result<void>> producer,
        dstd::SharedPtr<std::atomic<bool>> stop,
        dstd::SharedPtr<detail::DynamicPrune> dyn_prune,
        dstd::Vector<ViewFile> files, double time_scale,
        dstd::Vector<dstd::String> fnames, bool metadata_rows,
        dstd::SharedPtr<coro::AsyncSemaphore> gate)
        : channel_(std::move(channel)),
          budget_(std::move(budget)),
          producer_(std::move(producer)),
          stop_(std::move(stop)),
          dyn_prune_(std::move(dyn_prune)),
          files_(std::move(files)),
          time_scale_(time_scale),
          fnames_(std::move(fnames)),
          metadata_rows_(metadata_rows),
          gate_(std::move(gate)) {}

    // Abandoning the cursor must stop the scan behind it: the producer holds
    // its own channel registration, so nothing else ends it, and the byte
    // budget is released only here - a producer that outlives the cursor parks
    // in acquire() forever. Setting the flag alone cannot wake a parked
    // producer (the semaphore has no shutdown), so release enough permits for
    // it to run to its next is_cancelled() check and unwind. Closing the
    // channel wakes a producer parked on a full one the same way; left open,
    // it would hold every frame and morsel behind it until the process ends.
    ~StreamViewCursor() override {
        stop_->store(true, std::memory_order_relaxed);
        channel_->close();
        budget_->release(std::numeric_limits<std::uint32_t>::max());
        if (gate_) gate_->release(std::numeric_limits<std::uint32_t>::max());
    }

    // The fold behind `channel_` fans out across scan workers (see
    // ViewSource::open_stream / run_folds), so morsels land here in the order
    // workers finish them. Without `gate_` they go out that way, Unordered;
    // with it they go out in unit order (see receive()), a Sequence.
    // batch_index is this cursor's own hand-out counter.
    coro::CoroTask<dstd::Optional<dftracer::utils::dataframe::Morsel>> next(
        std::int64_t max_rows) override {
        const auto ordering =
            gate_ ? dftracer::utils::dataframe::Ordering::Sequence
                  : dftracer::utils::dataframe::Ordering::Unordered;
        if (max_rows <= 0) {
            std::uint64_t owed = 0;
            auto item = co_await receive(owed);
            if (item) {
                budget_->release(owed);
                item->batch_index = next_index_++;
                item->ordering = ordering;
            } else if (!producer_.get()) {
                co_await coro::stop_here();
            }
            co_return item;
        }

        if (!pending_ || offset_ >= pending_->rows) {
            auto item = co_await receive(pending_bytes_);
            if (!item) {
                if (!producer_.get()) co_await coro::stop_here();
                co_return dstd::nullopt;
            }
            pending_ = std::move(item);
            offset_ = 0;
        }

        const std::int64_t n = std::min(max_rows, pending_->rows - offset_);
        dftracer::utils::dataframe::Morsel out =
            slice_morsel(*pending_, offset_, n);
        out.batch_index = next_index_++;
        out.ordering = ordering;
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
        dstd::Optional<Pushable> pushed =
            translate_pred(predicate, fnames_, time_scale_);
        if (!pushed) co_return false;
        auto built = std::move(pushed->expr).build();
        if (!built.has_value()) co_return false;

        bool any = false;
        dstd::Map<dstd::String, dftracer::utils::index::Snapshot> snapshots;
        for (const ViewFile& f : files_) {
            // One snapshot per index for the whole call. A failed open leaves
            // none, and prune_file reports it.
            const dftracer::utils::index::Snapshot* snapshot = nullptr;
            if (!f.index_path.empty()) {
                auto at = snapshots.find(f.index_path);
                if (at == snapshots.end())
                    if (auto taken = index::plan::open_snapshot(f.index_path))
                        at = snapshots.emplace(f.index_path, std::move(*taken))
                                 .first;
                if (at != snapshots.end()) snapshot = &at->second;
            }
            // GCC 12 destroys a temporary built inside co_await twice.
            index::plan::PruneRequest prune_req{
                .index_path = dstd::String(dstd::StringView(f.index_path)),
                .file_path = dstd::String(dstd::StringView(f.file_path)),
                .query = &built.value(),
                .snapshot = snapshot,
                .metadata = metadata_rows_ ? index::plan::MetadataUse::Records
                                           : index::plan::MetadataUse::None};
            auto out = co_await index::plan::prune_file(std::move(prune_req));
            if (!out->file_may_match) {
                dyn_prune_->exclude_file(f.file_path);
                any = true;
                continue;
            }
            if (out->all_chunks) continue;
            dstd::Vector<std::uint64_t> excluded;
            std::size_t k = 0;
            for (std::uint64_t c = 0; c < out->total_chunks; ++c) {
                while (k < out->candidates.size() && out->candidates[k] < c)
                    ++k;
                if (k == out->candidates.size() || out->candidates[k] != c)
                    excluded.push_back(c);
            }
            if (!excluded.empty()) {
                dyn_prune_->exclude_checkpoints(f.file_path,
                                                std::move(excluded));
                any = true;
            }
        }
        co_return any;
    }

   private:
    using Morsel = dftracer::utils::dataframe::Morsel;

    // The next morsel with rows, or nullopt once the channel drains; `owed`
    // is the budget it still holds. With `gate_`, morsels come in unit order:
    // the ordered fold stamps each with its unit's seq and ends each unit with
    // an empty morsel. One that arrives before its turn is held, its budget
    // returned at once so no producer waits on budget the hold keeps; the
    // gate bounds how many units can be held. Units a stopped scan never
    // ended go out in seq order once the channel drains.
    coro::CoroTask<dstd::Optional<Morsel>> receive(std::uint64_t& owed) {
        if (!gate_) {
            auto item = co_await channel_->receive();
            owed = item ? detail::morsel_bytes(*item) : 0;
            co_return item;
        }
        owed = 0;
        for (;;) {
            auto it = held_.find(head_);
            if (it != held_.end() && !it->second.empty()) {
                Morsel m = std::move(it->second.front());
                it->second.pop_front();
                co_return m;
            }
            if (ended_.erase(head_) > 0 || (drained_ && it != held_.end())) {
                if (it != held_.end()) held_.erase(it);
                ++head_;
                gate_->release(1);
                continue;
            }
            if (drained_) {
                if (held_.empty()) co_return dstd::nullopt;
                head_ = held_.begin()->first;
                continue;
            }
            auto item = co_await channel_->receive();
            if (!item) {
                drained_ = true;
                continue;
            }
            budget_->release(detail::morsel_bytes(*item));
            const auto seq = static_cast<std::size_t>(item->batch_index);
            if (item->rows == 0)
                ended_.insert(seq);
            else if (seq == head_)
                co_return item;
            else
                held_[seq].push_back(std::move(*item));
        }
    }

    dstd::SharedPtr<coro::UnboundedChannel<dftracer::utils::dataframe::Morsel>>
        channel_;
    dstd::SharedPtr<coro::AsyncSemaphore> budget_;
    dstd::SharedFuture<dstd::Result<void>> producer_;
    dstd::SharedPtr<std::atomic<bool>> stop_;
    dstd::SharedPtr<detail::DynamicPrune> dyn_prune_;
    dstd::Vector<ViewFile> files_;
    double time_scale_ = 1.0;
    dstd::Vector<dstd::String> fnames_;
    // The rows are metadata records, which data evidence says nothing about.
    bool metadata_rows_ = false;
    dstd::Optional<dftracer::utils::dataframe::Morsel> pending_;
    std::int64_t offset_ = 0;
    std::uint64_t pending_bytes_ = 0;
    std::int64_t next_index_ = 0;
    dstd::SharedPtr<coro::AsyncSemaphore> gate_;
    std::size_t head_ = 0;
    dstd::Map<std::size_t, dstd::Deque<Morsel>> held_;
    dstd::HashSet<std::size_t> ended_;
    bool drained_ = false;
};

// A group key the view folds natively with the raw value, named as its output
// column. cat is left out: the view lowercases it, the engine does not.
dstd::Optional<GroupKey> fixed_group_key(const dstd::String& name) {
    if (name == "name") return GroupKey::name();
    if (name == "pid") return GroupKey::pid();
    if (name == "tid") return GroupKey::tid();
    if (name == "fhash") return GroupKey::fhash();
    if (name == "hhash") return GroupKey::hhash();
    return dstd::nullopt;
}

// Numeric event fields every event carries, so the view's per-event fold and
// the engine's column reduction see the same values.
bool is_fixed_numeric_field(const dstd::String& name) {
    return name == "ts" || name == "dur" || name == "pid" || name == "tid";
}

dstd::Optional<AggOp> simple_agg_op(dataframe::Agg op) {
    switch (op) {
        case dataframe::Agg::Count:
            return AggOp::Count;
        case dataframe::Agg::Sum:
            return AggOp::Sum;
        case dataframe::Agg::Min:
            return AggOp::Min;
        case dataframe::Agg::Max:
            return AggOp::Max;
        case dataframe::Agg::Mean:
            return AggOp::Mean;
        default:
            return dstd::nullopt;
    }
}

// The source column a bare column reference reads, or nullopt.
dstd::Optional<dstd::String> column_of(
    const dataframe::Expr& e, const dstd::Vector<dstd::String>& names) {
    const std::int32_t ci = dataframe::expr_col_index(e);
    if (ci < 0 || static_cast<std::size_t>(ci) >= names.size())
        return dstd::nullopt;
    return names[static_cast<std::size_t>(ci)];
}

}  // namespace

dstd::Optional<dataframe::SourceApplication> ViewSource::apply_filter(
    const dataframe::Expr& predicate) const {
    if (distinct_ || !can_stream_rows()) return dstd::nullopt;
    const std::uint64_t fp = dataframe::expr_fingerprint(predicate);
    if (std::find(applied_filters_.begin(), applied_filters_.end(), fp) !=
        applied_filters_.end())
        return dstd::nullopt;
    dstd::Optional<Pushable> pushed =
        translate_pred(predicate, trace_names(names()), plan_->time_scale);
    if (!pushed) return dstd::nullopt;
    auto built = std::move(pushed->expr).build();
    if (!built.has_value()) return dstd::nullopt;
    auto next = dstd::make_shared<ViewSource>(
        detail::scan::filter(plan_, std::move(built.value())), emit_dyn_);
    next->applied_filters_ = applied_filters_;
    next->applied_filters_.push_back(fp);
    next->absorb_aggregation_ = absorb_aggregation_;
    return dataframe::SourceApplication{
        std::move(next), pushed->exact ? dataframe::ApplyStatus::Exact
                                       : dataframe::ApplyStatus::Inexact};
}

dstd::Optional<dataframe::SourceApplication> ViewSource::apply_projection(
    const dstd::Vector<dataframe::NamedExpr>& exprs) const {
    if (distinct_ || !can_stream_rows()) return dstd::nullopt;
    const dstd::Vector<dstd::String> current = trace_names(names());
    dstd::Vector<dstd::String> cols;
    cols.reserve(exprs.size());
    for (const dataframe::NamedExpr& e : exprs) {
        dstd::Optional<dstd::String> c = column_of(e.expr, current);
        if (!c || dstd::StringView(*c) != dstd::StringView(e.name))
            return dstd::nullopt;
        cols.push_back(std::move(*c));
    }
    if (cols == current) return dstd::nullopt;
    auto next = dstd::make_shared<ViewSource>(detail::scan::select(plan_, cols),
                                              emit_dyn_);
    next->applied_filters_ = applied_filters_;
    next->absorb_aggregation_ = absorb_aggregation_;
    if (!same_names(next->names(), cols)) return dstd::nullopt;
    return dataframe::SourceApplication{std::move(next),
                                        dataframe::ApplyStatus::Exact};
}

namespace {

// `e`, positional against `current` (the scan's select tokens, so a list
// column is rebuilt as a list), as a computed column over the columns it
// reads, or dstd::nullopt when it reads none it can name.
dstd::Optional<detail::ComputedColumn> computed_from(
    dstd::String name, const dataframe::Expr& e,
    const dstd::Vector<dstd::String>& current) {
    detail::ComputedColumn c;
    c.name = std::move(name);
    dstd::Vector<std::int32_t> at(current.size(), -1);
    for (std::size_t i = 0; i < current.size(); ++i) {
        if (!dataframe::expr_references(e, static_cast<std::int32_t>(i)))
            continue;
        at[i] = static_cast<std::int32_t>(c.inputs.size());
        c.inputs.push_back(current[i]);
    }
    if (dataframe::expr_max_col(e) >= static_cast<std::int32_t>(current.size()))
        return dstd::nullopt;
    c.expr = dataframe::expr_remap_cols(e, at);
    return c;
}

}  // namespace

dstd::Optional<dataframe::SourceApplication> ViewSource::apply_aggregation(
    const dataframe::AggregateSpec& spec) const {
    const detail::ViewPlan& p = *plan_;
    if (distinct_ || output_ != TraceOutput::Events || !absorb_aggregation_ ||
        !detail::is_row_query(*plan_) || p.auto_numeric_metrics)
        return dstd::nullopt;
    const dstd::Vector<dstd::String> current = trace_names(names());
    const dstd::Vector<dstd::String>& tokens =
        p.select.size() == current.size() ? p.select : current;
    dstd::Vector<GroupKey> keys;
    dstd::Vector<detail::ComputedColumn> computed;
    dstd::Vector<dstd::String> raw_reads;
    dstd::Vector<dstd::String> expected;
    for (const dataframe::NamedExpr& k : spec.keys) {
        dstd::Optional<dstd::String> c = column_of(k.expr, current);
        if (c && dstd::StringView(*c) == dstd::StringView(k.name))
            if (dstd::Optional<GroupKey> key = fixed_group_key(*c)) {
                keys.push_back(std::move(*key));
                raw_reads.push_back(*c);
                expected.emplace_back(k.name);
                continue;
            }
        dstd::Optional<detail::ComputedColumn> col =
            computed_from(dstd::String(k.name), k.expr, tokens);
        if (!col) return dstd::nullopt;
        keys.push_back({GroupKey::Kind::Expr, dstd::String(k.name)});
        computed.push_back(std::move(*col));
        expected.emplace_back(k.name);
    }
    dstd::Vector<AggSpec> aggs;
    for (std::size_t i = 0; i < spec.aggs.size(); ++i) {
        const dataframe::AggregateExpr& a = spec.aggs[i];
        dstd::Optional<AggOp> op = simple_agg_op(a.op);
        if (!op || a.by.valid() || a.out.empty()) return dstd::nullopt;
        dstd::String field;
        if (*op != AggOp::Count) {
            dstd::Optional<dstd::String> c = column_of(a.input, current);
            if (c && is_fixed_numeric_field(*c)) {
                field = *c;
                raw_reads.push_back(*c);
            } else {
                field = "__view_agg_in_" + dstd::to_string(i);
                dstd::Optional<detail::ComputedColumn> col =
                    computed_from(field, a.input, tokens);
                if (!col) return dstd::nullopt;
                computed.push_back(std::move(*col));
            }
        }
        aggs.push_back(AggSpec(*op, std::move(field), dstd::String(a.out)));
        expected.emplace_back(a.out);
    }
    if (!computed.empty()) {
        // The view reads ts/dur unscaled for some aggregates; a computed column
        // must see the values the row stream would.
        if (p.time_scale != 1.0) return dstd::nullopt;
        // A computed column replaces a same-named field for every reader, so
        // it may not shadow one the aggregation reads raw.
        for (const detail::ComputedColumn& c : computed)
            if (std::find(raw_reads.begin(), raw_reads.end(), c.name) !=
                raw_reads.end())
                return dstd::nullopt;
    }
    // A row select here only narrowed the scan to what the aggregation reads;
    // on an aggregation the view would read it as an output projection.
    detail::ScanPlan grouped = detail::scan::select(plan_, {});
    if (!computed.empty()) {
        auto plan = dstd::make_shared<detail::ViewPlan>(*grouped);
        plan->computed = std::move(computed);
        plan->schema.reset();
        grouped = std::move(plan);
    }
    if (!keys.empty())
        grouped = detail::scan::group_by(grouped, std::move(keys));
    grouped = detail::scan::agg(grouped, std::move(aggs));
    auto next = dstd::make_shared<ViewSource>(std::move(grouped), emit_dyn_);
    if (!same_names(next->names(), expected)) return dstd::nullopt;
    return dataframe::SourceApplication{std::move(next),
                                        dataframe::ApplyStatus::Exact};
}

dstd::Optional<dataframe::SourceApplication> ViewSource::apply_distinct(
    const dstd::Vector<dstd::String>& subset) const {
    const detail::ViewPlan& p = *plan_;
    if (distinct_ || emit_dyn_ || !absorb_aggregation_ ||
        output_ != TraceOutput::Events || !can_stream_rows() ||
        p.select.empty() || p.cancelled || p.materialize ||
        !p.sort_col.empty() || !p.topk_col.empty() || p.limit || p.offset)
        return dstd::nullopt;
    const dstd::Vector<dstd::String> current = names();
    dstd::Vector<std::int64_t> key_idx;
    if (subset.empty()) {
        key_idx.resize(current.size());
        for (std::size_t i = 0; i < current.size(); ++i)
            key_idx[i] = static_cast<std::int64_t>(i);
    } else {
        key_idx.reserve(subset.size());
        for (const dstd::String& name : subset) {
            const auto it = std::find(current.begin(), current.end(), name);
            if (it == current.end()) return dstd::nullopt;
            key_idx.push_back(static_cast<std::int64_t>(it - current.begin()));
        }
    }
    if (!detail::distinct_keyable(p, key_idx)) return dstd::nullopt;
    auto next = dstd::make_shared<ViewSource>(plan_, emit_dyn_);
    next->applied_filters_ = applied_filters_;
    next->absorb_aggregation_ = absorb_aggregation_;
    next->distinct_ = true;
    next->distinct_key_idx_ = std::move(key_idx);
    return dataframe::SourceApplication{std::move(next),
                                        dataframe::ApplyStatus::Exact};
}

bool ViewSource::static_schema() const {
    return distinct_ && output_ == TraceOutput::Events &&
           detail::is_row_query(*plan_) && !plan_->select.empty();
}

dstd::Optional<dstd::String> ViewSource::batch_key() const {
    const detail::ViewPlan& p = *plan_;
    if (distinct_ || emit_dyn_ || !absorb_aggregation_ || p.cancelled ||
        p.materialize || p.auto_numeric_metrics || !p.sort_col.empty() ||
        !p.topk_col.empty() || p.limit || p.offset)
        return dstd::nullopt;
    // A shared scan hands rows out unordered, so a plan that promises scan
    // order reads on its own.
    if (p.ordered && output_ == TraceOutput::Events && detail::is_row_query(p))
        return dstd::nullopt;
    // The session's export branch writes whole events.
    if (output_ == TraceOutput::ExportJson && !p.select.empty())
        return dstd::nullopt;
    // A branch sees the whole shared scan, so only an unfiltered one shares.
    if (output_ == TraceOutput::Branch && (p.query || p.phase != Phase::Any))
        return dstd::nullopt;
    dstd::String key;
    for (const ViewFile& f : p.files) {
        key += f.file_path;
        key += '\0';
        key += f.index_path;
        key += '\0';
        key += dstd::to_string(f.pinned_c_end);
        key += '\0';
        key += dstd::to_string(f.pinned_uc_begin);
        key += '\0';
    }
    if (p.time_range)
        key += dstd::to_string(p.time_range->first) + ',' +
               dstd::to_string(p.time_range->second);
    key += '\0';
    key += dstd::to_string(p.time_scale) + '\0' +
           dstd::to_string(static_cast<int>(p.all_records)) + '\0' +
           p.rollup_root + '\0' + p.views_root + '\0' +
           dstd::to_string(p.memory_budget);
    return dstd::String(key);
}

namespace {}  // namespace

ViewSession ViewSource::base_session(const detail::ViewPlan& p) {
    auto base = dstd::make_shared<detail::ViewPlan>();
    base->files = p.files;
    base->all_records = p.all_records;
    base->time_scale = p.time_scale;
    base->rollup_root = p.rollup_root;
    base->views_root = p.views_root;
    base->memory_budget = p.memory_budget;
    base->time_range = p.time_range;
    base->scan_counter = p.scan_counter;
    return ViewSession(std::move(base));
}

namespace {

// A zero-copy view of `f`, for members that read the same output.
dataframe::DataFrame shared_frame(const dataframe::DataFrame& f) {
    dataframe::DataFrame out;
    out.names = f.names;
    out.columns.reserve(f.columns.size());
    for (const dataframe::Series& c : f.columns)
        out.columns.push_back(c.share());
    return out;
}

bool is_containment(TraceOutput o) {
    return o == TraceOutput::CallTree || o == TraceOutput::Flamegraph ||
           o == TraceOutput::FlamegraphPartial;
}

}  // namespace

dstd::Vector<dstd::Function<dataframe::DataFrame(const ExportStats&)>>
ViewSource::add_branches(ViewSession& session,
                         const dstd::Vector<const ViewSource*>& members,
                         const dstd::Vector<bool>& skip) {
    // Containment members over the same events and fields share one buffered
    // fold, whichever of its outputs each one reads.
    struct Tree {
        detail::ScanPlan plan;
        const ContainmentArgs* args;
        dstd::SharedPtr<dataframe::DataFrame> call_tree, flamegraph;
        dstd::SharedPtr<dstd::String> partial;
    };
    dstd::Vector<Tree> trees;
    dstd::Vector<std::size_t> tree_of(members.size(), 0);
    for (std::size_t i = 0; i < members.size(); ++i) {
        const ViewSource& m = *members[i];
        if (skip[i] || !is_containment(m.output_)) continue;
        auto it = std::find_if(trees.begin(), trees.end(), [&](const Tree& t) {
            return t.plan == m.plan_ && *t.args == m.tree_;
        });
        if (it == trees.end()) {
            trees.push_back(Tree{m.plan_, &m.tree_, nullptr, nullptr, nullptr});
            it = std::prev(trees.end());
        }
        tree_of[i] = static_cast<std::size_t>(it - trees.begin());
        if (m.output_ == TraceOutput::CallTree && !it->call_tree)
            it->call_tree = dstd::make_shared<dataframe::DataFrame>();
        if (m.output_ == TraceOutput::Flamegraph && !it->flamegraph)
            it->flamegraph = dstd::make_shared<dataframe::DataFrame>();
        if (m.output_ == TraceOutput::FlamegraphPartial && !it->partial)
            it->partial = dstd::make_shared<dstd::String>();
    }
    for (const Tree& t : trees)
        session.add_containment_branch(t.plan, t.args->partition, t.args->ts,
                                       t.args->dur, t.args->name, t.args->group,
                                       t.call_tree, t.flamegraph, t.partial);

    dstd::Vector<dstd::Function<dataframe::DataFrame(const ExportStats&)>> out(
        members.size());
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (skip[i]) continue;
        const ViewSource& m = *members[i];
        switch (m.output_) {
            case TraceOutput::Events: {
                Deferred<dataframe::DataFrame> h =
                    detail::is_row_query(*m.plan_)
                        ? session.collect_events(m.plan_)
                        : session.collect(m.plan_);
                out[i] = [h](const ExportStats&) { return std::move(h.get()); };
                break;
            }
            case TraceOutput::CallTree:
                out[i] = [f = trees[tree_of[i]].call_tree](const ExportStats&) {
                    return shared_frame(*f);
                };
                break;
            case TraceOutput::Flamegraph:
                out[i] = [f = trees[tree_of[i]].flamegraph](
                             const ExportStats&) { return shared_frame(*f); };
                break;
            case TraceOutput::FlamegraphPartial:
                out[i] = [p = trees[tree_of[i]].partial](const ExportStats&) {
                    return detail::partial_frame(*p);
                };
                break;
            case TraceOutput::AggregatePartial: {
                Deferred<dstd::String> h = session.aggregate_partial(m.plan_);
                out[i] = [h](const ExportStats&) {
                    return detail::partial_frame(h.get());
                };
                break;
            }
            case TraceOutput::Branch: {
                dstd::Function<void(const ExportStats&)> publish =
                    m.branch_(session);
                out[i] = [publish =
                              std::move(publish)](const ExportStats& stats) {
                    publish(stats);
                    return dataframe::DataFrame{};
                };
                break;
            }
            case TraceOutput::ExportJson: {
                dstd::Optional<Query> q = detail::effective_query(*m.plan_);
                Deferred<ExportStats> h =
                    q ? session.export_json(std::move(*q), *m.sink_)
                      : session.export_json(*m.sink_);
                out[i] = [h, sink = m.sink_](const ExportStats&) {
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
    dstd::Vector<dstd::SharedPtr<const ViewSource>> members) {
    Batch out;
    if (members.empty()) co_return out;
    ViewSession session = base_session(*members.front()->plan_);
    dstd::Vector<const ViewSource*> raw;
    raw.reserve(members.size());
    for (const auto& m : members) raw.push_back(m.get());
    dstd::Vector<dstd::Function<dataframe::DataFrame(const ExportStats&)>>
        resolve =
            add_branches(session, raw, dstd::Vector<bool>(raw.size(), false));
    out.stats = co_await session.execute();
    out.frames.reserve(resolve.size());
    for (auto& r : resolve) out.frames.push_back(r(out.stats));
    co_return out;
}

namespace {

// `f` laid out as the declared `schema`: its columns in schema order, a
// declared column the rows lack as nulls, then any column the schema does not
// declare, so a batch member matches the schema its plan was built against.
dataframe::DataFrame declared(dataframe::DataFrame f,
                              const dataframe::Schema& schema) {
    if (schema.fields.empty()) return f;
    dataframe::DataFrame out;
    const std::int64_t n = f.num_rows();
    dstd::Vector<bool> used(f.names.size(), false);
    for (const dataframe::Field& field : schema.fields) {
        const auto it = std::find(f.names.begin(), f.names.end(), field.name);
        out.names.push_back(field.name);
        if (it != f.names.end()) {
            const auto at = static_cast<std::size_t>(it - f.names.begin());
            used[at] = true;
            out.columns.push_back(std::move(f.columns[at]));
            continue;
        }
        const dataframe::TypeId t = field.type.id;
        const bool scalar = t != dataframe::TypeId::Unknown &&
                            t != dataframe::TypeId::List &&
                            t != dataframe::TypeId::Struct;
        out.columns.push_back(dataframe::Series::nulls(
            scalar ? t : dataframe::TypeId::String, n));
    }
    for (std::size_t c = 0; c < f.names.size(); ++c)
        if (!used[c]) {
            out.names.push_back(std::move(f.names[c]));
            out.columns.push_back(std::move(f.columns[c]));
        }
    return out;
}

// The shared scan every open_batch cursor reads from. Holding each channel's
// producer slot until it ends means every channel closes, even when the scan
// fails before its folds exist.
coro::CoroTask<void> run_open_batch(
    dstd::SharedPtr<ViewSession> session,
    dstd::Vector<dstd::Function<dataframe::DataFrame(const ExportStats&)>>
        frames,
    dstd::Vector<dataframe::Schema> schemas,
    dstd::Vector<dstd::SharedPtr<coro::UnboundedChannel<dataframe::Morsel>>>
        channels,
    dstd::Vector<dstd::SharedPtr<coro::AsyncSemaphore>> budgets) {
    dstd::Vector<dstd::UniquePtr<
        coro::UnboundedChannel<dataframe::Morsel>::ProducerGuard>>
        guards;
    guards.reserve(channels.size());
    for (auto& ch : channels)
        guards.push_back(
            dstd::make_unique<
                coro::UnboundedChannel<dataframe::Morsel>::ProducerGuard>(
                ch.get()));
    const ExportStats stats = co_await session->execute();
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (!frames[i]) continue;
        dataframe::DataFrame f = declared(frames[i](stats), schemas[i]);
        dataframe::Morsel m;
        m.rows = f.num_rows();
        m.columns = std::move(f.columns);
        const std::uint64_t bytes = detail::morsel_bytes(m);
        co_await budgets[i]->acquire(bytes);
        co_await channels[i]->send(std::move(m));
    }
}

}  // namespace

dstd::Optional<dstd::Vector<dstd::UniquePtr<dataframe::Cursor>>>
ViewSource::open_batch(
    dstd::Vector<dstd::SharedPtr<const dataframe::Source>> members,
    std::uint64_t memory_budget) const {
    dstd::Vector<const ViewSource*> views;
    views.reserve(members.size());
    for (const auto& m : members) {
        const auto* v = dynamic_cast<const ViewSource*>(m.get());
        if (!v || !v->streams_in_batch()) return dstd::nullopt;
        views.push_back(v);
    }
    // Streaming only pays for members whose rows it bounds; finished frames
    // hand over directly through collect_batch().
    if (std::none_of(views.begin(), views.end(),
                     [](const ViewSource* v) { return v->can_stream_rows(); }))
        return dstd::nullopt;

    auto session =
        dstd::make_shared<ViewSession>(base_session(*views.front()->plan_));
    dstd::Vector<dstd::SharedPtr<coro::UnboundedChannel<dataframe::Morsel>>>
        channels;
    dstd::Vector<dstd::SharedPtr<coro::AsyncSemaphore>> budgets;
    dstd::Vector<dstd::SharedPtr<std::atomic<bool>>> dropped;
    dstd::Vector<bool> streamed(views.size(), false);
    for (std::size_t i = 0; i < views.size(); ++i) {
        channels.push_back(coro::make_unbounded_channel<dataframe::Morsel>());
        budgets.push_back(
            dstd::make_shared<coro::AsyncSemaphore>(memory_budget));
        dropped.push_back(dstd::make_shared<std::atomic<bool>>(false));
        const ViewSource& v = *views[i];
        if (v.can_stream_rows()) {
            detail::add_stream_branch(*session->state_, v.plan_, channels[i],
                                      budgets[i], dropped[i]);
            streamed[i] = true;
        }
    }
    dstd::Vector<dstd::Function<dataframe::DataFrame(const ExportStats&)>>
        frames = add_branches(*session, views, streamed);
    dstd::Vector<dataframe::Schema> schemas;
    for (const ViewSource* v : views) schemas.push_back(v->schema());

    dstd::SharedFuture<dstd::Result<void>> producer =
        spawn_on_current_executor(run_open_batch(
            session, std::move(frames), std::move(schemas), channels, budgets));
    dstd::Vector<dstd::UniquePtr<dataframe::Cursor>> cursors;
    cursors.reserve(views.size());
    for (std::size_t i = 0; i < views.size(); ++i)
        cursors.push_back(dstd::make_unique<StreamViewCursor>(
            channels[i], budgets[i], producer, dropped[i], nullptr,
            dstd::Vector<ViewFile>{}, 1.0, dstd::Vector<dstd::String>{}, false,
            nullptr));
    return cursors;
}

bool ViewSource::streams_in_batch() const {
    return output_ != TraceOutput::Events || !detail::is_row_query(*plan_) ||
           can_stream_rows();
}

coro::CoroTask<dstd::Vector<dataframe::DataFrame>> ViewSource::collect_batch(
    dstd::Vector<dstd::SharedPtr<const dataframe::Source>> members) const {
    dstd::Vector<dstd::SharedPtr<const ViewSource>> views;
    views.reserve(members.size());
    for (auto& m : members)
        views.push_back(std::static_pointer_cast<const ViewSource>(m));
    dstd::Vector<dataframe::Schema> schemas;
    for (const auto& v : views) schemas.push_back(v->schema());
    Batch b = co_await run_batch(std::move(views));
    for (std::size_t i = 0; i < b.frames.size(); ++i)
        b.frames[i] = declared(std::move(b.frames[i]), schemas[i]);
    co_return std::move(b.frames);
}

bool ViewSource::can_stream_rows() const {
    // A distinct row scan buffers (it merges per-worker sets and orders by
    // first occurrence), so it never streams morsels.
    if (distinct_) return false;
    if (output_ != TraceOutput::Events || !detail::is_row_query(*plan_))
        return false;
    const detail::ViewPlan& p = *plan_;
    // The index lists the args of data events only, so dftracer metadata rows
    // take their columns from the rows themselves.
    if (!detail::plan_by_path(p) && p.select.empty() &&
        (p.all_records || p.phase == Phase::Metadata))
        return false;
    // detail::scan::collect() always strips sort/topk/offset/limit before
    // building a ViewSource; these checks stay as a defensive guard for any
    // other caller.
    return p.sort_col.empty() && p.topk_col.empty() && p.offset == 0 &&
           p.limit == 0;
}

// Matches build_row_frame's own empty-select order (fixed top-level fields,
// fhash/hhash if present, then sorted args), from index metadata rather than
// a scan - best-effort, may omit an arg key not yet indexed.
dstd::Vector<dstd::String> ViewSource::row_schema() const {
    // Path-decoded records: every column is a catalog path, as written.
    if (detail::plan_by_path(*plan_)) return detail::scan::columns(plan_);
    static const char* const TOP_LEVEL[] = {"name", "cat", "pid", "tid",
                                            "ts",   "dur", "ph"};
    dstd::Vector<dstd::String> out(std::begin(TOP_LEVEL), std::end(TOP_LEVEL));

    dstd::Vector<dstd::String> cols = detail::scan::columns(plan_);  // sorted
    const bool has_fhash =
        std::binary_search(cols.begin(), cols.end(), dstd::String("fhash"));
    const bool has_hhash =
        std::binary_search(cols.begin(), cols.end(), dstd::String("hhash"));
    if (has_fhash) out.push_back("fhash");
    if (has_hhash) out.push_back("hhash");

    for (dstd::String& c : cols) {
        if (c == "pid" || c == "tid" || c == "ts" || c == "dur" ||
            c == "name" || c == "cat" || c == "fhash" || c == "hhash")
            continue;
        // A flattened arg column: build_row_frame's empty-select branch
        // always names these "args.<key>", so the schemas must match.
        out.push_back(dstd::String(dftracer::utils::common::ARGS_PREFIX) + c);
    }
    return out;
}

dftracer::utils::dataframe::Schema ViewSource::schema() const {
    std::lock_guard<std::mutex> lock(schema_mu_);
    if (!schema_) schema_ = compute_schema();
    return *schema_;
}

namespace {

dataframe::Schema fixed_schema(
    std::initializer_list<dstd::Pair<const char*, dataframe::TypeId>> cols) {
    dataframe::Schema s;
    for (const auto& [name, type] : cols)
        s.fields.push_back(
            dataframe::Field{name, dataframe::scalar(type), true});
    return s;
}

}  // namespace

dftracer::utils::dataframe::Schema ViewSource::compute_schema() const {
    using T = dataframe::TypeId;
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
            return dataframe::Schema{};
        case TraceOutput::ExportJson:
            return fixed_schema({{"events_matched", T::Int64},
                                 {"events_scanned", T::Int64},
                                 {"lines_invalid", T::Int64},
                                 {"values_unconverted", T::Int64},
                                 {"chunks_scanned", T::Int64},
                                 {"chunks_skipped", T::Int64},
                                 {"chunks_covered", T::Int64},
                                 {"artifacts_committed", T::Bool},
                                 {"truncated", T::Bool},
                                 {"served_from_mv", T::Bool}});
    }
    dataframe::Schema s;
    if (can_stream_rows() || static_schema()) {
        // row_column_type() reports TypeId::Unknown for a flattened arg
        // column, since it is data-dependent and the function has no index
        // access; fill those in from the same harvest columns()/schema() use,
        // so name and type cannot disagree.
        const bool by_path = detail::plan_by_path(*plan_);
        dstd::HashMap<dstd::String, dataframe::DataType> harvested;
        auto resolve = [&](dstd::StringView name) {
            const dataframe::TypeId id = detail::row_column_type(name, by_path);
            if (id != dataframe::TypeId::Unknown) return dataframe::scalar(id);
            if (harvested.empty())
                harvested = detail::scan::column_types(plan_);
            dstd::StringView key = detail::window_inner(name);
            if (!by_path &&
                key.rfind(dftracer::utils::common::ARGS_PREFIX, 0) == 0)
                key.remove_prefix(dftracer::utils::common::ARGS_PREFIX.size());
            auto it = harvested.find(dstd::String(key));
            return it == harvested.end() ? dataframe::scalar(id) : it->second;
        };
        // A non-empty select fixes every streamed morsel's columns to exactly
        // this list (see open_stream()), so the schema must match it, not the
        // broader index-derived row_schema(). Canonicalize each select entry
        // the same way build_row_frame does, so a bare arg name (or one
        // colliding with a top-level field) resolves to the same column name
        // the producer actually emits.
        if (!plan_->select.empty()) {
            s.fields.reserve(plan_->select.size());
            for (const dstd::String& sel : plan_->select) {
                dstd::String col_name =
                    detail::canonical_row_column_name(sel, by_path);
                const dataframe::DataType type = resolve(col_name);
                s.fields.push_back(
                    dataframe::Field{std::move(col_name), type, true});
            }
        } else {
            dstd::Vector<dstd::String> names = row_schema();
            s.fields.reserve(names.size() + plan_->named_columns.size());
            for (const dstd::String& name : names)
                s.fields.push_back(
                    dataframe::Field{dstd::String(name), resolve(name), true});
            for (const dataframe::Field& f : plan_->named_columns) {
                dataframe::Field named = f;
                named.name = detail::canonical_row_column_name(f.name, by_path);
                if (std::find(names.begin(), names.end(),
                              dstd::StringView(named.name)) == names.end())
                    s.fields.push_back(std::move(named));
            }
        }
        return s;
    }
    // An aggregation's columns follow from its plan, so planning and explain()
    // never run it. A data-dependent (numeric-arg) aggregation, a row query
    // the stream cannot serve, or a plan still carrying an output select read
    // them from the result instead.
    if (!detail::is_row_query(*plan_) && plan_->select.empty())
        if (dstd::Optional<dataframe::Schema> planned =
                detail::aggregated_output_schema(*plan_))
            return *planned;
    const dataframe::DataFrame& buf = *buffer();
    s.fields.reserve(buf.columns.size());
    for (std::size_t i = 0; i < buf.columns.size(); ++i)
        s.fields.push_back(
            dataframe::Field{buf.names[i], buf.columns[i].data_type(), true});
    return s;
}

coro::CoroTask<dataframe::DataFrame> ViewSource::run_alone() const {
    const ContainmentArgs& t = tree_;
    switch (output_) {
        case TraceOutput::Events:
            if (distinct_)
                co_return co_await detail::run_collect_rows_distinct(
                    *plan_, distinct_key_idx_);
            break;
        case TraceOutput::CallTree:
            co_return co_await detail::scan::call_tree(plan_, t.partition, t.ts,
                                                       t.dur, t.name);
        case TraceOutput::Flamegraph:
            co_return co_await detail::scan::flamegraph(
                plan_, t.partition, t.ts, t.dur, t.name, t.group);
        // Each awaited result is named first: GCC 12 destroys a temporary
        // built inside co_await twice.
        case TraceOutput::FlamegraphPartial: {
            auto partial = co_await detail::scan::flamegraph_partial(
                plan_, t.partition, t.ts, t.dur, t.name, t.group);
            co_return detail::partial_frame(std::move(partial));
        }
        case TraceOutput::AggregatePartial: {
            auto partial = co_await detail::scan::aggregate_partial(plan_);
            co_return detail::partial_frame(std::move(partial));
        }
        case TraceOutput::ExportJson: {
            const ExportStats stats =
                co_await detail::scan::export_json(plan_, *sink_);
            sink_->flush();
            co_return detail::stats_frame(stats);
        }
        case TraceOutput::Branch: {
            ViewSession session = ViewSession(plan_);
            dstd::Function<void(const ExportStats&)> publish = branch_(session);
            publish(co_await session.execute());
            co_return dataframe::DataFrame{};
        }
    }
    co_return co_await detail::scan::collect_frame(plan_);
}

const dftracer::utils::dataframe::DataFrame* ViewSource::as_frame() const {
    if (can_stream_rows()) return nullptr;
    return buffer().get();
}

dstd::UniquePtr<dftracer::utils::dataframe::Cursor> ViewSource::open_stream(
    const detail::ScanPlan& v, std::uint64_t memory_budget,
    dstd::Vector<dstd::String> fnames) const {
    // The shared budget semaphore is the sole backpressure, acquired before
    // send and released once the cursor hands a morsel off.
    auto channel =
        coro::make_unbounded_channel<dftracer::utils::dataframe::Morsel>();
    auto budget = dstd::make_shared<coro::AsyncSemaphore>(memory_budget);
    auto intern = dstd::make_shared<dftracer::utils::dstd::StringIntern>();
    const double time_scale = v->time_scale;
    // Backing state for Cursor::narrow(): fuse() polls it per unit for as
    // long as this scan runs, so a narrow() call after the scan has already
    // started can still prune units it has not claimed yet.
    auto dyn_prune = dstd::make_shared<detail::DynamicPrune>();

    // The cursor's early-out: fuse polls the plan's cancel predicate per unit
    // and per batch, so the flag is composed into it rather than added beside
    // it. Composed, never overwritten, so a caller's own cancel_when survives.
    auto stop = dstd::make_shared<std::atomic<bool>>(false);
    detail::ScanPlan scan_view =
        detail::scan::cancel_when(v, [stop, prev = v->cancelled] {
            return stop->load(std::memory_order_relaxed) || (prev && (*prev)());
        });

    // Empty select: each batch discovers its own columns from the actual
    // scanned events, so morsels can differ batch to batch; name_ids lets
    // drain_to_frame reconcile them. A non-empty select instead fixes every
    // morsel's columns to that exact list (build_row_frame's select branch
    // always emits each one, null-filled where absent), matching schema().
    // An ordered scan lets workers claim at most two units each past the
    // oldest unit the cursor has not finished.
    auto gate =
        v->ordered
            ? dstd::make_shared<coro::AsyncSemaphore>(
                  2 * std::max<std::size_t>(1, common::available_parallelism()))
            : nullptr;
    auto task =
        [](detail::ScanPlan vv, double ts,
           dstd::SharedPtr<
               coro::UnboundedChannel<dftracer::utils::dataframe::Morsel>>
               ch,
           dstd::SharedPtr<coro::AsyncSemaphore> sem,
           dstd::SharedPtr<dftracer::utils::dstd::StringIntern> iv,
           dstd::SharedPtr<detail::DynamicPrune> dp, bool emit_dyn,
           dstd::SharedPtr<coro::AsyncSemaphore> gt) -> coro::CoroTask<void> {
        detail::StreamRowFold fold(
            ch, sem, iv, vv->select, ts, detail::metadata_rows(*vv), emit_dyn,
            nullptr, nullptr, detail::plan_by_path(*vv), gt != nullptr,
            detail::scan::json_columns(*vv));
        std::array<detail::Fold*, 1> folds{&fold};
        co_await detail::scan::run_folds(vv, folds, *iv, dp.get(), gt.get());
    }(scan_view, time_scale, channel, budget, intern, dyn_prune, emit_dyn_,
                                                     gate);

    dstd::SharedFuture<dstd::Result<void>> producer =
        spawn_on_current_executor(std::move(task));
    return dstd::make_unique<StreamViewCursor>(
        std::move(channel), std::move(budget), std::move(producer),
        std::move(stop), std::move(dyn_prune), v->files, time_scale,
        std::move(fnames), detail::metadata_rows(*v), std::move(gate));
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
        dstd::SharedPtr<const dftracer::utils::dataframe::DataFrame> buf =
            buffer();
        if (!req.projection.empty())
            buf =
                dstd::make_shared<const dftracer::utils::dataframe::DataFrame>(
                    common::unwrap(buf->select(req.projection)));
        r.cursor = dstd::make_unique<ViewCursor>(std::move(buf));
        return r;
    }

    // Streamable row query: translate each simple predicate into the scan
    // plan query (Exact) and push projection into detail::scan::select so the
    // fold harvests only those columns.
    const dstd::Vector<dstd::String> fnames =
        trace_names(req.projection.empty() ? names() : req.projection);
    const double time_scale = plan_->time_scale;
    detail::ScanPlan v = plan_;
    for (std::size_t i = 0; i < req.filters.size(); ++i) {
        dstd::Optional<Pushable> pushed =
            translate_pred(req.filters[i], fnames, time_scale);
        if (!pushed) continue;
        auto built = std::move(pushed->expr).build();
        if (!built.has_value()) continue;
        v = detail::scan::filter(v, std::move(built.value()));
        r.filters[i] = pushed->exact
                           ? dftracer::utils::dataframe::Pushed::Exact
                           : dftracer::utils::dataframe::Pushed::Inexact;
    }
    if (!req.projection.empty()) v = detail::scan::select(v, fnames);
    // The scan's own gate then bounds the units in flight, not the core count.
    if (req.memory_budget != 0 &&
        (v->memory_budget == 0 || req.memory_budget < v->memory_budget))
        v = detail::scan::memory_budget(v, req.memory_budget);

    r.cursor = open_stream(v, req.memory_budget, fnames);
    return r;
}

namespace detail {

dataframe::DataFrame partial_frame(dstd::StringView partial) {
    const std::int32_t offsets[2] = {0,
                                     static_cast<std::int32_t>(partial.size())};
    dataframe::DataFrame f;
    f.names = {"partial"};
    f.columns.push_back(dataframe::Series{dftu_series_new_string(
        static_cast<dftu_dtype>(dataframe::TypeId::Binary), offsets,
        partial.data(), 1, nullptr)});
    return f;
}

namespace {

// The column `name` of a one-row terminal frame. A frame without it, or with
// no row, did not come from that terminal.
dataframe::Series terminal_cell(const dataframe::DataFrame& frame,
                                const char* name) {
    const std::int64_t at = frame.column_index(name);
    if (at < 0 || static_cast<std::size_t>(at) >= frame.columns.size() ||
        frame.columns[static_cast<std::size_t>(at)].length() < 1)
        throw common::DFTUtilsException::cat(
            common::ErrorCode::Internal, "view terminal: the result has no '",
            name, "' row (", frame.columns.size(), " columns, ",
            frame.num_rows(), " rows)");
    return frame.columns[static_cast<std::size_t>(at)].materialize();
}

}  // namespace

dstd::String partial_of(const dataframe::DataFrame& frame) {
    return dstd::String(terminal_cell(frame, "partial").string_at(0));
}

dataframe::DataFrame stats_frame(const ExportStats& stats) {
    dataframe::DataFrame f;
    auto count = [&](const char* name, std::uint64_t v) {
        const auto x = static_cast<std::int64_t>(v);
        f.names.emplace_back(name);
        f.columns.push_back(dataframe::Series::flat_i64(&x, 1));
    };
    auto flag = [&](const char* name, bool v) {
        const std::uint8_t bits = v ? 1 : 0;
        f.names.emplace_back(name);
        f.columns.push_back(
            dataframe::Series::flat(dataframe::TypeId::Bool, &bits, 1));
    };
    count("events_matched", stats.events_matched);
    count("events_scanned", stats.events_scanned);
    count("lines_invalid", stats.lines_invalid);
    count("values_unconverted", stats.values_unconverted);
    count("chunks_scanned", stats.chunks_scanned);
    count("chunks_skipped", stats.chunks_skipped);
    count("chunks_covered", stats.chunks_covered);
    flag("artifacts_committed", stats.artifacts_committed);
    flag("truncated", stats.truncated);
    flag("served_from_mv", stats.served_from_mv);
    return f;
}

ExportStats stats_of(const dataframe::DataFrame& frame) {
    auto count = [&](const char* name) {
        return static_cast<std::uint64_t>(
            terminal_cell(frame, name).data<std::int64_t>()[0]);
    };
    auto flag = [&](const char* name) {
        return terminal_cell(frame, name).any();
    };
    ExportStats s;
    s.events_matched = count("events_matched");
    s.events_scanned = count("events_scanned");
    s.lines_invalid = count("lines_invalid");
    s.values_unconverted = count("values_unconverted");
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
