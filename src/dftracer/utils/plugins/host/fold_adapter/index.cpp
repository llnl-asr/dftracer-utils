// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi/index.h>
#include <dftracer/utils/plugins/abi_context.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/host/reserved_names.h>
#include <dftracer/utils/trace/index/extensions/extension_registry.h>

#include <cstdint>

namespace dftracer::utils::plugins::host {
namespace {

dstd::Error callback_failure(::dftu_status status, dstd::StringView message) {
    return common::make_error<"status {}: {}">(
               common::ErrorCode::Internal, status,
               message.empty() ? dstd::StringView("no message") : message)
        .error();
}

void append_out(void* self, const void* data, std::uint64_t len) {
    if (!data) return;
    static_cast<trace::index::extensions::ExtensionOut*>(self)->append(
        dstd::StringView(static_cast<const char*>(data), len));
}

class PluginIndexBuilder final
    : public trace::index::extensions::ExtensionBuilder {
   public:
    PluginIndexBuilder(dstd::SharedPtr<PluginLibrary> library,
                       const ::dftu_index_extension& vt, void* builder)
        : library_(std::move(library)), vt_(vt), builder_(builder) {}
    ~PluginIndexBuilder() override { vt_.destroy_builder(builder_); }
    PluginIndexBuilder(const PluginIndexBuilder&) = delete;
    PluginIndexBuilder& operator=(const PluginIndexBuilder&) = delete;

    dstd::Result<void> step(dstd::Span<const dstd::StringView> lines) override {
        lines_.clear();
        lines_.reserve(lines.size());
        for (dstd::StringView l : lines)
            lines_.push_back({l.data(), l.size(), nullptr, nullptr});
        plugins::AbiContext ctx;
        const dftu_status status =
            vt_.step(ctx.get(), builder_, lines_.data(), lines_.size());
        if (status < 0)
            return dstd::Unexpected<dstd::Error>(
                callback_failure(status, ctx.message()));
        return {};
    }

    dstd::Result<void> finish_granule(
        std::uint64_t granule,
        trace::index::extensions::ExtensionOut& out) override {
        const ::dftu_index_out sink{sizeof(::dftu_index_out), &out, append_out};
        plugins::AbiContext ctx;
        const dftu_status status =
            vt_.finish_granule(ctx.get(), builder_, granule, &sink);
        if (status < 0)
            return dstd::Unexpected<dstd::Error>(
                callback_failure(status, ctx.message()));
        return {};
    }

    bool has_finish_file() const override { return vt_.finish_file != nullptr; }

    dstd::Result<void> finish_file(
        trace::index::extensions::ExtensionOut& out) override {
        const ::dftu_index_out sink{sizeof(::dftu_index_out), &out, append_out};
        plugins::AbiContext ctx;
        const dftu_status status = vt_.finish_file(ctx.get(), builder_, &sink);
        if (status < 0)
            return dstd::Unexpected<dstd::Error>(
                callback_failure(status, ctx.message()));
        return {};
    }

   private:
    // First, so it is released after destroy_builder has run.
    dstd::SharedPtr<PluginLibrary> library_;
    const ::dftu_index_extension& vt_;
    void* builder_;
    dstd::Vector<::dftu_bytes> lines_;
};

class PluginIndexQuery final : public trace::index::extensions::ExtensionQuery {
   public:
    PluginIndexQuery(dstd::SharedPtr<PluginLibrary> library,
                     const ::dftu_index_extension& vt, void* compiled)
        : library_(std::move(library)), vt_(vt), compiled_(compiled) {}
    ~PluginIndexQuery() override { vt_.release(compiled_); }
    PluginIndexQuery(const PluginIndexQuery&) = delete;
    PluginIndexQuery& operator=(const PluginIndexQuery&) = delete;

    bool may_match(dstd::StringView payload) const override {
        return vt_.may_match(
                   compiled_,
                   reinterpret_cast<const std::uint8_t*>(payload.data()),
                   payload.size()) != 0;
    }

    bool file_may_match(dstd::StringView payload) const override {
        return vt_.file_may_match(
                   compiled_,
                   reinterpret_cast<const std::uint8_t*>(payload.data()),
                   payload.size()) != 0;
    }

