// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/udaf.h>
#include <dftracer/utils/dataframe/udaf_registry.h>
#include <dftracer/utils/dataframe/udf_registry.h>
#include <dftracer/utils/dataframe/udf_support.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/lower.h>

#include <cstdint>
#include <mutex>
#include <utility>

namespace dftracer::utils::dataframe {
namespace {

constexpr std::uint32_t KNOWN_FLAGS =
    DFTU_AGG_STATE_POD | DFTU_AGG_PARALLEL_SAFE | DFTU_AGG_HAS_RETRACT |
    DFTU_AGG_NULL_AWARE;
constexpr std::uint32_t MAX_STATE_ALIGN = 64;

struct Registry {
    std::mutex mutex;
    dstd::Vector<dstd::SharedPtr<const UdafRecord>> udafs;
};

Registry& registry() { return dstd::Singleton<Registry>::get(); }

// The singleton lives to process exit, so an empty list gives back its
// storage, or the exit check sees it as a leak.
void erase_locked(
    Registry& reg,
    dstd::Vector<dstd::SharedPtr<const UdafRecord>>::iterator it) {
    reg.udafs.erase(it);
    if (reg.udafs.empty())
        reg.udafs = dstd::Vector<dstd::SharedPtr<const UdafRecord>>();
}

dstd::Unexpected<dstd::Error> refused(dstd::StringView name,
                                      dstd::StringView why) {
    return dstd::Unexpected(dstd::Error::from_format<"udaf '{}' refused: {}">(
        dstd::Condition::InvalidArgument, name, why));
}

// The reason `desc` cannot register, or empty.
dstd::StringView misfit(const dftu_udaf_desc& d) {
    if (d.bind == nullptr || d.init == nullptr || d.update == nullptr ||
        d.combine == nullptr || d.finalize == nullptr)
        return "it needs bind, init, update, combine and finalize";
    if (d.min_args < 0 || d.max_args < d.min_args)
        return "the argument counts do not fit";
    if ((d.flags & ~KNOWN_FLAGS) != 0) return "it has an unknown flag";
    if (d.state_size == 0) return "state_size is 0";
    if (d.state_align == 0 || d.state_align > MAX_STATE_ALIGN ||
        (d.state_align & (d.state_align - 1)) != 0)
        return "state_align is not a power of two up to 64";
    if ((d.flags & DFTU_AGG_STATE_POD) == 0) {
        if (d.destroy == nullptr)
            return "a state that is not DFTU_AGG_STATE_POD needs destroy";
        if (d.serialize == nullptr || d.deserialize == nullptr)
            return "a state that is not DFTU_AGG_STATE_POD needs serialize and "
                   "deserialize";
    }
    if ((d.flags & DFTU_AGG_HAS_RETRACT) != 0 &&
        (d.retract == nullptr || d.value == nullptr))
        return "DFTU_AGG_HAS_RETRACT needs retract and value";
    return {};
}

}  // namespace

UdafRecord::UdafRecord(const dftu_udaf_desc& desc, void* self,
                       dstd::SharedPtr<const void> keep_alive)
    : desc_(desc),
      name_(desc.name),
      self_(self),
      keep_alive_(std::move(keep_alive)) {
    desc_.name = name_.c_str();
}

UdafBinding::UdafBinding(UdafBinding&& other) noexcept
    : udaf_(std::move(other.udaf_)),
      bound_(other.bound_),
      result_(std::move(other.result_)),
      arg_count_(other.arg_count_) {
    other.bound_ = nullptr;
}

UdafBinding::~UdafBinding() {
    if (udaf_ && bound_ != nullptr && udaf_->desc().free_bound != nullptr)
        udaf_->desc().free_bound(bound_);
}

dstd::Result<UdafBinding> bind_udaf(
    dstd::SharedPtr<const UdafRecord> udaf, dstd::Span<const DataType> args,
    dstd::Span<const dftu_scalar* const> consts) {
    const UdafRecord& u = *udaf;
    const auto n = static_cast<std::int64_t>(args.size());
    if (n < u.desc().min_args || n > u.desc().max_args)
        return dstd::Unexpected(
            dstd::Error::from_format<"'{}' takes {} to {} arguments, got {}">(
                dstd::Condition::InvalidArgument, u.name(), u.desc().min_args,
                u.desc().max_args, n));
    dstd::Vector<dftu_type_info> types;
    dstd::Vector<const dftu_scalar*> no_consts;
    types.reserve(args.size());
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!takes_type(args[i].id))
            return dstd::Unexpected(
                dstd::Error::from_format<
                    "'{}' argument {} is {}, which a udaf does "
                    "not take">(dstd::Condition::InvalidArgument, u.name(),
                                i + 1, type_name(args[i].id)));
        types.push_back(
            {sizeof(dftu_type_info), static_cast<dftu_dtype>(args[i].id)});
    }
    if (consts.empty())
        no_consts.resize(args.size(), nullptr);
    else if (consts.size() != args.size())
        return dstd::Unexpected(
            dstd::Error::from_format<"udaf '{}' bound with {} constants for {} "
                                     "arguments">(dstd::Condition::Internal,
                                                  u.name(), consts.size(),
                                                  args.size()));
    CallContext ctx;
    dftu_type_info out{sizeof(dftu_type_info), DFTU_TYPE_UNKNOWN};
    void* bound = nullptr;
    const dftu_status status = u.desc().bind(
        ctx.get(), u.self(), types.data(),
        static_cast<std::uint32_t>(types.size()),
        consts.empty() ? no_consts.data() : consts.data(), &out, &bound);
    if (status < 0)
        return dstd::Unexpected(
            call_error("udaf", u.name(), "bind", status, ctx));
    const auto result_id = static_cast<TypeId>(out.id);
    if (!takes_type(result_id)) {
        if (bound != nullptr && u.desc().free_bound != nullptr)
            u.desc().free_bound(bound);
        return dstd::Unexpected(dstd::Error::from_format<
                                "udaf '{}' bind chose result type {}, which a "
                                "udaf does not give">(
            dstd::Condition::InvalidArgument, u.name(),
            static_cast<std::int32_t>(out.id)));
    }
    return UdafBinding(std::move(udaf), bound, scalar(result_id),
                       static_cast<std::uint32_t>(types.size()));
}

