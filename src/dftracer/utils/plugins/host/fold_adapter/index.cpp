// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/c_api_error.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash/value_hash.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi/index.h>
#include <dftracer/utils/plugins/abi_context.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/host/plugin_abi.h>
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
        if (!vt_.step) return {};
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

    dstd::Result<void> step_column(dstd::StringView name,
                                   const dataframe::Series& column) override {
        if (!vt_.step_column) return {};
        const dstd::String path(name);
        plugins::AbiContext ctx;
        const dftu_status status = vt_.step_column(
            ctx.get(), builder_, path.c_str(),
            static_cast<std::uint32_t>(path.size()), column.handle());
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
        if (!vt_.file_may_match) return true;
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
        case trace::index::extensions::ExtensionValue::Kind::UInt64:
            // The host never passes one: a plugin leaf with a uint64 above
            // int64 is not compiled.
            out.kind = DFTU_VAL_F64;
            out.as.f64 = static_cast<double>(v.u);
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
        if (!vt ||
            !plugin_struct_size_ok(
                PluginStruct::IndexExtension, vt->struct_size,
                static_cast<std::uint32_t>(sizeof(::dftu_index_extension)))) {
            return common::make_error<
                "index extension '{}' refused: struct_size {} is outside "
                "[{}, {}]">(
                common::ErrorCode::InvalidArgument, name,
                vt ? vt->struct_size : 0,
                plugin_struct_min_size(PluginStruct::IndexExtension),
                static_cast<std::uint32_t>(sizeof(::dftu_index_extension)));
        }
        ::dftu_index_extension vt_copy{};
        common::copy_struct(
            &vt_copy, sizeof(vt_copy), vt, vt->struct_size,
            plugin_struct_min_size(PluginStruct::IndexExtension));
        if (!vt_copy.make_builder || !vt_copy.finish_granule ||
            !vt_copy.destroy_builder || !vt_copy.compile ||
            !vt_copy.may_match || !vt_copy.release) {
            return common::make_error<
                "index extension '{}' refused: a required callback is NULL">(
                common::ErrorCode::InvalidArgument, name);
        }
        dstd::Vector<dstd::String> columns;
        if (vt_copy.columns)
            for (const char* const* c = vt_copy.columns; *c; ++c)
                columns.emplace_back(*c);
        const dstd::StringView plugin = name.substr(0, name.find('.'));
        if (!columns.empty() && !vt_copy.step_column) {
            return common::make_error<
                "index extension '{}' of plugin '{}' refused: it lists "
                "columns but has no step_column">(
                common::ErrorCode::InvalidArgument, name, plugin);
        }
        if (!vt_copy.step && columns.empty()) {
            return common::make_error<
                "index extension '{}' of plugin '{}' refused: it has "
                "neither step nor step_column with columns, so it would "
                "read nothing">(common::ErrorCode::InvalidArgument, name,
                                plugin);
        }
        return trace::index::extensions::ExtensionPtr(
            dstd::make_shared<PluginIndexExtension>(std::move(library),
                                                    dstd::String(name), vt_copy,
                                                    self, std::move(columns)));
    }

    PluginIndexExtension(dstd::SharedPtr<PluginLibrary> library,
                         dstd::String name, const ::dftu_index_extension& vt,
                         void* self, dstd::Vector<dstd::String> columns)
        : library_(std::move(library)),
          name_(std::move(name)),
          vt_(vt),
          self_(self),
          columns_(std::move(columns)) {}

    dstd::StringView name() const override { return name_; }
    std::uint32_t version() const override { return vt_.version; }
    std::uint64_t params_hash() const override { return vt_.params_hash; }
    std::uint64_t max_payload() const override { return vt_.max_payload; }
    bool has_file_check() const override {
        return vt_.file_may_match != nullptr;
    }
    dstd::Span<const dstd::String> columns() const override {
        return {columns_.data(), columns_.size()};
    }
    bool takes_lines() const override { return vt_.step != nullptr; }

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
        l.type = static_cast<std::int32_t>(leaf.type);
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
    dstd::Vector<dstd::String> columns_;
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

std::uint64_t host_hash_value(const ::dftu_value* v) {
    if (!v) return dstd::hash::value_hash_null();
    switch (v->kind) {
        case DFTU_VAL_BOOL:
            return dstd::hash::value_hash_bool(v->as.b != 0);
        case DFTU_VAL_I64:
            return dstd::hash::value_hash_int64(v->as.i64);
        case DFTU_VAL_F64:
            return dstd::hash::value_hash_double(v->as.f64);
        case DFTU_VAL_STR:
            return dstd::hash::value_hash_string(
                dstd::StringView(v->as.str, v->count));
        default:
            return dstd::hash::value_hash_null();
    }
}

