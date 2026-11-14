// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_UDAF_REGISTRY_H
#define DFTRACER_UTILS_DATAFRAME_UDAF_REGISTRY_H

#include <dftracer/utils/dataframe/c_api_udaf.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::dataframe {

/// A registered user-defined aggregate: an immutable record that every binding
/// and state table of it shares, so the library behind its callbacks stays
/// loaded until the last of them is gone, as for @ref UdfRecord.
class UdafRecord {
   public:
    UdafRecord(const dftu_udaf_desc& desc, void* self,
               dstd::SharedPtr<const void> keep_alive);
    UdafRecord(const UdafRecord&) = delete;
    UdafRecord& operator=(const UdafRecord&) = delete;

    dstd::StringView name() const noexcept { return name_; }
    const dftu_udaf_desc& desc() const noexcept { return desc_; }
    void* self() const noexcept { return self_; }
    bool is_pod() const noexcept {
        return (desc_.flags & DFTU_AGG_STATE_POD) != 0;
    }
    bool is_null_aware() const noexcept {
        return (desc_.flags & DFTU_AGG_NULL_AWARE) != 0;
    }
    bool is_parallel_safe() const noexcept {
        return (desc_.flags & DFTU_AGG_PARALLEL_SAFE) != 0;
    }

   private:
    dftu_udaf_desc desc_;
    dstd::String name_;
    void* self_;
    dstd::SharedPtr<const void> keep_alive_;
};

/// A UDAF bound to the argument types of one call site: the `bound` of its
/// bind and the result type bind chose. Move-only; free_bound runs when it is
/// destroyed, so it must outlive every state made for it.
class UdafBinding {
   public:
    UdafBinding(UdafBinding&& other) noexcept;
    UdafBinding& operator=(UdafBinding&&) = delete;
    UdafBinding(const UdafBinding&) = delete;
    UdafBinding& operator=(const UdafBinding&) = delete;
    ~UdafBinding();

    const DataType& result_type() const noexcept { return result_; }
    const UdafRecord& udaf() const noexcept { return *udaf_; }
    void* bound() const noexcept { return bound_; }
    std::uint32_t arg_count() const noexcept { return arg_count_; }

   private:
    friend dstd::Result<UdafBinding> bind_udaf(
        dstd::SharedPtr<const UdafRecord> udaf, dstd::Span<const DataType> args,
        dstd::Span<const dftu_scalar* const> consts);

    UdafBinding(dstd::SharedPtr<const UdafRecord> udaf, void* bound,
                DataType result, std::uint32_t arg_count) noexcept
        : udaf_(std::move(udaf)),
          bound_(bound),
          result_(std::move(result)),
          arg_count_(arg_count) {}

    dstd::SharedPtr<const UdafRecord> udaf_;
    void* bound_;
    DataType result_;
    std::uint32_t arg_count_;
};

/// Calls bind of `udaf` for argument types `args` (each a type a UDAF takes)
/// and the call's constants `consts` (one per argument; null where the argument
/// is no constant, or empty for none). Fails, naming the UDAF, for an argument
/// count outside its range, a type a UDAF does not take, or a bind that fails
/// or gives such a type.
dstd::Result<UdafBinding> bind_udaf(
    dstd::SharedPtr<const UdafRecord> udaf, dstd::Span<const DataType> args,
    dstd::Span<const dftu_scalar* const> consts);

/// The result type of the UDAF `name` bound to argument types `args`, or
/// Unknown when no UDAF has that name or its bind refuses the types. For a plan
/// that wants the type of a result column before it runs.
DataType udaf_result_type(dstd::StringView name,
                          dstd::Span<const DataType> args);

/// Registers a UDAF like dftu_svc_ops::register_udaf, which copies the record
/// and the name. `keep_alive` is held by the record, and so by every binding
/// of it, until the last is gone. Fails, naming the problem, for a descriptor
/// that does not fit or a name that a built-in function or aggregate, an op, a
/// UDF or another UDAF holds.
dstd::Result<void> add_udaf(const dftu_udaf_desc& desc, void* self,
                            dstd::SharedPtr<const void> keep_alive);

/// The UDAF registered under `name`, or null.
dstd::SharedPtr<const UdafRecord> find_udaf(dstd::StringView name);

/// The names of the registered UDAFs, in registration order.
dstd::Vector<dstd::String> udaf_names();

/// Removes `name` from the registry. Bindings and state tables that hold the
/// UDAF keep it.
bool unregister_udaf(dstd::StringView name);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_UDAF_REGISTRY_H
