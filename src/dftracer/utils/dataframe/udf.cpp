// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/c_api_error.h>
#include <dftracer/utils/dataframe/internal/validate.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/dataframe/udaf_registry.h>
#include <dftracer/utils/dataframe/udf.h>
#include <dftracer/utils/dataframe/udf_registry.h>
#include <dftracer/utils/dataframe/udf_support.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/lower.h>

#include <cstdint>
#include <mutex>
#include <utility>

namespace dftracer::utils::dataframe {
namespace {

thread_local coro::CancelView t_cancel;

struct Registry {
    std::mutex mutex;
    dstd::Vector<dstd::SharedPtr<const UdfRecord>> udfs;
};

Registry& registry() { return dstd::Singleton<Registry>::get(); }

// The singleton lives to process exit, so an empty list gives back its
// storage, or the exit check sees it as a leak.
void erase_locked(Registry& reg,
                  dstd::Vector<dstd::SharedPtr<const UdfRecord>>::iterator it) {
    reg.udfs.erase(it);
    if (reg.udfs.empty())
        reg.udfs = dstd::Vector<dstd::SharedPtr<const UdfRecord>>();
}

bool is_name_part(dstd::StringView part) {
    if (part.empty() || dstd::is_ascii_digit(part.front())) return false;
    for (const char c : part)
        if (!dstd::is_ascii_alnum(c) && c != '_') return false;
    return true;
}

dstd::Unexpected<dstd::Error> invalid(dstd::StringView what) {
    return dstd::Unexpected(
        dstd::Error(dstd::Condition::InvalidArgument, what));
}

// Null where an argument is null: `out` is picked per row from itself or an
// all-null column by the AND of the arguments' valid masks.
dstd::Result<Series> mask_nulls(const UdfRecord& udf, Series out,
                                dstd::Span<const Series* const> args,
                                std::int64_t rows) {
    Series valid;
    for (const Series* arg : args) {
        if (arg->null_count() == 0) continue;
        Series mask = arg->valid_mask();
        valid = valid.valid() ? valid.logical_and(mask) : std::move(mask);
    }
    if (!valid.valid()) return out;
    Series nulls = Series::nulls(out.type(), rows);
    Series masked = out.where(valid, nulls);
    if (!masked.valid())
        return dstd::Unexpected(dstd::Error::from_format<
                                "udf '{}' result of type {} cannot be nulled">(
            dstd::Condition::Unsupported, udf.name(), type_name(out.type())));
    return masked;
}

dftu_status set_error(dftu_ctx* c, dftu_status code, const char* msg,
                      std::uint32_t len) {
    auto* self = static_cast<CallContext*>(c->h);
    if (msg != nullptr) self->set_message(dstd::StringView(msg, len));
    return code;
}

dftu_status check(dftu_ctx*) {
    return t_cancel.requested() ? DFTU_E_CANCELLED : DFTU_OK;
}

const dftu_ctx_vt CALL_VT = {sizeof(dftu_ctx_vt), &set_error, &check};

}  // namespace

CallContext::CallContext() noexcept
    : ctx_{sizeof(dftu_ctx), 0, &CALL_VT, this} {}

bool is_function_name(dstd::StringView name) {
    std::size_t from = 0;
    for (;;) {
        const std::size_t dot = name.find('.', from);
        const dstd::StringView part = name.substr(
            from, dot == dstd::StringView::npos ? dstd::StringView::npos
                                                : dot - from);
        if (!is_name_part(part)) return false;
        if (dot == dstd::StringView::npos) return true;
        from = dot + 1;
    }
}

bool takes_type(TypeId id) {
    switch (id) {
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
        case TypeId::Float32:
        case TypeId::Float64:
        case TypeId::String:
        case TypeId::Binary:
            return true;
        default:
            return false;
    }
}

dstd::Error call_error(dstd::StringView kind, dstd::StringView name,
                       dstd::StringView step, dftu_status status,
                       const CallContext& ctx) {
    const dstd::Condition condition = common::to_condition(status);
    if (!ctx.message().empty())
        return dstd::Error::from_format<"{} '{}' {}: {}">(condition, kind, name,
                                                          step, ctx.message());
    return dstd::Error::from_format<"{} '{}' {} failed">(condition, kind, name,
                                                         step);
}

UdfRecord::UdfRecord(const dftu_udf_desc& desc, void* self,
                     dstd::SharedPtr<const void> keep_alive)
    : desc_(desc),
      name_(desc.name),
      self_(self),
      keep_alive_(std::move(keep_alive)) {
    desc_.name = name_.c_str();
}

UdfBinding::UdfBinding(UdfBinding&& other) noexcept
    : udf_(std::move(other.udf_)),
      bound_(other.bound_),
      result_(std::move(other.result_)),
      arg_count_(other.arg_count_) {
    other.bound_ = nullptr;
}

UdfBinding::~UdfBinding() {
    if (udf_ && bound_ != nullptr && udf_->desc_.free_bound != nullptr)
        udf_->desc_.free_bound(bound_);
}

dstd::Result<UdfBinding> bind_udf(dstd::SharedPtr<const UdfRecord> udf,
                                  dstd::Span<const DataType> args,
                                  dstd::Span<const dftu_scalar* const> consts) {
    const UdfRecord& u = *udf;
    const auto n = static_cast<std::int64_t>(args.size());
    if (n < u.min_args() || n > u.max_args())
        return dstd::Unexpected(
            dstd::Error::from_format<"'{}' takes {} to {} arguments, got {}">(
                dstd::Condition::InvalidArgument, u.name(), u.min_args(),
                u.max_args(), n));
    dstd::Vector<dftu_type_info> types;
    types.reserve(args.size());
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!takes_type(args[i].id))
            return dstd::Unexpected(
                dstd::Error::from_format<
                    "'{}' argument {} is {}, which a udf does "
                    "not take">(dstd::Condition::InvalidArgument, u.name(),
                                i + 1, type_name(args[i].id)));
        types.push_back(
            {sizeof(dftu_type_info), static_cast<dftu_dtype>(args[i].id)});
    }
    CallContext ctx;
    dftu_type_info out{sizeof(dftu_type_info), DFTU_TYPE_UNKNOWN};
    void* bound = nullptr;
    const dftu_status status = u.desc_.bind(
        ctx.get(), u.self_, types.data(),
        static_cast<std::uint32_t>(types.size()), consts.data(), &out, &bound);
    if (status < 0)
        return dstd::Unexpected(
            call_error("udf", u.name(), "bind", status, ctx));
    const auto result_id = static_cast<TypeId>(out.id);
    if (!takes_type(result_id)) {
        if (bound != nullptr && u.desc_.free_bound != nullptr)
            u.desc_.free_bound(bound);
        return dstd::Unexpected(
            dstd::Error::from_format<
                "udf '{}' bind chose result type {}, which a "
                "udf does not give">(dstd::Condition::InvalidArgument, u.name(),
                                     static_cast<std::int32_t>(out.id)));
    }
    return UdfBinding(std::move(udf), bound, scalar(result_id),
                      static_cast<std::uint32_t>(types.size()));
}