template <class In, class Hash>
void hash_rows(const ::dftu_series* col, std::uint64_t* out, std::uint64_t n,
               Hash hash) {
    const In* data = static_cast<const In*>(::dftu_series_data(col));
    const bool nulls = ::dftu_series_null_count(col) > 0;
    for (std::uint64_t i = 0; i < n; ++i)
        out[i] =
            nulls && ::dftu_series_is_null(col, static_cast<std::int64_t>(i))
                ? dstd::hash::value_hash_null()
                : hash(data[i]);
}

dftu_status host_hash_series(const ::dftu_series* col, std::uint64_t* out,
                             std::uint64_t n) {
    using dataframe::TypeId;
    if (!col || !out ||
        static_cast<std::int64_t>(n) != ::dftu_series_length(col))
        return DFTU_E_INVALID;
    const auto type = static_cast<TypeId>(::dftu_series_type(col));
    if (type == TypeId::String || type == TypeId::LargeString) {
        const bool nulls = ::dftu_series_null_count(col) > 0;
        for (std::uint64_t i = 0; i < n; ++i) {
            const auto row = static_cast<std::int64_t>(i);
            if (nulls && ::dftu_series_is_null(col, row)) {
                out[i] = dstd::hash::value_hash_null();
                continue;
            }
            std::int64_t len = 0;
            const char* p = ::dftu_series_string_at(col, row, &len);
            out[i] = dstd::hash::value_hash_string(
                dstd::StringView(p ? p : "", static_cast<std::size_t>(len)));
        }
        return DFTU_OK;
    }
    const bool numeric = type == TypeId::Int8 || type == TypeId::Int16 ||
                         type == TypeId::Int32 || type == TypeId::Int64 ||
                         type == TypeId::Uint8 || type == TypeId::Uint16 ||
                         type == TypeId::Uint32 || type == TypeId::Uint64 ||
                         type == TypeId::Float32 || type == TypeId::Float64;
    if (!numeric) return DFTU_E_UNSUPPORTED;
    dataframe::Series flat;
    if (::dftu_series_encoding(col) != 0) {
        flat = dataframe::Series(::dftu_series_materialize(col));
        if (!flat.valid()) return DFTU_E_NO_MEMORY;
        col = flat.handle();
    }
    switch (type) {
        case TypeId::Int8:
            hash_rows<std::int8_t>(col, out, n, dstd::hash::value_hash_int64);
            break;
        case TypeId::Int16:
            hash_rows<std::int16_t>(col, out, n, dstd::hash::value_hash_int64);
            break;
        case TypeId::Int32:
            hash_rows<std::int32_t>(col, out, n, dstd::hash::value_hash_int64);
            break;
        case TypeId::Int64:
            hash_rows<std::int64_t>(col, out, n, dstd::hash::value_hash_int64);
            break;
        case TypeId::Uint8:
            hash_rows<std::uint8_t>(col, out, n, dstd::hash::value_hash_uint64);
            break;
        case TypeId::Uint16:
            hash_rows<std::uint16_t>(col, out, n,
                                     dstd::hash::value_hash_uint64);
            break;
        case TypeId::Uint32:
            hash_rows<std::uint32_t>(col, out, n,
                                     dstd::hash::value_hash_uint64);
            break;
        case TypeId::Uint64:
            hash_rows<std::uint64_t>(col, out, n,
                                     dstd::hash::value_hash_uint64);
            break;
        case TypeId::Float32:
            hash_rows<float>(col, out, n, [](float v) {
                return dstd::hash::value_hash_double(static_cast<double>(v));
            });
            break;
        default:
            hash_rows<double>(col, out, n, dstd::hash::value_hash_double);
            break;
    }
    return DFTU_OK;
}

const ::dftu_svc_index g_index = {sizeof(::dftu_svc_index), host_index_register,
                                  dstd::hash::VALUE_HASH_VERSION,
                                  host_hash_value, host_hash_series};

}  // namespace

dftu_status detail::register_plugin_index_extension(
    ::dftu_ctx* ctx, dstd::StringView name, const ::dftu_index_extension* vt,
    void* self, dstd::SharedPtr<PluginLibrary> library) {
    if (refuse_plugin_op_name(name)) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"index extension '{}' refused: the bare and 'dftu.' "
                      "namespaces are host-reserved, so an extension name "
                      "must be '<plugin>.<name>'">(name));
    }
    auto ext = PluginIndexExtension::make(name, vt, self, std::move(library));
    if (!ext) return host_fail(ctx, ext.error());
    auto registered =
        trace::index::extensions::register_extension(std::move(*ext));
    if (!registered) return host_fail(ctx, registered.error());
    return DFTU_OK;
}

const void* detail::index_ext_vtable() { return &g_index; }

std::uint32_t detail::index_hash_version() {
    return static_cast<std::uint32_t>(dstd::hash::VALUE_HASH_VERSION);
}

std::uint64_t detail::index_hash_value(const ::dftu_value* v) {
    return host_hash_value(v);
}

dftu_status detail::index_hash_series(const ::dftu_series* column,
                                      std::uint64_t* out, std::uint64_t n) {
    return host_hash_series(column, out, n);
}

}  // namespace dftracer::utils::plugins::host
