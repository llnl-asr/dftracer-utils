// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_UDAF_ADAPTER_H
#define DFTRACER_UTILS_DATAFRAME_UDAF_ADAPTER_H

// The classes a user-defined aggregate is written with, and the one adapter
// that turns such a class into a dftu_udaf_desc. A plugin registers the class
// through PluginBuilder::udaf and in-process code through make_udaf (udaf.h);
// this header holds neither, so a plugin includes no more than the C ABI needs.
// bind takes the @ref UdfBind and update the @ref UdfBatch of a scalar UDF: the
// argument types, the constants and a batch of argument columns are the same.

#include <dftracer/utils/dataframe/c_api_udaf.h>
#include <dftracer/utils/dataframe/udf_adapter.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

#include <concepts>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

namespace dftracer::utils::dataframe {

/// The states of a batch of rows or of a finalize, as the host hands them over:
/// entry `i` is the state of row or group `i`. It is null for a row that
/// update skips, never in finalize.
template <class State>
class UdafStateSet {
   public:
    UdafStateSet(void* const* states, std::uint64_t n) noexcept
        : states_(states), n_(n) {}

    std::uint64_t size() const noexcept { return n_; }

    State* operator[](std::uint64_t i) const noexcept {
        return static_cast<State*>(states_[i]);
    }