   private:
    // First, so it is released after release() has run.
    dstd::SharedPtr<PluginLibrary> library_;
    const ::dftu_index_extension& vt_;
    void* compiled_;
};

::dftu_value to_c(const trace::index::extensions::ExtensionValue& v) {
    ::dftu_value out{};
    switch (v.kind) {
        case trace::index::extensions::ExtensionValue::Kind::Bool:
            out.kind = DFTU_VAL_BOOL;
            out.as.b = v.b ? 1U : 0U;
            break;
        case trace::index::extensions::ExtensionValue::Kind::Int64:
            out.kind = DFTU_VAL_I64;
            out.as.i64 = v.i;
            break;
        case trace::index::extensions::ExtensionValue::Kind::Double:
            out.kind = DFTU_VAL_F64;
            out.as.f64 = v.f;
            break;
        case trace::index::extensions::ExtensionValue::Kind::String:
            out.kind = DFTU_VAL_STR;
            out.count = static_cast<std::uint32_t>(v.str->size());
            out.as.str = v.str->c_str();
            break;
    }
    return out;
}

::dftu_index_op to_c(trace::index::extensions::ExtensionOp op) {
    switch (op) {
        case trace::index::extensions::ExtensionOp::Eq:
            return DFTU_INDEX_EQ;
        case trace::index::extensions::ExtensionOp::Ne:
            return DFTU_INDEX_NE;
        case trace::index::extensions::ExtensionOp::Lt:
            return DFTU_INDEX_LT;
        case trace::index::extensions::ExtensionOp::Le:
            return DFTU_INDEX_LE;
        case trace::index::extensions::ExtensionOp::Gt:
            return DFTU_INDEX_GT;
        case trace::index::extensions::ExtensionOp::Ge:
            return DFTU_INDEX_GE;
        case trace::index::extensions::ExtensionOp::In:
            return DFTU_INDEX_IN;
        case trace::index::extensions::ExtensionOp::NotIn:
            return DFTU_INDEX_NOT_IN;
        case trace::index::extensions::ExtensionOp::Like:
            return DFTU_INDEX_LIKE;
        case trace::index::extensions::ExtensionOp::Ilike:
            return DFTU_INDEX_ILIKE;
        case trace::index::extensions::ExtensionOp::Regex:
            return DFTU_INDEX_REGEX;
        case trace::index::extensions::ExtensionOp::Iregex:
            return DFTU_INDEX_IREGEX;
        case trace::index::extensions::ExtensionOp::Icontains:
            return DFTU_INDEX_ICONTAINS;
    }
    return DFTU_INDEX_EQ;
}

/// An index extension a plugin registered (plugins/abi/index.h).
class PluginIndexExtension final : public trace::index::extensions::Extension {
   public:
    static dstd::Result<trace::index::extensions::ExtensionPtr> make(
        dstd::StringView name, const ::dftu_index_extension* vt, void* self,
        dstd::SharedPtr<PluginLibrary> library) {
        if (!vt || vt->struct_size != sizeof(::dftu_index_extension)) {
            return common::make_error<
                "index extension '{}' refused: its struct_size is not "
                "sizeof(dftu_index_extension)">(
                common::ErrorCode::InvalidArgument, name);
        }
        if (!vt->make_builder || !vt->step || !vt->finish_granule ||
            !vt->destroy_builder || !vt->compile || !vt->may_match ||
            !vt->release) {
            return common::make_error<
                "index extension '{}' refused: a required callback is NULL">(
                common::ErrorCode::InvalidArgument, name);
        }
        return trace::index::extensions::ExtensionPtr(
            dstd::make_shared<PluginIndexExtension>(
                std::move(library), dstd::String(name), *vt, self));
    }

    PluginIndexExtension(dstd::SharedPtr<PluginLibrary> library,
                         dstd::String name, const ::dftu_index_extension& vt,
                         void* self)
        : library_(std::move(library)),
          name_(std::move(name)),
          vt_(vt),
          self_(self) {}