dstd::Result<Series> UdfBinding::run(dstd::Span<const Series* const> args,
                                     std::int64_t rows) const {
    const UdfRecord& u = *udf_;
    if (args.size() != arg_count_)
        return dstd::Unexpected(
            dstd::Error::from_format<"udf '{}' bound for {} arguments got {}">(
                dstd::Condition::Internal, u.name(), arg_count_, args.size()));
    if (t_cancel.requested())
        return dstd::Unexpected(dstd::Error::from_format<"udf '{}' cancelled">(
            dstd::Condition::Cancelled, u.name()));
    dstd::Vector<Series> flat;
    dstd::Vector<const dftu_series*> handles;
    flat.reserve(args.size());
    handles.reserve(args.size());
    for (const Series* arg : args) {
        flat.push_back(arg->materialize());
        handles.push_back(flat.back().handle());
    }
    CallContext ctx;
    dftu_series* produced = nullptr;
    const dftu_status status =
        u.desc_.exec(ctx.get(), bound_, handles.data(),
                     static_cast<std::uint32_t>(handles.size()),
                     static_cast<std::uint64_t>(rows), &produced);
    Series out(produced);
    if (status < 0)
        return dstd::Unexpected(
            call_error("udf", u.name(), "exec", status, ctx));
    if (!out.valid())
        return dstd::Unexpected(
            dstd::Error::from_format<"udf '{}' exec returned no column">(
                dstd::Condition::Internal, u.name()));
    if (out.length() != rows || out.type() != result_.id)
        return dstd::Unexpected(dstd::Error::from_format<
                                "udf '{}' exec returned {} rows of {}, not "
                                "{} rows of {}">(
            dstd::Condition::Internal, u.name(), out.length(),
            type_name(out.type()), rows, type_name(result_.id)));
    if (auto checked = validate_if_checking(out); !checked)
        return dstd::Unexpected(dstd::Error::from_format<
                                "udf '{}' exec returned a broken column: {}">(
            dstd::Condition::Internal, u.name(), checked.error().message()));
    if (u.is_null_aware()) return out;
    return mask_nulls(u, std::move(out), args, rows);
}

