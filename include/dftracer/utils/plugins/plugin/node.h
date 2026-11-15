// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_NODE_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_NODE_H

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/abi/task.h>
#include <dftracer/utils/plugins/plugin/source.h>
#include <dftracer/utils/plugins/plugin/types.h>

#include <cstdint>
#include <string>
#include <utility>

/* A LazyFrame plan node written as a C++ class. node_vtable<N>() adapts N to
 * dftu_node_vt; register it with PluginBuilder::node<N>(), after which
 * LazyFrame::op(name, args) (dftu_lazyframe_op) stacks it on a plan.
 *
 * N provides:
 *   void output_schema(const SchemaView& in, const OpArgs& args,
 *                      SchemaBuilder& out) const;   // or dstd::Result<void>
 *   dstd::Result<dstd::UniquePtr<C>> open(InputCursor in, const OpArgs& args,
 *                                         std::uint64_t memory_budget,
 *                                         const Host& host) const;
 * where C is a cursor type as in plugin/source.h (next, and optionally
 * narrow). The cursor owns `in` and pulls it with in.next(). `memory_budget` is
 * the plan's budget in bytes, never 0. `host` is the cursor's host, as in
 * plugin/source.h: reserve memory through host.memory() in open(). A failed or
 * empty open() leaves the upstream cursor with the host, as dftu_node_vt::open
 * requires; a failed next() fails the scan with its error's status and
 * message. */

namespace dftracer::utils::plugins {

/// The plan's schema at the node's input, read without a scan.
class SchemaView {
   public:
    explicit SchemaView(const dftu_schema* s) noexcept : s_(s) {}
    std::int32_t size() const { return dftu_schema_field_count(s_); }
    /// Append input column `i`, unchanged, to `out`.
    void copy_to(SchemaBuilder& out, std::int32_t i) const {
        dftu_schema_copy_field(out.raw(), s_, i);
    }
    void copy_all(SchemaBuilder& out) const {
        for (std::int32_t i = 0; i < size(); ++i) copy_to(out, i);
    }
    const dftu_schema* raw() const noexcept { return s_; }

   private:
    const dftu_schema* s_;
};

/// The operands dftu_lazyframe_op was given, zero-filled when it was given
/// none. Read only the slots the node's own contract names.
class OpArgs {
   public:
    explicit OpArgs(const dftu_op_arg* a) noexcept : a_(a) {}
    const dftu_op_val& operator[](int i) const { return a_->args[i]; }
    std::int64_t i64(int i) const { return a_->args[i].i64; }
    double f64(int i) const { return a_->args[i].f64; }
    dstd::StringView str(int i) const {
        const auto& s = a_->args[i].str;
        return s.ptr ? dstd::StringView(s.ptr, static_cast<std::size_t>(s.len))
                     : dstd::StringView();
    }
    const dftu_op_arg* raw() const noexcept { return a_; }

   private:
    const dftu_op_arg* a_;
};

namespace detail {

// Who destroys the upstream cursor: only the node, and only once open()
// succeeded. `dropped` records an InputCursor gone before that point.
struct InputOwnership {
    bool committed = false;
    bool dropped = false;
};

// How an InputCursor runs the task its upstream hands back: through the task
// service of the host the node was registered with. Only the service pointer
// and its `h` are kept, never the host, which dies when the plugin factory
// returns; dftu_svc_task::run uses no host state, so they stay valid.
struct TaskRunner {
    const dftu_svc_task* svc = nullptr;
    void* h = nullptr;

    TaskRunner() = default;
    explicit TaskRunner(const dftu_plugin_host* host) noexcept {
        if (!host || !host->get_service) return;
        svc = static_cast<const dftu_svc_task*>(
            host->get_service(host->h, DFTU_SVC_TASK));
        h = host->h;
    }

    dftu_status run(dftu_ctx* ctx, dftu_task* t) const {
        if (!svc || !svc->run)
            return dftu_fail(ctx, DFTU_E_UNSUPPORTED,
                             "the host offers no task service to run with");
        return svc->run(ctx, h, t);
    }
};

// The ctx InputCursor hands its upstream: it keeps the message the upstream
// sets, which InputCursor then returns in its error, so the node's own
// callback reports it.
struct LocalCtx {
    dftu_ctx base;
    dstd::String message;

    static dftu_status set_error(dftu_ctx* c, dftu_status code, const char* msg,
                                 std::uint32_t len) {
        static_cast<LocalCtx*>(c->h)->message.assign(msg, len);
        return code;
    }
    static dftu_status check(dftu_ctx*) { return DFTU_OK; }
    static const dftu_ctx_vt* vt() {
        static const dftu_ctx_vt VT = {sizeof(dftu_ctx_vt), set_error, check};
        return &VT;
    }

