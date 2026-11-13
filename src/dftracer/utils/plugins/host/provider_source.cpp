// SPDX-License-Identifier: MIT
// The adapter from a registered dftu_source_vt to the dataframe Source and
// Cursor interfaces. A task a cursor callback hands back through `pending` is
// awaited and freed here, the driving side.

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>
#include <dftracer/utils/dataframe/internal/expr_handle.h>
#include <dftracer/utils/dataframe/internal/join_names.h>
#include <dftracer/utils/dataframe/internal/provider_registry.h>
#include <dftracer/utils/dataframe/internal/schema_types.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi_context.h>
#include <dftracer/utils/plugins/host/cursor_host.h>
#include <dftracer/utils/plugins/host/plugin_task.h>
#include <dftracer/utils/plugins/host/provider_source.h>
#include <dftracer/utils/plugins/task_abi.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

namespace dftracer::utils::plugins::host {
namespace {

// The vtable and `self` of one source, shared by every Source and registry
// entry made from it. `self` is destroyed once, when the last of them is gone,
// if this handle owns it.
struct ProviderHandle {
    // First, so it is released after destroy() has run.
    dstd::SharedPtr<PluginLibrary> library;
    dstd::SharedPtr<PluginMemory> memory;
    ::dftu_source_vt vt;
    void* self;
    bool owned;

    ProviderHandle(dstd::SharedPtr<PluginLibrary> l,
                   dstd::SharedPtr<PluginMemory> m, const ::dftu_source_vt& v,
                   void* s, bool o)
        : library(std::move(l)),
          memory(std::move(m)),
          vt(v),
          self(s),
          owned(o) {}
    ProviderHandle(const ProviderHandle&) = delete;
    ProviderHandle& operator=(const ProviderHandle&) = delete;
    ~ProviderHandle() {
        if (owned && vt.destroy) vt.destroy(self);
    }
};

// Any dftu_pushed value the host has not defined is untrusted and treated as
// DFTU_PUSHED_NO (always sound: the engine re-applies) rather than trusted,
// since a source has no way to prove an Exact claim at runtime.
dataframe::Pushed to_pushed(std::int32_t v) {
    switch (v) {
        case DFTU_PUSHED_NO:
            return dataframe::Pushed::No;
        case DFTU_PUSHED_INEXACT:
            return dataframe::Pushed::Inexact;
        case DFTU_PUSHED_EXACT:
            return dataframe::Pushed::Exact;
        default:
            return dataframe::Pushed::No;
    }
}

// Adapts a registered dftu_cursor_vt as a dataframe::Cursor. A task a callback
// hands back through `pending` is awaited and freed here, the driving side.
class ProviderCursor final : public dataframe::Cursor {
   public:
    ProviderCursor(
        const ::dftu_cursor_vt* vt, void* self,
        dstd::UniquePtr<CursorHost> host,
        dstd::Vector<dstd::String> expected_projection,
        dstd::Vector<dataframe::TypeId> expected_types,
        dftracer::utils::dstd::SharedPtr<const dataframe::Source> source)
        : source_(std::move(source)),
          vt_(vt),
          self_(self),
          host_(std::move(host)),
          expected_projection_(std::move(expected_projection)),
          expected_types_(std::move(expected_types)) {}

    ~ProviderCursor() override {
        if (pending_task_) plugins::free_abi_task(pending_task_);
        if (pending_frame_) ::dftu_dataframe_free(pending_frame_);
        if (vt_->destroy) vt_->destroy(self_);
        host_.reset();
    }

    ProviderCursor(const ProviderCursor&) = delete;
    ProviderCursor& operator=(const ProviderCursor&) = delete;

    coro::CoroTask<dataframe::MorselResult> next(
        std::int64_t max_rows) override {
        if (pending_task_ == nullptr) {
            const coro::CancelView cancel =
                co_await coro::this_coro::cancel_view;
            dataframe::MorselResult out;
            if (call_next(cancel, max_rows, out)) co_return std::move(out);
        }
        ::dftu_task* pending = pending_task_;
        pending_task_ = nullptr;
        auto awaiting = await_plugin_task(pending);
        auto awaited = co_await std::move(awaiting);
        ::dftu_dataframe* frame = pending_frame_;
        pending_frame_ = nullptr;
        if (!awaited) {
            if (frame) ::dftu_dataframe_free(frame);
            co_return awaited.error();
        }
        co_return finish(frame, *awaited);
    }

