// SPDX-License-Identifier: MIT
// The adapter from a registered dftu_node_vt to the dataframe NodeImpl, and the
// bridge that hands a plugin node its input cursor as a dftu_cursor_vt.

#include <dftracer/utils/common/c_api_error.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>
#include <dftracer/utils/dataframe/internal/expr_handle.h>
#include <dftracer/utils/dataframe/internal/morsel_frame.h>
#include <dftracer/utils/dataframe/internal/name_cache.h>
#include <dftracer/utils/dataframe/internal/node_registry.h>
#include <dftracer/utils/dataframe/internal/run_counters.h>
#include <dftracer/utils/dataframe/internal/schema_types.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi_context.h>
#include <dftracer/utils/plugins/host/cursor_host.h>
#include <dftracer/utils/plugins/host/explain_abi.h>
#include <dftracer/utils/plugins/host/frame_check.h>
#include <dftracer/utils/plugins/host/node_adapter.h>
#include <dftracer/utils/plugins/host/plugin_abi.h>
#include <dftracer/utils/plugins/host/plugin_task.h>
#include <dftracer/utils/plugins/task_abi.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::plugins::host {
namespace {

// Adapts a dataframe Cursor as a dftu_cursor_vt, the reverse of the provider
// cursor: a registered node's open() pulls its input through this bridge
// exactly as it would pull from any other node's input, so the host and a
// plugin node speak the same cursor protocol in either direction. On success,
// ownership of `cursor_` passes to whichever side ends up calling destroy()
// (see dftu_node_vt::open's Doxygen); on a failed open() the caller still holds
// the owning unique_ptr and this object is destroyed normally.
class HostCursorBridge {
   public:
    HostCursorBridge(dstd::UniquePtr<dataframe::Cursor> cursor,
                     dstd::Vector<dstd::String> schema)
        : cursor_(std::move(cursor)), schema_(std::move(schema)) {}

    static const ::dftu_cursor_vt* vt() {
        static const ::dftu_cursor_vt v{
            sizeof(::dftu_cursor_vt), &HostCursorBridge::next_thunk,
            &HostCursorBridge::destroy_thunk, &HostCursorBridge::narrow_thunk};
        return &v;
    }
    void* self() { return this; }

   private:
    // The frame of one read of cursor_: null at end of stream, an error when
    // the read failed.
    dstd::Result<::dftu_dataframe*> answer(dataframe::MorselResult&& read) {
        if (!read) return read.error();
        if (!*read) return static_cast<::dftu_dataframe*>(nullptr);
        auto df = dataframe::morsel_to_frame(std::move(**read), names_, schema_,
                                             cursor_->out_names());
        if (!df) return df.error();
        return dataframe::dataframe_handle_wrap(std::move(*df));
    }

    // Ends the task running next_task as one frame, end of stream or error.
    coro::CoroTask<void> next_task(plugins::AbiTask* task,
                                   std::int64_t max_rows,
                                   ::dftu_dataframe** out) {
        auto reading = cursor_->next(max_rows);
        dataframe::MorselResult read = co_await std::move(reading);
        auto frame = answer(std::move(read));
        if (!frame) {
            task->fail(common::to_status(frame.error().condition()),
                       frame.error().message());
        } else if (*frame == nullptr) {
            task->status = DFTU_END;
        } else {
            *out = *frame;
        }
    }

    // Answers inline when cursor_->try_next answered without suspending;
    // false leaves everything untouched so the caller falls back to
    // next_task. Mirrors next_task's success/error/end-of-stream shapes
    // exactly, so a plugin node cannot tell which path answered it apart
    // from `pending`.
    bool try_next_sync(::dftu_ctx* ctx, std::int64_t max_rows,
                       ::dftu_dataframe** out, dftu_status* status) {
        dataframe::MorselResult read;
        if (!cursor_->try_next(max_rows, read)) return false;
        auto frame = answer(std::move(read));
        if (!frame) {
            *status =
                dftu_fail(ctx, common::to_status(frame.error().condition()),
                          frame.error().c_str());
        } else if (*frame == nullptr) {
            *status = DFTU_END;
        } else {
            *out = *frame;
            *status = DFTU_OK;
        }
        return true;
    }