dstd::Result<void> add_udf(const dftu_udf_desc& desc, void* self,
                           dstd::SharedPtr<const void> keep_alive) {
    if (desc.name == nullptr || !is_function_name(desc.name))
        return invalid(
            "a udf name is letters, digits and '_', with '.' between parts");
    if (duql::is_builtin_function(desc.name))
        return dstd::Unexpected(dstd::Error::from_format<
                                "udf '{}' refused: the name is a built-in duql "
                                "function">(dstd::Condition::InvalidArgument,
                                            dstd::StringView(desc.name)));
    if (desc.bind == nullptr || desc.exec == nullptr)
        return dstd::Unexpected(
            dstd::Error::from_format<"udf '{}' needs bind and exec">(
                dstd::Condition::InvalidArgument, dstd::StringView(desc.name)));
    if (desc.min_args < 0 || desc.max_args < desc.min_args)
        return dstd::Unexpected(dstd::Error::from_format<
                                "udf '{}' argument counts {}..{} do not fit">(
            dstd::Condition::InvalidArgument, dstd::StringView(desc.name),
            desc.min_args, desc.max_args));
    if ((desc.flags & ~static_cast<std::uint32_t>(DFTU_FN_VOLATILE |
                                                  DFTU_FN_NULL_AWARE)) != 0)
        return dstd::Unexpected(
            dstd::Error::from_format<"udf '{}' has an unknown flag">(
                dstd::Condition::Unsupported, dstd::StringView(desc.name)));
    if (find_udaf(desc.name) != nullptr)
        return dstd::Unexpected(
            dstd::Error::from_format<"udf '{}' refused: the name is a udaf">(
                dstd::Condition::InvalidArgument, dstd::StringView(desc.name)));
    auto record =
        dstd::make_shared<const UdfRecord>(desc, self, std::move(keep_alive));
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        for (const auto& held : reg.udfs)
            if (held->name() == record->name())
                return dstd::Unexpected(
                    dstd::Error::from_format<"udf '{}' is already registered">(
                        dstd::Condition::InvalidArgument, record->name()));
        reg.udfs.push_back(record);
    }
    // The op registry lists the UDF so dftu_op_find sees the name. The op has
    // no function: a UDF runs through duql and expressions, which bind it.
    const dftu_op_desc op{record->name().data(),
                          DFTU_OP_SIG(SERIES, SERIES, NONE, NONE), nullptr};
    if (!register_op(op, dstd::SharedPtr<const void>(record))) {
        dstd::SharedPtr<const UdfRecord> removed;
        Registry& reg = registry();
        {
            std::lock_guard<std::mutex> lock(reg.mutex);
            for (auto it = reg.udfs.begin(); it != reg.udfs.end(); ++it) {
                if (*it == record) {
                    removed = std::move(*it);
                    erase_locked(reg, it);
                    break;
                }
            }
        }
        return dstd::Unexpected(
            dstd::Error::from_format<"udf '{}' clashes with an op of the same "
                                     "name">(dstd::Condition::InvalidArgument,
                                             record->name()));
    }
    return {};
}

dstd::SharedPtr<const UdfRecord> find_udf(dstd::StringView name) {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    for (const auto& held : reg.udfs)
        if (held->name() == name) return held;
    return nullptr;
}

dstd::Vector<dstd::String> udf_names() {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    dstd::Vector<dstd::String> out;
    for (const auto& held : reg.udfs) out.emplace_back(held->name());
    return out;
}

bool unregister_udf(dstd::StringView name) {
    // The last reference may drop the plugin library, so `removed` outlives
    // the lock.
    dstd::SharedPtr<const UdfRecord> removed;
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        for (auto it = reg.udfs.begin(); it != reg.udfs.end(); ++it) {
            if ((*it)->name() == name) {
                removed = std::move(*it);
                erase_locked(reg, it);
                break;
            }
        }
    }
    if (removed == nullptr) return false;
    dftu_op_unregister(removed->name().data());
    return true;
}

Udf& Udf::operator=(Udf&& other) noexcept {
    if (this != &other) {
        if (is_owning_) unregister_udf(name_);
        name_ = std::move(other.name_);
        record_ = std::move(other.record_);
        is_owning_ = other.is_owning_;
        other.name_.clear();
        other.is_owning_ = false;
    }
    return *this;
}

Udf::~Udf() {
    if (is_owning_) unregister_udf(name_);
}

dstd::Result<Udf> make_udf(const dftu_udf_desc& desc, void* self) {
    auto added = add_udf(desc, self, nullptr);
    if (!added) return dstd::Unexpected(std::move(added.error()));
    auto record = find_udf(desc.name);
    return Udf(dstd::String(desc.name),
               dstd::SharedPtr<const void>(std::move(record)), true);
}

Expr Udf::operator()(const dstd::Vector<Expr>& args) const {
    return udf_call_expr(std::static_pointer_cast<const UdfRecord>(
                             dstd::SharedPtr<const void>(record_)),
                         args);
}

dstd::Result<Udf> udf(dstd::StringView name) {
    auto record = find_udf(name);
    if (!record)
        return dstd::Unexpected(dstd::Error::from_format<"no udf named '{}'">(
            dstd::Condition::NotFound, name));
    return Udf(dstd::String(name),
               dstd::SharedPtr<const void>(std::move(record)), false);
}

coro::CancelView udf_cancel() noexcept { return t_cancel; }

UdfCancelScope::UdfCancelScope(coro::CancelView cancel) noexcept
    : previous_(t_cancel) {
    t_cancel = cancel;
}

UdfCancelScope::~UdfCancelScope() { t_cancel = previous_; }

}  // namespace dftracer::utils::dataframe