    // Calls the provider's next() exactly once. Without a pending task the
    // frame is already there and is finished and returned here. A pending
    // task means the provider began a genuinely async operation; that call
    // cannot be repeated (it already ran), so the task and its frame slot are
    // stashed in members for next() to await and finish. Callers must fall
    // back to next() immediately on a false return rather than calling
    // try_next again, exactly as the dataframe::Cursor::try_next contract
    // requires.
    bool try_next(std::int64_t max_rows,
                  dataframe::MorselResult& out) override {
        return call_next(coro::CancelView{}, max_rows, out);
    }

    // The three optional slots, each absent when the provider left it NULL.
    coro::CoroTask<bool> narrow(const dataframe::Expr& predicate) override {
        if (!vt_->narrow) co_return false;
        std::int32_t applied = 0;
        AbiHandle<::dftu_expr, ::dftu_expr_free> h(
            dataframe::expr_handle_wrap(predicate));
        const coro::CancelView cancel = co_await coro::this_coro::cancel_view;
        plugins::AbiContext ctx(cancel);
        ::dftu_task* t = nullptr;
        const dftu_status status =
            vt_->narrow(ctx.get(), self_, h.get(), &applied, &t);
        bool awaited_ok = status >= 0;
        if (status < 0) {
            if (t) plugins::free_abi_task(t);
            DFTRACER_UTILS_LOG_WARN(
                "{}", plugin_error(ctx, status, "provider narrow").format());
        } else if (t) {
            auto awaiting = await_plugin_task(t);
            auto awaited = co_await std::move(awaiting);
            awaited_ok = awaited.has_value();
            if (!awaited_ok)
                DFTRACER_UTILS_LOG_WARN("provider narrow failed: {}",
                                        awaited.error().c_str());
        }
        const bool narrowed = awaited_ok && applied != 0;
        co_return narrowed;
    }

   private:
    // true when `out` holds the result; false when the call left a task to
    // await.
    bool call_next(coro::CancelView cancel, std::int64_t max_rows,
                   dataframe::MorselResult& out) {
        host_->poll_reclaim();
        plugins::AbiContext ctx(cancel);
        // The member is the out slot: a pending task writes it after this
        // call returns, so it must outlive the task.
        ::dftu_task* pending = nullptr;
        const dftu_status status =
            vt_->next(ctx.get(), self_, max_rows, &pending_frame_, &pending);
        if (pending != nullptr && status >= 0) {
            pending_task_ = pending;
            return false;
        }
        ::dftu_dataframe* frame = pending_frame_;
        pending_frame_ = nullptr;
        if (status < 0) {
            if (pending) plugins::free_abi_task(pending);
            if (frame) ::dftu_dataframe_free(frame);
            out = plugin_error(ctx, status, "provider next");
            return true;
        }
        out = finish(frame, status);
        return true;
    }

    dataframe::MorselResult finish(::dftu_dataframe* frame,
                                   dftu_status status) {
        if (status == DFTU_END) {
            if (frame) ::dftu_dataframe_free(frame);
            return dataframe::MorselResult(dftracer::utils::dstd::nullopt);
        }
        if (!frame)
            return common::make_error<
                "provider next: it returned no frame and not DFTU_END">(
                common::ErrorCode::Internal);

        dataframe::DataFrame df = dataframe::dataframe_handle_take(frame);
        // Checked on every morsel, not just the first: the check is
        // O(projected columns), negligible next to the morsel's row data, and
        // the contract binds every dftu_dataframe the cursor produces, not
        // only its first.
        if (!expected_projection_.empty() &&
            !std::equal(df.names.begin(), df.names.end(),
                        expected_projection_.begin(),
                        expected_projection_.end()))
            return common::make_error<
                "provider scan: frame columns [{}] do not match the requested "
                "projection [{}]">(common::ErrorCode::Internal,
                                   dataframe::join_names(df.names),
                                   dataframe::join_names(expected_projection_));

        if (expected_types_.size() == df.columns.size())
            for (std::size_t i = 0; i < expected_types_.size(); ++i) {
                const dataframe::TypeId want = expected_types_[i];
                const dataframe::TypeId got = df.columns[i].type();
                if (want != dataframe::TypeId::Unknown && got != want)
                    return common::make_error<
                        "provider scan: column {} is {} but the provider "
                        "declared {}">(common::ErrorCode::InvalidArgument,
                                       df.names[i], dataframe::type_name(got),
                                       dataframe::type_name(want));
            }

        dataframe::Morsel m;
        m.rows = df.num_rows();
        m.columns = std::move(df.columns);
        return dataframe::MorselResult(std::move(m));
    }