    static dftu_status next_thunk(::dftu_ctx* ctx, void* self,
                                  std::int64_t max_rows, ::dftu_dataframe** out,
                                  ::dftu_task** pending) {
        auto* self_ = static_cast<HostCursorBridge*>(self);
        *out = nullptr;
        *pending = nullptr;
        dftu_status status = DFTU_OK;
        if (self_->try_next_sync(ctx, max_rows, out, &status)) return status;
        *pending = plugins::make_abi_task([=](plugins::AbiTask* task) {
            return self_->next_task(task, max_rows, out);
        });
        return DFTU_OK;
    }
    static void destroy_thunk(void* self) {
        dstd::Delete<HostCursorBridge>{}(static_cast<HostCursorBridge*>(self));
    }

    coro::CoroTask<void> narrow_task(dataframe::Expr predicate,
                                     std::int32_t* out) {
        const bool applied = co_await cursor_->narrow(predicate);
        *out = applied ? 1 : 0;
    }
    static dftu_status narrow_thunk(::dftu_ctx*, void* self,
                                    const ::dftu_expr* predicate,
                                    std::int32_t* out_applied,
                                    ::dftu_task** pending) {
        auto* self_ = static_cast<HostCursorBridge*>(self);
        *pending = nullptr;
        if (out_applied) *out_applied = 0;
        if (!predicate || !out_applied) return DFTU_OK;
        *pending = plugins::task_to_abi(self_->narrow_task(
            dataframe::expr_handle_unwrap(predicate), out_applied));
        return DFTU_OK;
    }

    dstd::UniquePtr<dataframe::Cursor> cursor_;
    dstd::Vector<dstd::String> schema_;
    dataframe::NameCache names_;
};

// The vtable and `self` of one node, shared by the registry entry and every
// cursor opened from it. `self` is destroyed once, when the last of them is
// gone, if this handle owns it.
struct NodeHandle {
    // First, so it is released after destroy() has run.
    dstd::SharedPtr<PluginLibrary> library;
    dstd::SharedPtr<PluginMemory> memory;
    ::dftu_node_vt vt;
    void* self;
    dstd::String name;
    bool owned;

    NodeHandle(dstd::SharedPtr<PluginLibrary> l,
               dstd::SharedPtr<PluginMemory> m, const ::dftu_node_vt& v,
               void* s, dstd::StringView n, bool o)
        : library(std::move(l)),
          memory(std::move(m)),
          vt(v),
          self(s),
          name(n),
          owned(o) {}
    NodeHandle(const NodeHandle&) = delete;
    NodeHandle& operator=(const NodeHandle&) = delete;
    ~NodeHandle() {
        if (owned && vt.destroy) vt.destroy(self);
    }
};

// A node's output cursor as a dataframe Cursor: pulls the plugin's cursor like
// any built-in one.
class PluginNodeCursor final : public dataframe::Cursor {
   public:
    PluginNodeCursor(dstd::SharedPtr<NodeHandle> node,
                     dstd::UniquePtr<CursorHost> host, void* out_self,
                     const ::dftu_cursor_vt* out_vt,
                     dstd::SharedPtr<dataframe::RunCounters> counters)
        : counters_(std::move(counters)),
          node_(std::move(node)),
          host_(std::move(host)),
          out_self_(out_self),
          out_vt_(),
          // Once per cursor: building it per frame would cost every scan the
          // check is off for.
          label_(dstd::fmt<"node '{}' next">(node_->name)) {
        common::copy_struct(&out_vt_, sizeof(out_vt_), out_vt,
                            out_vt->struct_size,
                            plugin_struct_min_size(PluginStruct::CursorVt));
    }

    ~PluginNodeCursor() override {
        if (out_vt_.destroy) out_vt_.destroy(out_self_);
        host_.reset();
    }

    PluginNodeCursor(const PluginNodeCursor&) = delete;
    PluginNodeCursor& operator=(const PluginNodeCursor&) = delete;