DataType udaf_result_type(dstd::StringView name,
                          dstd::Span<const DataType> args) {
    dstd::SharedPtr<const UdafRecord> record = find_udaf(name);
    if (!record) return scalar(TypeId::Unknown);
    dstd::Result<UdafBinding> bound = bind_udaf(std::move(record), args, {});
    return bound ? bound->result_type() : scalar(TypeId::Unknown);
}

dstd::Result<void> add_udaf(const dftu_udaf_desc& desc, void* self,
                            dstd::SharedPtr<const void> keep_alive) {
    if (desc.name == nullptr || !is_function_name(desc.name))
        return dstd::Unexpected(dstd::Error(
            dstd::Condition::InvalidArgument,
            "a udaf name is letters, digits and '_', with '.' between parts"));
    const dstd::StringView name(desc.name);
    if (duql::is_builtin_function(name))
        return refused(name,
                       "the name is a built-in duql function or "
                       "aggregate");
    if (find_udf(name) != nullptr) return refused(name, "the name is a udf");
    if (dftu_op_find(desc.name) != nullptr)
        return refused(name, "the name is an op");
    if (const dstd::StringView why = misfit(desc); !why.empty())
        return refused(name, why);
    auto record =
        dstd::make_shared<const UdafRecord>(desc, self, std::move(keep_alive));
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    for (const auto& held : reg.udafs)
        if (held->name() == record->name())
            return dstd::Unexpected(
                dstd::Error::from_format<"udaf '{}' is already registered">(
                    dstd::Condition::InvalidArgument, record->name()));
    reg.udafs.push_back(std::move(record));
    return {};
}

dstd::SharedPtr<const UdafRecord> find_udaf(dstd::StringView name) {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    for (const auto& held : reg.udafs)
        if (held->name() == name) return held;
    return nullptr;
}

dstd::Vector<dstd::String> udaf_names() {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    dstd::Vector<dstd::String> out;
    for (const auto& held : reg.udafs) out.emplace_back(held->name());
    return out;
}

bool unregister_udaf(dstd::StringView name) {
    // The last reference may drop the plugin library, so `removed` outlives
    // the lock.
    dstd::SharedPtr<const UdafRecord> removed;
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        for (auto it = reg.udafs.begin(); it != reg.udafs.end(); ++it) {
            if ((*it)->name() == name) {
                removed = std::move(*it);
                erase_locked(reg, it);
                break;
            }
        }
    }
    return removed != nullptr;
}

Udaf& Udaf::operator=(Udaf&& other) noexcept {
    if (this != &other) {
        if (is_owning_) unregister_udaf(name_);
        name_ = std::move(other.name_);
        record_ = std::move(other.record_);
        is_owning_ = other.is_owning_;
        other.name_.clear();
        other.is_owning_ = false;
    }
    return *this;
}

Udaf::~Udaf() {
    if (is_owning_) unregister_udaf(name_);
}

dstd::Result<Udaf> make_udaf(const dftu_udaf_desc& desc, void* self) {
    auto added = add_udaf(desc, self, nullptr);
    if (!added) return dstd::Unexpected(std::move(added.error()));
    auto record = find_udaf(desc.name);
    return Udaf(dstd::String(desc.name),
                dstd::SharedPtr<const void>(std::move(record)), true);
}

dstd::Result<Udaf> udaf(dstd::StringView name) {
    auto record = find_udaf(name);
    if (!record)
        return dstd::Unexpected(dstd::Error::from_format<"no udaf named '{}'">(
            dstd::Condition::NotFound, name));
    return Udaf(dstd::String(name),
                dstd::SharedPtr<const void>(std::move(record)), false);
}

}  // namespace dftracer::utils::dataframe