    // Released after the cursor itself: a derived source's destroy() runs
    // only once every cursor it opened is gone.
    dftracer::utils::dstd::SharedPtr<const dataframe::Source> source_;
    const ::dftu_cursor_vt* vt_;
    void* self_;
    // After the plugin's cursor is destroyed, so what it left is released then.
    dstd::UniquePtr<CursorHost> host_;
    dstd::Vector<dstd::String> expected_projection_;
    dstd::Vector<dataframe::TypeId> expected_types_;
    ::dftu_dataframe* pending_frame_ = nullptr;
    ::dftu_task* pending_task_ = nullptr;
};

// A value outside dftu_apply_status reads as NO_CHANGE, like to_pushed.
dftracer::utils::dstd::Optional<dataframe::ApplyStatus> to_apply_status(
    std::int32_t v) {
    switch (v) {
        case DFTU_APPLY_EXACT:
            return dataframe::ApplyStatus::Exact;
        case DFTU_APPLY_INEXACT:
            return dataframe::ApplyStatus::Inexact;
        default:
            return dftracer::utils::dstd::nullopt;
    }
}

// Owns the dftu_expr handles lent to one apply() call.
class ExprLoans {
   public:
    ExprLoans() = default;
    ExprLoans(const ExprLoans&) = delete;
    ExprLoans& operator=(const ExprLoans&) = delete;
    ~ExprLoans() {
        for (dftu_expr* h : owned_) dftu_expr_free(h);
    }
    const dftu_expr* lend(const dataframe::Expr& e) {
        if (!e.valid()) return nullptr;
        owned_.push_back(dataframe::expr_handle_wrap(e));
        return owned_.back();
    }

