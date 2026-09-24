#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_NODE_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_NODE_H

#include <dftracer/utils/core/coro/abi.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/plugin/source.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

/* A LazyFrame plan node written as a C++ class. node_vtable<N>() adapts N to
 * dftu_node_vt; register it with PluginBuilder::node<N>(), after which
 * LazyFrame::op(name, args) (dftu_lazyframe_op) stacks it on a plan.
 *
 * N provides:
 *   void output_schema(const SchemaView& in, const OpArgs& args,
 *                      SchemaBuilder& out) const;
 *   std::unique_ptr<C> open(InputCursor in, const OpArgs& args) const;
 * where C is a cursor type as in plugin/source.h (next, and optionally
 * narrow / resident_bytes / reclaim). The cursor owns `in` and pulls it with
 * in.next(). A throwing or empty open() leaves the upstream cursor with the
 * host, as dftu_node_vt::open requires; a throwing next() surfaces as a scan
 * error. */

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
    std::string_view str(int i) const {
        const auto& s = a_->args[i].str;
        return s.ptr ? std::string_view(s.ptr, static_cast<std::size_t>(s.len))
                     : std::string_view();
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

}  // namespace detail

/// The node's upstream cursor. Pulling it may run a task to completion on the
/// default runtime, since a node's cursor has no host to await with.
class InputCursor {
   public:
    InputCursor(void* self, const dftu_cursor_vt* vt,
                std::shared_ptr<detail::InputOwnership> own) noexcept
        : self_(self), vt_(vt), own_(std::move(own)) {}
    InputCursor(InputCursor&& o) noexcept
        : self_(o.self_),
          vt_(std::exchange(o.vt_, nullptr)),
          own_(std::move(o.own_)) {}
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

    /// The next upstream frame, or std::nullopt at the end of the stream.
    /// Throws std::runtime_error when the upstream fails.
    std::optional<OwnedFrame> next(std::int64_t max_rows) {
        dftu_result_frame out{};
        dftu_task* t = vt_->next(self_, max_rows, &out);
        if (t && dftu_task_run(dftu_default_runtime(), t) != 0)
            throw std::runtime_error("upstream task failed");
        if (!DFTU_RESULT_OK(out))
            throw std::runtime_error(out.u.err.message ? out.u.err.message
                                                       : "upstream failed");
        if (!out.u.value) return std::nullopt;
        return OwnedFrame(out.u.value);
    }

    /// Offer `predicate` upstream. Only sound when it is positional against
    /// the upstream's columns too (a node that keeps every column in place).
    bool narrow(ExprView predicate) {
        if (!vt_->narrow) return false;
        std::int32_t applied = 0;
        dftu_task* t = vt_->narrow(self_, predicate.raw(), &applied);
        if (t && dftu_task_run(dftu_default_runtime(), t) != 0) return false;
        return applied != 0;
    }

   private:
    void* self_;
    const dftu_cursor_vt* vt_;
    std::shared_ptr<detail::InputOwnership> own_;
};

namespace detail {

template <class N>
struct NodeBox {
    std::unique_ptr<N> node;
};

template <class N>
struct NodeVt {
    static void output_schema(void* self, const dftu_schema* in,
                              const dftu_op_arg* args, dftu_schema* out) {
        try {
            SchemaBuilder b(out);
            static_cast<NodeBox<N>*>(self)->node->output_schema(
                SchemaView(in), OpArgs(args), b);
        } catch (...) {
        }
    }

    static void* open(void* self, void* in_self, const dftu_cursor_vt* in_vt,
                      const dftu_op_arg* args, void** out_cursor_self,
                      const dftu_cursor_vt** out_vt) {
        auto own = std::make_shared<InputOwnership>();
        try {
            auto cursor = static_cast<NodeBox<N>*>(self)->node->open(
                InputCursor(in_self, in_vt, own), OpArgs(args));
            if (!cursor) return nullptr;
            using C = typename decltype(cursor)::element_type;
            auto* box = new CursorBox<C>{std::move(cursor), {}};
            // Nothing below throws, so the upstream changes hands only once
            // open() is certain to succeed.
            own->committed = true;
            if (own->dropped && in_vt->destroy) in_vt->destroy(in_self);
            *out_cursor_self = box;
            *out_vt = CursorVt<C>::vt();
            return box;
        } catch (...) {
            return nullptr;
        }
    }

    static void destroy(void* self) { delete static_cast<NodeBox<N>*>(self); }

    static const dftu_node_vt* vt() {
        static const dftu_node_vt v = {output_schema, open, destroy};
        return &v;
    }
};

}  // namespace detail

/// The C vtable for N. A `self` for it is made by make_node_self().
template <class N>
const dftu_node_vt* node_vtable() {
    return detail::NodeVt<N>::vt();
}

/// Wrap `node` as the `self` its vtable expects. Ownership passes to whoever
/// calls the vtable's destroy() on it.
template <class N>
void* make_node_self(std::unique_ptr<N> node) {
    return new detail::NodeBox<N>{std::move(node)};
}

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_PLUGIN_NODE_H
