// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_UDF_ADAPTER_H
#define DFTRACER_UTILS_DATAFRAME_UDF_ADAPTER_H

// The classes a scalar UDF is written with, and the one adapter that turns such
// a class into a dftu_udf_desc. A plugin registers the class through
// PluginBuilder::udf and in-process code through make_udf (udf.h); this header
// holds neither, so a plugin includes no more than the C ABI needs.

#include <dftracer/utils/dataframe/c_api_udf.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

#include <concepts>
#include <cstdint>

namespace dftracer::utils::dataframe {

/// What bind of a scalar UDF sees: the argument types, the constants the call
/// writes, and the result type to pick.
class UdfBind {
   public:
    UdfBind(const dftu_type_info* args, std::uint32_t n,
            const dftu_scalar* const* consts, dftu_type_info* out) noexcept
        : args_(args), n_(n), consts_(consts), out_(out) {}

    /// The number of arguments the call passes.
    std::uint32_t size() const noexcept { return n_; }

    /// The type of argument `i`, a DFTU_TYPE_* code.
    dftu_dtype type(std::uint32_t i) const noexcept { return args_[i].id; }

    /// The value of argument `i` when the call writes a constant there, else
    /// null. Borrowed for the bind call only.
    const dftu_scalar* constant(std::uint32_t i) const noexcept {
        return consts_[i];
    }

    /// Picks the type of the result column.
    void result(dftu_dtype id) noexcept {
        out_->struct_size = sizeof(dftu_type_info);
        out_->id = id;
    }

   private:
    const dftu_type_info* args_;
    std::uint32_t n_;
    const dftu_scalar* const* consts_;
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

/// The status a callback returns for `error`, with its message on `ctx`.
inline dftu_status udf_report(dftu_ctx* ctx,
                              const dstd::Error& error) noexcept {
    const dftu_status status = udf_status_of(error);
    return error.message().empty() ? status
                                   : dftu_fail(ctx, status, error.c_str());
}

}  // namespace detail

/// One batch a scalar UDF runs on.
class UdfBatch {
   public:
    UdfBatch(dftu_ctx* ctx, const dftu_series* const* args, std::uint32_t n,
             std::uint64_t rows) noexcept
        : ctx_(ctx), args_(args), n_(n), rows_(rows) {}

    std::uint32_t size() const noexcept { return n_; }
    std::uint64_t rows() const noexcept { return rows_; }

    /// Argument column `i`, borrowed, `rows()` rows long.
    const dftu_series* arg(std::uint32_t i) const noexcept { return args_[i]; }

    /// Fails with a cancelled error once the query was cancelled. Poll it once
    /// per few thousand rows of a long batch and return what it gives.
    dstd::Result<void> check() const noexcept {
        const dftu_status status = dftu_check(ctx_);
        if (status < 0) return detail::udf_error(status);
        return {};
    }

   private:
    dftu_ctx* ctx_;
    const dftu_series* const* args_;
    std::uint32_t n_;
    std::uint64_t rows_;
};

/// How a UDF is called: the DFTU_FN_* flags and the argument counts it takes.
struct UdfOptions {
    std::uint32_t flags = 0;
    std::int32_t min_args = 1;
    std::int32_t max_args = 1;
};

/// A type `U` that registers as a scalar UDF is the state its bind keeps: bind
/// gives a new `U` (or an error, which fails the query) and exec runs it on one
/// batch, giving an owned result column of `batch.rows()` rows of the type bind
/// chose. exec may run on several batches at once, so it only reads `U`.
template <class U>
concept UdfType = requires(UdfBind& bind, UdfBatch& batch, const U& udf) {
    { U::bind(bind) } -> std::same_as<dstd::Result<dstd::UniquePtr<U>>>;
    { udf.exec(batch) } -> std::same_as<dstd::Result<dftu_series*>>;
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
    d.flags = options.flags;
    d.min_args = options.min_args;
    d.max_args = options.max_args;
    d.bind = [](dftu_ctx* ctx, void*, const dftu_type_info* args,
                std::uint32_t n, const dftu_scalar* const* consts,
                dftu_type_info* out, void** bound) noexcept -> dftu_status {
        UdfBind bind(args, n, consts, out);
        dstd::Result<dstd::UniquePtr<U>> made = U::bind(bind);
        if (!made) return udf_report(ctx, made.error());
        *bound = made->release();
        return DFTU_OK;
    };
    d.exec = [](dftu_ctx* ctx, void* bound, const dftu_series* const* args,
                std::uint32_t n, std::uint64_t rows,
                dftu_series** out) noexcept -> dftu_status {
        UdfBatch batch(ctx, args, n, rows);
        dstd::Result<dftu_series*> made =
            static_cast<const U*>(bound)->exec(batch);
        if (!made) return udf_report(ctx, made.error());
        *out = *made;
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