   private:
    dstd::Vector<dftu_expr*> owned_;
};

class ProviderSource final
    : public dataframe::Source,
      public std::enable_shared_from_this<ProviderSource> {
   public:
    explicit ProviderSource(dstd::SharedPtr<ProviderHandle> handle)
        : handle_(std::move(handle)) {}

    // A source a plugin derived through apply(): its handle owns the state,
    // destroyed once, and it holds the sources it was derived from (its parent,
    // and a join's right side) so the plugin's state behind them outlives it.
    ProviderSource(
        dstd::SharedPtr<ProviderHandle> handle,
        dstd::Vector<dftracer::utils::dstd::SharedPtr<const dataframe::Source>>
            inputs)
        : inputs_(std::move(inputs)), handle_(std::move(handle)) {}

    ProviderSource(const ProviderSource&) = delete;
    ProviderSource& operator=(const ProviderSource&) = delete;

    dftracer::utils::dstd::Optional<dataframe::SourceApplication> apply_filter(
        const dataframe::Expr& predicate) const override {
        ExprLoans loans;
        ::dftu_apply_request req{};
        req.kind = DFTU_APPLY_FILTER;
        req.u.filter.predicate = loans.lend(predicate);
        return offer(req, false);
    }

    dftracer::utils::dstd::Optional<dataframe::SourceApplication>
    apply_projection(
        const dstd::Vector<dataframe::NamedExpr>& exprs) const override {
        ExprLoans loans;
        dstd::Vector<::dftu_named_expr> cexprs;
        cexprs.reserve(exprs.size());
        for (const dataframe::NamedExpr& e : exprs)
            cexprs.push_back({e.name.c_str(), loans.lend(e.expr)});
        ::dftu_apply_request req{};
        req.kind = DFTU_APPLY_PROJECTION;
        req.u.projection.exprs = cexprs.data();
        req.u.projection.n_exprs = static_cast<std::int32_t>(cexprs.size());
        return offer(req, true);
    }

    dftracer::utils::dstd::Optional<dataframe::SourceApplication>
    apply_aggregation(const dataframe::AggregateSpec& spec) const override {
        ExprLoans loans;
        dstd::Vector<::dftu_named_expr> keys;
        keys.reserve(spec.keys.size());
        for (const dataframe::NamedExpr& k : spec.keys)
            keys.push_back({k.name.c_str(), loans.lend(k.expr)});
        dstd::Vector<::dftu_apply_agg> aggs;
        aggs.reserve(spec.aggs.size());
        for (const dataframe::AggregateExpr& a : spec.aggs)
            aggs.push_back({to_string(a.op), loans.lend(a.input),
                            loans.lend(a.by), a.param, a.out.c_str()});
        ::dftu_apply_request req{};
        req.kind = DFTU_APPLY_AGGREGATION;
        req.u.aggregation.keys = keys.data();
        req.u.aggregation.n_keys = static_cast<std::int32_t>(keys.size());
        req.u.aggregation.aggs = aggs.data();
        req.u.aggregation.n_aggs = static_cast<std::int32_t>(aggs.size());
        return offer(req, true);
    }

    dftracer::utils::dstd::Optional<dataframe::SourceApplication> apply_sort(
        const dataframe::SortSpec& spec) const override {
        return offer_sort(DFTU_APPLY_SORT, spec, 0);
    }

    dftracer::utils::dstd::Optional<dataframe::SourceApplication> apply_topn(
        const dataframe::SortSpec& spec, std::int64_t k) const override {
        return offer_sort(DFTU_APPLY_TOPN, spec, k);
    }

    dftracer::utils::dstd::Optional<dataframe::SourceApplication> apply_limit(
        std::int64_t offset, std::int64_t n) const override {
        ::dftu_apply_request req{};
        req.kind = DFTU_APPLY_LIMIT;
        req.u.limit.offset = offset;
        req.u.limit.n = n;
        return offer(req, false);
    }

    dftracer::utils::dstd::Optional<dataframe::SourceApplication> apply_tail(
        std::int64_t n) const override {
        ::dftu_apply_request req{};
        req.kind = DFTU_APPLY_TAIL;
        req.u.tail.n = n;
        return offer(req, false);
    }

    // Only a right side from a provider can cross the C ABI; a plugin tells
    // whether it is its own by comparing other_vt.
    dftracer::utils::dstd::Optional<dataframe::SourceApplication> apply_join(
        const dataframe::JoinSpec& spec) const override {
        const auto* other =
            dynamic_cast<const ProviderSource*>(spec.other.get());
        if (!other) return dftracer::utils::dstd::nullopt;
        dstd::Vector<const char*> left_on;
        left_on.reserve(spec.left_on.size());
        for (const dstd::String& k : spec.left_on) left_on.push_back(k.c_str());
        dstd::Vector<const char*> right_on;
        right_on.reserve(spec.right_on.size());
        for (const dstd::String& k : spec.right_on)
            right_on.push_back(k.c_str());
        ::dftu_apply_request req{};
        req.kind = DFTU_APPLY_JOIN;
        req.u.join.other_self = other->handle_->self;
        req.u.join.other_vt = &other->handle_->vt;
        req.u.join.left_on = left_on.data();
        req.u.join.right_on = right_on.data();
        req.u.join.n_on = static_cast<std::int32_t>(left_on.size());
        req.u.join.how = static_cast<std::int32_t>(spec.how);
        req.u.join.suffix = spec.suffix.c_str();
        return offer(req, true, spec.other);
    }

    // dataframe::Source::schema has no error channel, so a failing provider is
    // logged and reads as no columns; scan() reports the same failure as its
    // error.
    dataframe::Schema schema() const override {
        auto read = read_schema();
        if (!read) {
            DFTRACER_UTILS_LOG_ERROR("{}", read.error().format());
            return dataframe::Schema{};
        }
        return std::move(*read);
    }

    dstd::Result<dataframe::ScanResult> scan(
        const dataframe::ScanRequest& req) const override {
        dataframe::ScanResult r;
        r.filters.assign(req.filters.size(), dataframe::Pushed::No);

        dstd::Vector<const char*> projection;
        projection.reserve(req.projection.size());
        for (const dstd::String& name : req.projection)
            projection.push_back(name.c_str());

        dstd::Vector<dftu_expr*> owned_filters;
        owned_filters.reserve(req.filters.size());
        dstd::Vector<const dftu_expr*> filters;
        filters.reserve(req.filters.size());
        for (const dataframe::Expr& f : req.filters) {
            dftu_expr* h = dataframe::expr_handle_wrap(f);
            owned_filters.push_back(h);
            filters.push_back(h);
        }

        ::dftu_scan_request creq{};
        creq.projection = projection.empty() ? nullptr : projection.data();
        creq.n_projection = static_cast<std::int32_t>(projection.size());
        creq.filters = filters.empty() ? nullptr : filters.data();
        creq.n_filters = static_cast<std::int32_t>(filters.size());
        creq.limit = req.limit;
        creq.memory_budget = req.memory_budget;

        // Pre-filled DFTU_PUSHED_NO: a provider that writes nothing, or writes
        // only some entries, degrades to "the engine re-applies", which is
        // always sound.
        dstd::Vector<std::int32_t> out_pushed(filters.size(), DFTU_PUSHED_NO);

        void* cursor_self = nullptr;
        const ::dftu_cursor_vt* vt = nullptr;
        plugins::AbiContext ctx;
        auto host = dstd::make_unique<CursorHost>(handle_->memory, "provider",
                                                  req.memory_budget);
        const dftu_status status =
            handle_->vt.scan(ctx.get(), handle_->self, host->host(), &creq,
                             out_pushed.data(), &cursor_self, &vt);

        for (dftu_expr* h : owned_filters) dftu_expr_free(h);

        if (status < 0) return plugin_error(ctx, status, "provider scan");
        if (!vt || vt->struct_size != sizeof(::dftu_cursor_vt) || !vt->next)
            return common::make_error<
                "provider scan: the provider gave no valid cursor vtable">(
                common::ErrorCode::Internal);

        for (std::size_t i = 0; i < r.filters.size(); ++i)
            r.filters[i] = to_pushed(out_pushed[i]);
        auto read = read_schema();
        if (!read) return read.error();
        const dataframe::Schema& declared = *read;
        dstd::Vector<dataframe::TypeId> expected_types;
        if (req.projection.empty()) {
            for (const dataframe::Field& f : declared.fields)
                expected_types.push_back(f.type.id);
        } else {
            for (const dstd::String& name : req.projection) {
                dataframe::TypeId id = dataframe::TypeId::Unknown;
                for (const dataframe::Field& f : declared.fields)
                    if (f.name == name) id = f.type.id;
                expected_types.push_back(id);
            }
        }
        r.cursor = dstd::make_unique<ProviderCursor>(
            vt, cursor_self, std::move(host), req.projection,
            std::move(expected_types), shared_from_this());
        return r;
    }

   private:
    dstd::Result<dataframe::Schema> read_schema() const {
        dataframe::Schema s;
        const char* const* names = nullptr;
        std::int32_t n = 0;
        plugins::AbiContext ctx;
        const dftu_status status =
            handle_->vt.schema(ctx.get(), handle_->self, &names, &n);
        if (status < 0) return plugin_error(ctx, status, "provider schema");
        if (n <= 0 || !names) return s;

        dstd::Vector<dstd::String> name_strs;
        name_strs.reserve(static_cast<std::size_t>(n));
        for (std::int32_t i = 0; i < n; ++i)
            name_strs.push_back(names[i] ? names[i] : "");

        if (handle_->vt.schema_types) {
            ::dftu_schema built;
            plugins::AbiContext types_ctx;
            const dftu_status typed_status = handle_->vt.schema_types(
                types_ctx.get(), handle_->self, &built);
            if (typed_status < 0)
                return plugin_error(types_ctx, typed_status,
                                    "provider schema_types");
            if (dftracer::utils::dstd::Optional<dataframe::Schema> typed =
                    dataframe::to_schema(built, name_strs))
                return *typed;
        }

        s.fields.reserve(name_strs.size());
        for (const dstd::String& name : name_strs)
            s.fields.push_back(dataframe::Field{
                name, scalar(dataframe::TypeId::Unknown), true});
        return s;
    }

    dftracer::utils::dstd::Optional<dataframe::SourceApplication> offer_sort(
        ::dftu_apply_kind kind, const dataframe::SortSpec& spec,
        std::int64_t k) const {
        dstd::Vector<const char*> by;
        by.reserve(spec.by.size());
        for (const dstd::String& b : spec.by) by.push_back(b.c_str());
        dstd::Vector<std::int32_t> descending;
        descending.reserve(spec.descending.size());
        for (bool d : spec.descending) descending.push_back(d ? 1 : 0);
        ::dftu_apply_sort_args sort{by.data(), descending.data(),
                                    static_cast<std::int32_t>(by.size())};
        ::dftu_apply_request req{};
        req.kind = kind;
        if (kind == DFTU_APPLY_TOPN)
            req.u.topn = {sort, k};
        else
            req.u.sort = sort;
        return offer(req, false);
    }

    dftracer::utils::dstd::Optional<dataframe::SourceApplication> offer(
        const ::dftu_apply_request& req, bool schema_changing,
        dftracer::utils::dstd::SharedPtr<const dataframe::Source> other =
            nullptr) const {
        if (!handle_->vt.apply) return dftracer::utils::dstd::nullopt;
        ::dftu_apply_result out{};
        out.status = DFTU_APPLY_NO_CHANGE;
        plugins::AbiContext ctx;
        const dftu_status applied =
            handle_->vt.apply(ctx.get(), handle_->self, &req, &out);
        if (applied < 0) {
            DFTRACER_UTILS_LOG_ERROR(
                "{}; the engine applies the operation itself",
                plugin_error(ctx, applied, "provider apply").format());
            return dftracer::utils::dstd::nullopt;
        }
        if (out.status == DFTU_APPLY_NO_CHANGE)
            return dftracer::utils::dstd::nullopt;
        dftracer::utils::dstd::Optional<dataframe::ApplyStatus> status =
            to_apply_status(out.status);
        if (!out.vt) return dftracer::utils::dstd::nullopt;
        if (out.vt->struct_size != sizeof(::dftu_source_vt)) {
            DFTRACER_UTILS_LOG_ERROR(
                "provider apply: the derived source's vtable has a wrong "
                "struct_size; the engine applies the operation itself");
            return dftracer::utils::dstd::nullopt;
        }
        dstd::Vector<dftracer::utils::dstd::SharedPtr<const dataframe::Source>>
            inputs{shared_from_this()};
        if (other) inputs.push_back(std::move(other));
        auto derived = dstd::make_shared<ProviderSource>(
            dstd::make_shared<ProviderHandle>(handle_->library, handle_->memory,
                                              *out.vt, out.self, true),
            std::move(inputs));
        // Constructed first so an answer refused below is still destroyed.
        if (!status ||
            (schema_changing && *status != dataframe::ApplyStatus::Exact))
            return dftracer::utils::dstd::nullopt;
        return dataframe::SourceApplication{std::move(derived), *status};
    }

    // Before handle_, so this source's destroy runs first and the sources it
    // was derived from are released after it.
    dstd::Vector<dftracer::utils::dstd::SharedPtr<const dataframe::Source>>
        inputs_;
    dstd::SharedPtr<ProviderHandle> handle_;
};

class VtableProvider final : public dataframe::ProviderFactory {
   public:
    explicit VtableProvider(dstd::SharedPtr<ProviderHandle> handle)
        : handle_(std::move(handle)) {}

