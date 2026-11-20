// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_UDF_REGISTRY_H
#define DFTRACER_UTILS_DATAFRAME_UDF_REGISTRY_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/c_api_udf.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::dataframe {

class UdfBinding;

/// A registered scalar UDF: an immutable record that every plan holding a
/// binding of it shares, so the library behind its callbacks stays loaded
/// until the last plan is gone, as for @ref OpDesc.
class UdfRecord {
   public:
    UdfRecord(const dftu_udf_desc& desc, void* self,
              dstd::SharedPtr<const void> keep_alive);
    UdfRecord(const UdfRecord&) = delete;
    UdfRecord& operator=(const UdfRecord&) = delete;

    dstd::StringView name() const noexcept { return name_; }
    std::int32_t min_args() const noexcept { return desc_.min_args; }
    std::int32_t max_args() const noexcept { return desc_.max_args; }
    bool is_null_aware() const noexcept {
        return (desc_.flags & DFTU_FN_NULL_AWARE) != 0;
    }

   private:
    friend class UdfBinding;
    friend dstd::Result<UdfBinding> bind_udf(
        dstd::SharedPtr<const UdfRecord> udf, dstd::Span<const DataType> args,
        dstd::Span<const dftu_scalar* const> consts);

    dftu_udf_desc desc_;
    dstd::String name_;
    void* self_;
    dstd::SharedPtr<const void> keep_alive_;
};

/// A UDF bound to the argument types of one call site. exec of it may run on
/// several batches at once.
class UdfBinding {
   public:
    UdfBinding(UdfBinding&& other) noexcept;
    UdfBinding& operator=(UdfBinding&&) = delete;
    UdfBinding(const UdfBinding&) = delete;
    UdfBinding& operator=(const UdfBinding&) = delete;
    ~UdfBinding();

    const DataType& result_type() const noexcept { return result_; }
    const UdfRecord& udf() const noexcept { return *udf_; }

    /// The result column over `rows` rows of the argument columns `args`
    /// (one per bound argument, each of `rows` rows). Polls the cancel of
    /// @ref UdfCancelScope before the call, and the UDF polls it inside.
    /// Unless the UDF is null aware, a row with a null argument is null in
    /// the result.
    dstd::Result<Series> run(dstd::Span<const Series* const> args,
                             std::int64_t rows) const;

   private:
    friend dstd::Result<UdfBinding> bind_udf(
        dstd::SharedPtr<const UdfRecord> udf, dstd::Span<const DataType> args,
        dstd::Span<const dftu_scalar* const> consts);

    UdfBinding(dstd::SharedPtr<const UdfRecord> udf, void* bound,
               DataType result, std::uint32_t arg_count) noexcept
        : udf_(std::move(udf)),
          bound_(bound),
          result_(std::move(result)),
          arg_count_(arg_count) {}

    dstd::SharedPtr<const UdfRecord> udf_;
    void* bound_;
    DataType result_;
    std::uint32_t arg_count_;
};

/// Calls bind of `udf` for argument types `args` (each a type a UDF takes) and
/// the call's constants `consts` (one per argument; null where the argument is
/// no constant). Fails, naming the UDF, for an argument count outside its
/// range, a type a UDF does not take, or a bind that fails or gives such a
/// type.
dstd::Result<UdfBinding> bind_udf(dstd::SharedPtr<const UdfRecord> udf,
                                  dstd::Span<const DataType> args,
                                  dstd::Span<const dftu_scalar* const> consts);

/// Registers a UDF like dftu_svc_ops::register_udf, which copies the record
/// and the name. `keep_alive` is held by the record, and so by every binding
/// of it, until the last is gone. Fails, naming the problem, for a name that is
/// not a duql function name, one already registered or counts that do not fit.
dstd::Result<void> add_udf(const dftu_udf_desc& desc, void* self,
                           dstd::SharedPtr<const void> keep_alive);

/// The UDF registered under `name`, or null.
dstd::SharedPtr<const UdfRecord> find_udf(dstd::StringView name);

/// The names of the registered UDFs, in registration order.
dstd::Vector<dstd::String> udf_names();

/// Removes `name` from the registry. Plans that hold the UDF keep it.
bool unregister_udf(dstd::StringView name);

/// The expression that calls `udf` over `args`, each a valid expression (fatal
/// otherwise). Counts and types are checked when it compiles.
Expr udf_call_expr(dstd::SharedPtr<const UdfRecord> udf,
                   dstd::Span<const Expr> args);

/// The cancel that @ref UdfCancelScope holds on this thread (empty without a
/// scope).
coro::CancelView udf_cancel() noexcept;

/// While one lives on a thread, a UDF call there reports @p cancel through
/// dftu_check and does not start once it is requested. The scope covers
/// synchronous work only, so it never stays across a suspension.
class UdfCancelScope {
   public:
    explicit UdfCancelScope(coro::CancelView cancel) noexcept;
    UdfCancelScope(const UdfCancelScope&) = delete;
    UdfCancelScope& operator=(const UdfCancelScope&) = delete;
    ~UdfCancelScope();

   private:
    coro::CancelView previous_;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_UDF_REGISTRY_H