    dstd::StringView name() const override { return name_; }
    std::uint32_t version() const override { return vt_.version; }
    std::uint64_t params_hash() const override { return vt_.params_hash; }
    std::uint64_t max_payload() const override { return vt_.max_payload; }
    bool has_file_check() const override {
        return vt_.file_may_match != nullptr;
    }

    dstd::Result<dstd::UniquePtr<trace::index::extensions::ExtensionBuilder>>
    make_builder() const override {
        plugins::AbiContext ctx;
        void* b = nullptr;
        const dftu_status status = vt_.make_builder(ctx.get(), self_, &b);
        if (status < 0 || !b) {
            return common::make_error<"{}">(common::ErrorCode::Internal,
                                            ctx.message());
        }
        return dstd::UniquePtr<trace::index::extensions::ExtensionBuilder>(
            dstd::make_unique<PluginIndexBuilder>(library_, vt_, b));
    }

    dstd::Result<dstd::UniquePtr<trace::index::extensions::ExtensionQuery>>
    compile(
        const trace::index::extensions::ExtensionLeaf& leaf) const override {
        const dstd::String path(leaf.path);
        ::dftu_value value = to_c(leaf.value);
        dstd::Vector<::dftu_value> items;
        if (leaf.op == trace::index::extensions::ExtensionOp::In ||
            leaf.op == trace::index::extensions::ExtensionOp::NotIn) {
            items.reserve(leaf.items.size());
            for (const auto& item : leaf.items) items.push_back(to_c(item));
            value = ::dftu_value{};
            value.kind = DFTU_VAL_ARRAY;
            value.count = static_cast<std::uint32_t>(items.size());
            value.as.items = items.data();
        }
        ::dftu_index_leaf l{};
        l.path = path.c_str();
        l.path_len = static_cast<std::uint32_t>(path.size());
        l.any = leaf.any ? 1 : 0;
        l.op = to_c(leaf.op);
        l.value = &value;
        plugins::AbiContext ctx;
        void* compiled = nullptr;
        const dftu_status status = vt_.compile(ctx.get(), self_, &l, &compiled);
        if (status < 0) {
            return dstd::Unexpected<dstd::Error>(
                callback_failure(status, ctx.message()));
        }
        if (!compiled)
            return dstd::UniquePtr<trace::index::extensions::ExtensionQuery>();
        return dstd::UniquePtr<trace::index::extensions::ExtensionQuery>(
            dstd::make_unique<PluginIndexQuery>(library_, vt_, compiled));
    }

   private:
    dstd::SharedPtr<PluginLibrary> library_;
    dstd::String name_;
    ::dftu_index_extension vt_;
    void* self_;
};

// An extension registered mid-scan would build nothing for the files already
// indexed and prune nothing in the running query, so the scan-time host does
// not register extensions.
dftu_status host_index_register(::dftu_ctx* ctx, void*, const char* name,
                                const ::dftu_index_extension*, void*) {
    return host_fail(
        ctx, DFTU_E_UNSUPPORTED,
        dstd::fmt<"index extension '{}' refused: register from the plugin "
                  "factory, which runs before the scan; the scan-time host "
                  "does not register index extensions">(
            dstd::StringView(name ? name : "(null)")));
}

const ::dftu_svc_index g_index = {sizeof(::dftu_svc_index),
                                  host_index_register};

}  // namespace

dftu_status detail::register_plugin_index_extension(
    ::dftu_ctx* ctx, const char* name, const ::dftu_index_extension* vt,
    void* self, dstd::SharedPtr<PluginLibrary> library) {
    if (refuse_plugin_op_name(name)) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"index extension '{}' refused: the bare and 'dftu.' "
                      "namespaces are host-reserved, so an extension name "
                      "must be '<plugin>.<name>'">(
                dstd::StringView(name ? name : "(null)")));
    }
    auto ext = PluginIndexExtension::make(name, vt, self, std::move(library));
    if (!ext) return host_fail(ctx, ext.error());
    auto registered =
        trace::index::extensions::register_extension(std::move(*ext));
    if (!registered) return host_fail(ctx, registered.error());
    return DFTU_OK;
}

const void* detail::index_ext_vtable() { return &g_index; }

}  // namespace dftracer::utils::plugins::host