    LocalCtx() : base{sizeof(dftu_ctx), 0, vt(), this} {}
    LocalCtx(const LocalCtx&) = delete;
    LocalCtx& operator=(const LocalCtx&) = delete;
};

}  // namespace detail

/// The node's upstream cursor. Pulling it may run a task to completion through
/// the host's task service, since a node's cursor has no host to await with.
class InputCursor {
   public:
    InputCursor(void* self, const dftu_cursor_vt* vt,
                dstd::SharedPtr<detail::InputOwnership> own,
                detail::TaskRunner runner) noexcept
        : self_(self), vt_(vt), own_(std::move(own)), runner_(runner) {}
    InputCursor(InputCursor&& o) noexcept
        : self_(o.self_),
          vt_(std::exchange(o.vt_, nullptr)),
          own_(std::move(o.own_)),
          runner_(o.runner_) {}
    InputCursor& operator=(InputCursor&&) = delete;
    InputCursor(const InputCursor&) = delete;
    InputCursor& operator=(const InputCursor&) = delete;
    ~InputCursor() {
        if (!vt_) return;
        if (own_->committed) {
            if (vt_->destroy) vt_->destroy(self_);
        } else {
            own_->dropped = true;
        }
    }

    /// The next upstream frame, or dstd::nullopt at the end of the stream. An
    /// error with the upstream's status and message when it fails.
    dstd::Result<dstd::Optional<OwnedDataFrame>> next(std::int64_t max_rows) {
        detail::LocalCtx ctx;
        dftu_dataframe* out = nullptr;
        dftu_task* pending = nullptr;
        dftu_status st = vt_->next(&ctx.base, self_, max_rows, &out, &pending);
        if (st >= 0 && pending) st = runner_.run(&ctx.base, pending);
        if (st < 0) {
            if (out) dftu_dataframe_free(out);
            return make_error(st, ctx.message.empty()
                                      ? dstd::StringView("upstream failed")
                                      : dstd::StringView(ctx.message));
        }
        if (st == DFTU_END || !out) return dstd::Optional<OwnedDataFrame>();
        return dstd::Optional<OwnedDataFrame>(OwnedDataFrame(out));
    }

    /// Offer `predicate` upstream. Only sound when it is positional against
    /// the upstream's columns too (a node that keeps every column in place).
    bool narrow(ExprView predicate) {
        if (!vt_->narrow) return false;
        detail::LocalCtx ctx;
        std::int32_t applied = 0;
        dftu_task* pending = nullptr;
        dftu_status st =
            vt_->narrow(&ctx.base, self_, predicate.raw(), &applied, &pending);
        if (st >= 0 && pending) st = runner_.run(&ctx.base, pending);
        return st >= 0 && applied != 0;
    }

   private:
    void* self_;
    const dftu_cursor_vt* vt_;
    dstd::SharedPtr<detail::InputOwnership> own_;
    detail::TaskRunner runner_;
};

namespace detail {

template <class N>
struct NodeBox {
    dstd::UniquePtr<N> node;
    TaskRunner runner;
};

template <class N>
struct NodeVt {
    static dftu_status output_schema(dftu_ctx* ctx, void* self,
                                     const dftu_schema* in,
                                     const dftu_op_arg* args,
                                     dftu_schema* out) noexcept {
        return to_status(ctx, [&]() {
            SchemaBuilder b(out);
            return static_cast<NodeBox<N>*>(self)->node->output_schema(
                SchemaView(in), OpArgs(args), b);
        });
    }

    static dftu_status open(dftu_ctx* ctx, void* self,
                            const dftu_plugin_host* host, void* in_self,
                            const dftu_cursor_vt* in_vt,
                            const dftu_op_arg* args,
                            std::uint64_t memory_budget, void** out_cursor_self,
                            const dftu_cursor_vt** out_vt) noexcept {
        auto own = dstd::make_shared<InputOwnership>();
        return to_status(ctx, [&]() -> dftu_status {
            auto opened = static_cast<NodeBox<N>*>(self)->node->open(
                InputCursor(in_self, in_vt, own,
                            static_cast<NodeBox<N>*>(self)->runner),
                OpArgs(args), memory_budget, Host(host, ctx));
            if (!opened.has_value()) return report(ctx, opened.error());
            auto cursor = std::move(*opened);
            if (!cursor)
                return dftu_fail(ctx, DFTU_E_INTERNAL,
                                 "open returned no cursor");
            using C = typename decltype(cursor)::element_type;
            auto* box = new CursorBox<C>{std::move(cursor), host};
            // Nothing below fails, so the upstream changes hands only once
            // open() is certain to succeed.
            own->committed = true;
            if (own->dropped && in_vt->destroy) in_vt->destroy(in_self);
            *out_cursor_self = box;
            *out_vt = CursorVt<C>::vt();
            return DFTU_OK;
        });
    }

    static void destroy(void* self) { delete static_cast<NodeBox<N>*>(self); }

    static const dftu_node_vt* vt() {
        static const dftu_node_vt VT = {sizeof(dftu_node_vt), output_schema,
                                        open, destroy};
        return &VT;
    }
};

}  // namespace detail

/// The C vtable for N. A `self` for it is made by make_node_self().
template <class N>
const dftu_node_vt* node_vtable() {
    return detail::NodeVt<N>::vt();
}

/// Wrap `node` as the `self` its vtable expects. `host` is the host the node
/// is registered through; the node's input cursor runs the tasks it is handed
/// with that host's task service. Ownership passes to whoever calls the
/// vtable's destroy() on it.
template <class N>
void* make_node_self(dstd::UniquePtr<N> node, const dftu_plugin_host* host) {
    return new detail::NodeBox<N>{std::move(node), detail::TaskRunner(host)};
}

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_PLUGIN_NODE_H
