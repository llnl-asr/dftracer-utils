// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_UDF_ADAPTER_H
#define DFTRACER_UTILS_DATAFRAME_UDF_ADAPTER_H

// The classes a scalar UDF is written with, and the one adapter that turns such
// a class into a dftu_udf_desc. A plugin registers the class through
// PluginBuilder::udf and in-process code through make_udf (udf.h); this header
// holds neither, so a plugin includes no more than the C ABI needs.
//
// A UDF reads its arguments as @ref ColumnView and gives its result as a
// @ref Column, so it never writes a dftu_* type, call or constant: the adapter
// translates to the C ABI.

#include <dftracer/utils/dataframe/c_api_udf.h>
#include <dftracer/utils/dataframe/plugin_frame.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace dftracer::utils::dataframe {

/// Bit operators over a flag set whose underlying type is `std::uint32_t`, so a
/// UDF and a UDAF name their flags without naming a DFTU_* constant.
template <class Flag>
    requires std::is_enum_v<Flag> &&
             std::is_same_v<std::underlying_type_t<Flag>, std::uint32_t>
constexpr Flag operator|(Flag a, Flag b) noexcept {
    return static_cast<Flag>(static_cast<std::uint32_t>(a) |
                             static_cast<std::uint32_t>(b));
}

/// @see operator|
template <class Flag>
    requires std::is_enum_v<Flag> &&
             std::is_same_v<std::underlying_type_t<Flag>, std::uint32_t>
constexpr Flag operator&(Flag a, Flag b) noexcept {
    return static_cast<Flag>(static_cast<std::uint32_t>(a) &
                             static_cast<std::uint32_t>(b));
}

/// @see operator|
template <class Flag>
    requires std::is_enum_v<Flag> &&
             std::is_same_v<std::underlying_type_t<Flag>, std::uint32_t>
constexpr Flag& operator|=(Flag& a, Flag b) noexcept {
    a = a | b;
    return a;
}

/// @see operator|
template <class Flag>
    requires std::is_enum_v<Flag> &&
             std::is_same_v<std::underlying_type_t<Flag>, std::uint32_t>
constexpr Flag& operator&=(Flag& a, Flag b) noexcept {
    a = a & b;
    return a;
}

/// Whether any flag is set.
template <class Flag>
    requires std::is_enum_v<Flag> &&
             std::is_same_v<std::underlying_type_t<Flag>, std::uint32_t>
constexpr bool any(Flag set) noexcept {
    return static_cast<std::uint32_t>(set) != 0;
}

/// Whether every flag of `flag` is set in `set`.
template <class Flag>
    requires std::is_enum_v<Flag> &&
             std::is_same_v<std::underlying_type_t<Flag>, std::uint32_t>
constexpr bool has_flag(Flag set, Flag flag) noexcept {
    return (set & flag) == flag;
}

/// What a scalar UDF promises the host. Without a flag the UDF is
/// deterministic and a row with a null argument gives a null result.
enum class UdfFlag : std::uint32_t {
    None = 0,
    /// The result can differ for the same arguments. Advisory: the host never
    /// folds a UDF call to a constant and never stores a lookup side that calls
    /// any UDF in the lookup cache, with or without this flag.
    Volatile = DFTU_FN_VOLATILE,
    /// The UDF reads null arguments itself: the host passes the columns as they
    /// are and keeps the validity of the result the UDF made.
    NullAware = DFTU_FN_NULL_AWARE,
};

/// What bind of a scalar UDF sees: the argument types, the constants the call
/// writes, and the result type to pick.
class UdfBind {
   public:
    UdfBind(dstd::Span<const dftu_type_info> args,
            dstd::Span<const dftu_scalar* const> consts,
            dftu_type_info* out) noexcept
        : args_(args), consts_(consts), out_(out) {}

    /// The number of arguments the call passes.
    std::uint32_t size() const noexcept {
        return static_cast<std::uint32_t>(args_.size());
    }

    /// The type of argument `i`, such as @ref TypeId "TypeId::Int64".
    TypeId type(std::uint32_t i) const noexcept {
        return i < args_.size() ? static_cast<TypeId>(args_[i].id)
                                : TypeId::Unknown;
    }

    /// The value of argument `i` when the call writes a constant there, else
    /// nothing. Borrowed for the bind call only. The tag of the value does not
    /// name the argument's type: read it with @ref ScalarView::as and cast it
    /// to the type @ref type gives.
    dstd::Optional<ScalarView> constant(std::uint32_t i) const noexcept {
        const dftu_scalar* const scalar =
            i < consts_.size() ? consts_[i] : nullptr;
        if (scalar == nullptr) return dstd::nullopt;
        return dstd::make_optional(ScalarView(scalar));
    }

    /// Picks the type of the result column.
    void result(TypeId id) noexcept {
        if (out_ == nullptr) return;
        out_->struct_size = sizeof(dftu_type_info);
        out_->id = static_cast<dftu_dtype>(id);
    }

   private:
    dstd::Span<const dftu_type_info> args_;
    dstd::Span<const dftu_scalar* const> consts_;
    dftu_type_info* out_;
};