    coro::CoroTask<dataframe::MorselResult> next(
        std::int64_t max_rows) override {
        host_->poll_reclaim();
        const coro::CancelView cancel = co_await coro::this_coro::cancel_view;
        plugins::AbiContext ctx(cancel, node_->name, counters_.get());
        AbiHandle<::dftu_dataframe, ::dftu_dataframe_free> frame;
        ::dftu_task* pending = nullptr;
        dftu_status status = out_vt_.next(ctx.get(), out_self_, max_rows,
                                          frame.slot(), &pending);
        if (status < 0) {
            if (pending) plugins::free_abi_task(pending);
            co_return plugin_error(ctx, status,
                                   dstd::fmt<"lazy op '{}' next">(node_->name));
        }
        if (pending != nullptr) {
            auto awaiting = await_plugin_task(pending);
            auto awaited = co_await std::move(awaiting);
            if (!awaited) {
                co_return dstd::Error::from_format<"lazy op '{}' next: {}">(
                    awaited.error().condition(), node_->name,
                    awaited.error().message());
            }
            status = *awaited;
        }
        if (status == DFTU_END) co_return dstd::nullopt;
        if (!frame.get())
            co_return common::make_error<
                "lazy op '{}': next returned no frame and not DFTU_END">(
                common::ErrorCode::Internal, node_->name);

        dataframe::DataFrame df =
            dataframe::dataframe_handle_take(frame.release());
        if (auto checked = check_plugin_frame(df, label_); !checked)
            co_return checked.error();
        dataframe::Morsel m;
        m.rows = df.num_rows();
        m.columns = std::move(df.columns);
        co_return std::move(m);
    }

    coro::CoroTask<bool> narrow(const dataframe::Expr& predicate) override {
        if (!out_vt_.narrow) co_return false;
        std::int32_t applied = 0;
        AbiHandle<::dftu_expr, ::dftu_expr_free> h(
            dataframe::expr_handle_wrap(predicate));
        const coro::CancelView cancel = co_await coro::this_coro::cancel_view;
        plugins::AbiContext ctx(cancel, node_->name, counters_.get());
        ::dftu_task* t = nullptr;
        const dftu_status status =
            out_vt_.narrow(ctx.get(), out_self_, h.get(), &applied, &t);
        bool awaited_ok = status >= 0;
        if (status < 0) {
            if (t) plugins::free_abi_task(t);
            DFTRACER_UTILS_LOG_WARN(
                "{}", plugin_error(ctx, status, "plugin node narrow").format());
        } else if (t) {
            auto awaiting = await_plugin_task(t);
            auto awaited = co_await std::move(awaiting);
            awaited_ok = awaited.has_value();
            if (!awaited_ok)
                DFTRACER_UTILS_LOG_WARN("plugin node narrow failed: {}",
                                        awaited.error().c_str());
        }
        co_return awaited_ok&& applied != 0;
    }

   private:
    // Last to go, so the run outlives the plugin cursor that adds to it.
    dstd::SharedPtr<dataframe::RunCounters> counters_;
    dstd::SharedPtr<NodeHandle> node_;
    // After the plugin's cursor is destroyed, so what it left is released then.
    dstd::UniquePtr<CursorHost> host_;
    void* out_self_;
    ::dftu_cursor_vt out_vt_;
    dstd::String label_;
};

class VtableNode final : public dataframe::NodeImpl {
   public:
    explicit VtableNode(dstd::SharedPtr<NodeHandle> handle)
        : handle_(std::move(handle)) {}

    // dftu_schema::top mirrors dataframe::Schema::fields, so the call is a
    // plain copy in and out, no per-field translation needed.
    dstd::Result<dataframe::Schema> output_schema(
        const dataframe::Schema& in,
        const dataframe::OpArgs& args) const override {
        ::dftu_schema in_built;
        in_built.top = in.fields;
        ::dftu_schema out_built;
        plugins::AbiContext ctx({}, handle_->name);
        const dftu_status status = handle_->vt.output_schema(
            ctx.get(), handle_->self, &in_built, &args.raw(), &out_built);
        if (status < 0)
            return plugin_error(
                ctx, status,
                dstd::fmt<"lazy op '{}' output_schema">(handle_->name));
        dataframe::Schema out;
        out.fields = std::move(out_built.top);
        return out;
    }