   private:
    void* const* states_;
    std::uint64_t n_;
};

/// How a UDAF is called: the DFTU_AGG_* flags it sets (DFTU_AGG_PARALLEL_SAFE,
/// DFTU_AGG_NULL_AWARE) and the argument counts it takes. The adapter derives
/// only DFTU_AGG_STATE_POD, from the state type.
struct UdafOptions {
    std::uint32_t flags = 0;
    std::int32_t min_args = 1;
    std::int32_t max_args = 1;
};

/// A type `A` that registers as a UDAF is the state its bind keeps. `A::State`
/// is the state of one group: default constructible, aligned to at most 64.
/// The host zero-fills its memory and constructs it in place, and destroys it
/// in place. A trivially copyable State is plain bytes (DFTU_AGG_STATE_POD) and
/// the host copies it to spill. Any other State needs `serialize`, which
/// appends the state to a string, and `deserialize`, which reads those bytes
/// into a state a default construction made.
///
/// bind gives a new `A` or an error, which fails the query. update feeds a
/// batch to the states of its rows (null entries are skipped rows), combine
/// merges `from` into `into`, and finalize gives an owned result column with
/// one row per state, of the type bind chose. They only read `A`, so one `A`
/// serves every state set. A UDAF that retracts rows is not written through
/// this class.
template <class A>
concept UdafType =
    requires {
        typename A::State;
        requires std::is_default_constructible_v<typename A::State>;
        requires alignof(typename A::State) <= 64;
    } &&
    requires(UdfBind& bind, const UdfBatch& batch, const A& udaf,
             UdafStateSet<typename A::State>& states,
             typename A::State& state) {
        { A::bind(bind) } -> std::same_as<dstd::Result<dstd::UniquePtr<A>>>;
        { udaf.update(states, batch) } -> std::same_as<dstd::Result<void>>;
        { udaf.combine(state, state) } -> std::same_as<dstd::Result<void>>;
        { udaf.finalize(states) } -> std::same_as<dstd::Result<dftu_series*>>;
    } &&
    (std::is_trivially_copyable_v<typename A::State> ||
     requires(const A& udaf, const typename A::State& cstate,
              typename A::State& state, dstd::String& out,
              dstd::StringView in) {
         { udaf.serialize(cstate, out) } -> std::same_as<void>;
         { udaf.deserialize(state, in) } -> std::same_as<dstd::Result<void>>;
     });

/// A @ref UdafType that names itself, for @ref make_udaf: `NAME` is the duql
/// name, and `FLAGS`, `MIN_ARGS` and `MAX_ARGS` are optional (one argument by
/// default).
template <class A>
concept NamedUdafType = UdafType<A> && requires {
    { A::NAME } -> std::convertible_to<const char*>;
};

namespace detail {

template <UdafType A>
dftu_udaf_desc udaf_desc_of(const char* name, UdafOptions options) noexcept {
    using State = typename A::State;
    constexpr bool IS_POD = std::is_trivially_copyable_v<State>;
    dftu_udaf_desc d{};
    d.struct_size = sizeof(dftu_udaf_desc);
    d.name = name;
    d.flags = options.flags | (IS_POD ? DFTU_AGG_STATE_POD : 0u);
    d.min_args = options.min_args;
    d.max_args = options.max_args;
    d.state_size = sizeof(State);
    d.state_align = alignof(State);
    d.bind = [](dftu_ctx* ctx, void*, const dftu_type_info* args,
                std::uint32_t n, const dftu_scalar* const* consts,
                dftu_type_info* out, void** bound) noexcept -> dftu_status {
        UdfBind bind(args, n, consts, out);
        dstd::Result<dstd::UniquePtr<A>> made = A::bind(bind);
        if (!made) return udf_report(ctx, made.error());
        *bound = made->release();
        return DFTU_OK;
    };
    d.init = [](void*, void* const* states, std::uint64_t n) noexcept {
        for (std::uint64_t i = 0; i < n; ++i) ::new (states[i]) State();
    };
    d.update = [](dftu_ctx* ctx, void* bound, void* const* states,
                  const dftu_series* const* args, std::uint32_t nargs,
                  std::uint64_t rows) noexcept -> dftu_status {
        const UdfBatch batch(ctx, args, nargs, rows);
        UdafStateSet<State> set(states, rows);
        dstd::Result<void> done =
            static_cast<const A*>(bound)->update(set, batch);
        return done ? DFTU_OK : udf_report(ctx, done.error());
    };
    d.combine = [](dftu_ctx* ctx, void* bound, void* const* into,
                   void* const* from, std::uint64_t n) noexcept -> dftu_status {
        const A& udaf = *static_cast<const A*>(bound);
        for (std::uint64_t i = 0; i < n; ++i) {
            dstd::Result<void> done = udaf.combine(
                *static_cast<State*>(into[i]), *static_cast<State*>(from[i]));
            if (!done) return udf_report(ctx, done.error());
        }
        return DFTU_OK;
    };
    d.finalize = [](dftu_ctx* ctx, void* bound, void* const* states,
                    std::uint64_t n,
                    dftu_series** out) noexcept -> dftu_status {
        UdafStateSet<State> set(states, n);
        dstd::Result<dftu_series*> made =
            static_cast<const A*>(bound)->finalize(set);
        if (!made) return udf_report(ctx, made.error());
        *out = *made;
        return DFTU_OK;
    };
    if constexpr (!IS_POD) {
        d.destroy = [](void*, void* const* states, std::uint64_t n) noexcept {
            for (std::uint64_t i = 0; i < n; ++i)
                static_cast<State*>(states[i])->~State();
        };
        d.serialize = [](dftu_ctx*, void* bound, const void* state,
                         std::uint8_t* out, std::uint64_t cap,
                         std::uint64_t* size) noexcept -> dftu_status {
            dstd::String bytes;
            static_cast<const A*>(bound)->serialize(
                *static_cast<const State*>(state), bytes);
            *size = bytes.size();
            if (out != nullptr && cap >= bytes.size())
                std::memcpy(out, bytes.data(), bytes.size());
            return DFTU_OK;
        };
        d.deserialize = [](dftu_ctx* ctx, void* bound, const std::uint8_t* in,
                           std::uint64_t n,
                           void* state) noexcept -> dftu_status {
            dstd::Result<void> done = static_cast<const A*>(bound)->deserialize(
                *static_cast<State*>(state),
                dstd::StringView(reinterpret_cast<const char*>(in), n));
            return done ? DFTU_OK : udf_report(ctx, done.error());
        };
    }
    d.free_bound = [](void* bound) noexcept {
        dstd::UniquePtr<A> owner(static_cast<A*>(bound));
    };
    return d;
}

template <NamedUdafType A>
constexpr UdafOptions udaf_options_of() noexcept {
    UdafOptions o;
    if constexpr (requires { A::FLAGS; }) o.flags = A::FLAGS;
    if constexpr (requires { A::MIN_ARGS; }) o.min_args = A::MIN_ARGS;
    if constexpr (requires { A::MAX_ARGS; })
        o.max_args = A::MAX_ARGS;
    else if constexpr (requires { A::MIN_ARGS; })
        o.max_args = A::MIN_ARGS;
    return o;
}

}  // namespace detail

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_UDAF_ADAPTER_H
