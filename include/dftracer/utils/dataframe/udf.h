// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_UDF_H
#define DFTRACER_UTILS_DATAFRAME_UDF_H

#include <dftracer/utils/dataframe/c_api_udf.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/udf_adapter.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <concepts>
#include <cstdint>
#include <utility>

namespace dftracer::utils::dataframe {

/// A registered scalar UDF, callable over expressions: `udf(col(0), col(1))`
/// is the @ref Expr that calls it. Move-only.
///
/// A handle from @ref make_udf owns the registration: destroying it removes
/// the name, so no new lookup finds it. A handle from @ref udf only looks the
/// name up and never removes it. Either way an expression, a plan or a copy
/// of the UDF that was taken before keeps the UDF callable, and so keeps the
/// plugin library that holds it loaded.
class Udf {
   public:
    Udf(dstd::String name, dstd::SharedPtr<const void> record,
        bool is_owning) noexcept
        : name_(std::move(name)),
          record_(std::move(record)),
          is_owning_(is_owning) {}
    Udf(Udf&& other) noexcept
        : name_(std::move(other.name_)),
          record_(std::move(other.record_)),
          is_owning_(other.is_owning_) {
        other.name_.clear();
        other.is_owning_ = false;
    }
    Udf& operator=(Udf&& other) noexcept;
    Udf(const Udf&) = delete;
    Udf& operator=(const Udf&) = delete;
    ~Udf();

    dstd::StringView name() const noexcept { return name_; }

    /// Whether destroying this handle removes the name from the registry.
    bool is_owning() const noexcept { return is_owning_; }

    /// The call over `args`. Argument counts and types are checked when the
    /// expression compiles, with the UDF's own message.
    Expr operator()(dstd::Span<const Expr> args) const;

    template <class... A>
        requires(std::same_as<A, Expr> && ...)
    Expr operator()(const A&... args) const {
        dstd::Vector<Expr> list;
        (list.push_back(args), ...);
        return (*this)(list);
    }

   private:
    dstd::String name_;
    dstd::SharedPtr<const void> record_;
    bool is_owning_;
};

/// Registers the UDF record `desc` like dftu_svc_ops::register_udf, for code in
/// this process, and also lists it in the op registry so dftu_op_find sees the
/// name. The result owns the registration. Fails, naming the problem, for a bad
/// descriptor or a name that a built-in function, another UDF or an op already
/// holds.
dstd::Result<Udf> make_udf(const dftu_udf_desc& desc, void* self = nullptr);

/// Registers `T` as a scalar UDF named `T::NAME` for this process, with the
/// same bind, exec, null and cancel rules as a plugin UDF: both build the
/// descriptor through one adapter. No library is loaded. The result owns the
/// registration, so the name leaves the registry when it is destroyed.
template <NamedUdfType T>
dstd::Result<Udf> make_udf() {
    const dftu_udf_desc desc =
        detail::udf_desc_of<T>(T::NAME, detail::udf_options_of<T>());
    return make_udf(desc, nullptr);
}

/// The UDF registered under `name`, from a plugin or @ref make_udf. Fails with
/// NotFound for an unknown name. The handle does not own the registration.
dstd::Result<Udf> udf(dstd::StringView name);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_UDF_H