    dstd::Result<dstd::UniquePtr<dataframe::Cursor>> open(
        dstd::UniquePtr<dataframe::Cursor> input,
        dstd::Span<const dstd::String> input_names,
        const dataframe::OpArgs& args, std::uint64_t memory_budget,
        dstd::SharedPtr<dataframe::RunCounters> counters) const override {
        auto bridge = dstd::make_unique<HostCursorBridge>(
            std::move(input),
            dstd::Vector<dstd::String>(input_names.begin(), input_names.end()));
        void* out_self = nullptr;
        const ::dftu_cursor_vt* out_vt = nullptr;
        plugins::AbiContext ctx({}, handle_->name, counters.get());
        auto host = dstd::make_unique<CursorHost>(handle_->memory, "node",
                                                  memory_budget);
        const dftu_status status =
            handle_->vt.open(ctx.get(), handle_->self, host->host(),
                             bridge->self(), HostCursorBridge::vt(),
                             &args.raw(), memory_budget, &out_self, &out_vt);
        if (status < 0)
            return plugin_error(ctx, status,
                                dstd::fmt<"lazy op '{}' open">(handle_->name));
        if (!out_vt ||
            !plugin_struct_size_ok(
                PluginStruct::CursorVt, out_vt->struct_size,
                static_cast<std::uint32_t>(sizeof(::dftu_cursor_vt))) ||
            !out_vt->next)
            return common::make_error<
                "lazy op '{}': open() gave no valid cursor vtable">(
                common::ErrorCode::Internal, handle_->name);
        // The node took ownership of the input cursor on success: it (not us)
        // now owns destroying it, via out_vt->destroy eventually reaching
        // HostCursorBridge::destroy_thunk.
        bridge.release();
        return dstd::UniquePtr<dataframe::Cursor>(
            dstd::make_unique<PluginNodeCursor>(handle_, std::move(host),
                                                out_self, out_vt,
                                                std::move(counters)));
    }

    dstd::Vector<dataframe::ExplainLine> explain(
        const dataframe::OpArgs& args) const override {
        if (!handle_->vt.explain) return {};
        ExplainLines lines;
        plugins::AbiContext ctx({}, handle_->name);
        const dftu_status status = handle_->vt.explain(
            ctx.get(), handle_->self, &args.raw(), lines.out());
        if (status < 0) {
            DFTRACER_UTILS_LOG_WARN(
                "{}", plugin_error(ctx, status, "node explain").format());
            return {};
        }
        return lines.take();
    }

   private:
    dstd::SharedPtr<NodeHandle> handle_;
};

}  // namespace

dstd::Result<void> register_node_vtable(dstd::StringView name,
                                        const ::dftu_node_vt* vt, void* self,
                                        dstd::SharedPtr<PluginLibrary> library,
                                        dstd::SharedPtr<PluginMemory> memory) {
    if (name.empty() || !vt || !vt->output_schema || !vt->open) {
        return dstd::Error::from_format<
            "node register: the name, output_schema and open must be set">(
            dstd::Condition::InvalidArgument);
    }
    if (!plugin_struct_size_ok(
            PluginStruct::NodeVt, vt->struct_size,
            static_cast<std::uint32_t>(sizeof(::dftu_node_vt)))) {
        return dstd::Error::from_format<
            "node '{}': struct_size {} is outside [{}, {}]">(
            dstd::Condition::InvalidArgument, name, vt->struct_size,
            plugin_struct_min_size(PluginStruct::NodeVt),
            static_cast<std::uint32_t>(sizeof(::dftu_node_vt)));
    }
    ::dftu_node_vt vt_copy{};
    common::copy_struct(&vt_copy, sizeof(vt_copy), vt, vt->struct_size,
                        plugin_struct_min_size(PluginStruct::NodeVt));
    // Not owned until registered, so a refused registration leaves `self` to
    // the caller.
    auto handle = dstd::make_shared<NodeHandle>(
        std::move(library), std::move(memory), vt_copy, self, name, false);
    auto registered =
        dataframe::register_node(name, dstd::make_shared<VtableNode>(handle));
    if (registered) handle->owned = true;
    return registered;
}

}  // namespace dftracer::utils::plugins::host