    dstd::SharedPtr<dataframe::Source> make_source() override {
        return dstd::make_shared<ProviderSource>(handle_);
    }

   private:
    dstd::SharedPtr<ProviderHandle> handle_;
};

}  // namespace

dstd::SharedPtr<dataframe::Source> make_provider_source(
    const ::dftu_source_vt& vt, void* self,
    dstd::SharedPtr<PluginMemory> memory) {
    return dstd::make_shared<ProviderSource>(dstd::make_shared<ProviderHandle>(
        nullptr, std::move(memory), vt, self, false));
}

dstd::Result<void> register_provider_vtable(
    dstd::StringView name, const ::dftu_source_vt* vt, void* self,
    dstd::SharedPtr<PluginLibrary> library,
    dstd::SharedPtr<PluginMemory> memory) {
    if (name.empty() || !vt) {
        return dstd::Error::from_format<
            "provider register: the name and the vtable must be set">(
            dstd::Condition::InvalidArgument);
    }
    if (vt->struct_size != sizeof(::dftu_source_vt)) {
        return dstd::Error::from_format<
            "provider '{}': struct_size is not sizeof(dftu_source_vt)">(
            dstd::Condition::InvalidArgument, name);
    }
    // Not owned until registered, so a refused registration leaves `self` to
    // the caller.
    auto handle = dstd::make_shared<ProviderHandle>(
        std::move(library), std::move(memory), *vt, self, false);
    auto registered = dataframe::register_provider(
        name, dstd::make_shared<VtableProvider>(handle));
    if (registered) handle->owned = true;
    return registered;
}

}  // namespace dftracer::utils::plugins::host