namespace detail {

/// The status of `error`, the numbering of dftu_status.
inline dftu_status udf_status_of(const dstd::Error& error) noexcept {
    return -static_cast<dftu_status>(error.condition()) - 1;
}

inline dstd::Error udf_error(dftu_status status,
                             dstd::StringView message = {}) noexcept {
    const std::int32_t code = -status - 1;
    const auto condition =
        status < 0 &&
                code <= static_cast<std::int32_t>(dstd::Condition::OutOfMemory)
            ? static_cast<dstd::Condition>(code)
            : dstd::Condition::Unknown;
    return dstd::Error(condition, message);
}

/// The status a callback returns for `error`, with its message on `ctx`. A null
/// `ctx` takes only the status.
inline dftu_status udf_report(dftu_ctx* ctx,
                              const dstd::Error& error) noexcept {
    const dftu_status status = udf_status_of(error);
    return error.message().empty() || ctx == nullptr
               ? status
               : dftu_fail(ctx, status, error.c_str());
}

}  // namespace detail

/// One batch a scalar UDF runs on: the argument columns the host borrowed, and
/// the row count they share. It holds no reference and allocates nothing.
class UdfBatch {
   public:
    UdfBatch(dftu_ctx* ctx, dstd::Span<const dftu_series* const> args,
             std::uint64_t rows) noexcept
        : ctx_(ctx), args_(args), rows_(rows) {}

    std::uint32_t size() const noexcept {
        return static_cast<std::uint32_t>(args_.size());
    }
    std::uint64_t rows() const noexcept { return rows_; }

    /// Argument column `i`, borrowed, `rows()` rows long. Read it with a
    /// @ref ColumnView: a column the host passes is FLAT, or a String column
    /// that @ref ColumnView::string_at reads as it is.
    ColumnView arg(std::uint32_t i) const noexcept {
        return ColumnView(i < args_.size() ? args_[i] : nullptr);
    }

    /// Fails with a cancelled error once the query was cancelled. Poll it once
    /// per few thousand rows of a long batch and return what it gives.
    dstd::Result<void> check() const noexcept {
        const dftu_status status = dftu_check(ctx_);
        if (status < 0) return detail::udf_error(status);
        return {};
    }

   private:
    dftu_ctx* ctx_;
    dstd::Span<const dftu_series* const> args_;
    std::uint64_t rows_;
};

/// How a UDF is called: the @ref UdfFlag set and the argument counts it takes.
struct UdfOptions {
    UdfFlag flags = UdfFlag::None;
    std::int32_t min_args = 1;
    std::int32_t max_args = 1;
};

/// A type `U` that registers as a scalar UDF is the state its bind keeps: bind
/// gives a new `U` (or an error, which fails the query) and exec runs it on one
/// batch, giving a @ref Column of `batch.rows()` rows of the type bind chose
/// (or an error, which fails the query with its message). exec may run on
/// several batches at once, so it only reads `U`.
template <class U>
concept UdfType = requires(UdfBind& bind, UdfBatch& batch, const U& udf) {
    { U::bind(bind) } -> std::same_as<dstd::Result<dstd::UniquePtr<U>>>;
    { udf.exec(batch) } -> std::same_as<dstd::Result<Column>>;
};

/// A @ref UdfType that names itself, for @ref make_udf: `NAME` is the duql
/// name, and `FLAGS`, `MIN_ARGS` and `MAX_ARGS` are optional (a deterministic
/// UDF of one argument by default).
template <class U>
concept NamedUdfType = UdfType<U> && requires {
    { U::NAME } -> std::convertible_to<const char*>;
};

namespace detail {

template <UdfType U>
dftu_udf_desc udf_desc_of(const char* name, UdfOptions options) noexcept {
    dftu_udf_desc d{};
    d.struct_size = sizeof(dftu_udf_desc);
    d.name = name;
    d.flags = static_cast<std::uint32_t>(options.flags);
    d.min_args = options.min_args;
    d.max_args = options.max_args;
    d.bind = [](dftu_ctx* ctx, void*, const dftu_type_info* args,
                std::uint32_t n, const dftu_scalar* const* consts,
                dftu_type_info* out, void** bound) noexcept -> dftu_status {
        UdfBind bind(dstd::Span<const dftu_type_info>(args, n),
                     consts != nullptr
                         ? dstd::Span<const dftu_scalar* const>(consts, n)
                         : dstd::Span<const dftu_scalar* const>(),
                     out);
        dstd::Result<dstd::UniquePtr<U>> made = U::bind(bind);
        if (!made) return udf_report(ctx, made.error());
        *bound = made->release();
        return DFTU_OK;
    };
    d.exec = [](dftu_ctx* ctx, void* bound, const dftu_series* const* args,
                std::uint32_t n, std::uint64_t rows,
                dftu_series** out) noexcept -> dftu_status {
        UdfBatch batch(ctx, dstd::Span<const dftu_series* const>(args, n),
                       rows);
        dstd::Result<Column> made = static_cast<const U*>(bound)->exec(batch);
        if (!made) return udf_report(ctx, made.error());
        *out = made->release();
        return DFTU_OK;
    };
    d.free_bound = [](void* bound) noexcept {
        dstd::UniquePtr<U> owner(static_cast<U*>(bound));
    };
    return d;
}

template <NamedUdfType U>
constexpr UdfOptions udf_options_of() noexcept {
    UdfOptions o;
    if constexpr (requires { U::FLAGS; }) o.flags = U::FLAGS;
    if constexpr (requires { U::MIN_ARGS; }) o.min_args = U::MIN_ARGS;
    if constexpr (requires { U::MAX_ARGS; })
        o.max_args = U::MAX_ARGS;
    else if constexpr (requires { U::MIN_ARGS; })
        o.max_args = U::MIN_ARGS;
    return o;
}

}  // namespace detail

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_UDF_ADAPTER_H
