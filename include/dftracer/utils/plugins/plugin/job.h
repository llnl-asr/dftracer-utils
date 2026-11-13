// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_JOB_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_JOB_H

#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin/async.h>
#include <dftracer/utils/plugins/plugin/map.h>
#include <dftracer/utils/plugins/plugin/types.h>

#include <array>
#include <cstdint>
#include <type_traits>
#include <utility>

/* A compile-time-typed layer over dftu_svc_task, so a C++ plugin author writes
 * real values instead of void* and sizes, and a mismatched pipe is a compile
 * error instead of a runtime DFTU_E_INVALID. Values are trivially copyable
 * (they cross the C ABI as bytes).
 *
 *   auto a = co_await make_job<std::int64_t, std::int64_t>(host, [](auto x) {
 *       return x * 2; });
 *   auto b = co_await make_job<std::int64_t, std::int64_t>(host, [](auto x) {
 *       return x + 10; });
 *   auto both = co_await (a | b);
 *   std::int64_t in = 5, out = 0;
 *   co_await run(both, in, out);    // out == 20; a | c with another type
 *                                   // does not compile
 *
 * Every call that the host refuses returns its error as a dstd::Result, which
 * `co_await` turns into the end of the task. */

namespace dftracer::utils::plugins {

/// The dftu_type a C++ value type crosses the ABI as; anything without a
/// fixed-width mapping crosses as an opaque DFTU_T_BYTES blob (still
/// size-checked).
template <class T>
constexpr dftu_type type_tag() {
    if constexpr (std::is_same_v<T, std::int64_t>)
        return DFTU_T_I64;
    else if constexpr (std::is_same_v<T, double>)
        return DFTU_T_F64;
    else if constexpr (std::is_same_v<T, std::int32_t>)
        return DFTU_T_I32;
    else if constexpr (std::is_same_v<T, std::int16_t>)
        return DFTU_T_I16;
    else if constexpr (std::is_same_v<T, std::int8_t>)
        return DFTU_T_I8;
    else if constexpr (std::is_same_v<T, std::uint64_t>)
        return DFTU_T_U64;
    else if constexpr (std::is_same_v<T, std::uint32_t>)
        return DFTU_T_U32;
    else if constexpr (std::is_same_v<T, std::uint16_t>)
        return DFTU_T_U16;
    else if constexpr (std::is_same_v<T, std::uint8_t>)
        return DFTU_T_U8;
    else if constexpr (std::is_same_v<T, float>)
        return DFTU_T_F32;
    else if constexpr (std::is_same_v<T, dftu_series*>)
        return DFTU_T_SERIES;
    else if constexpr (std::is_same_v<T, dftu_dataframe*>)
        return DFTU_T_TABLE;
    else
        return DFTU_T_BYTES;
}

/// A typed job In -> Out over a dftu_job handle. Copying is copying the handle;
/// the job itself is fold-lifetime, owned by the host.
template <class In, class Out>
class Job {
   public:
    Job(Host host, dftu_job* job) : host_(host), job_(job) {}
    Host host() const noexcept { return host_; }
    dftu_job* raw() const noexcept { return job_; }

   private:
    Host host_;
    dftu_job* job_;
};

namespace detail {

template <class In, class Out, class Fn>
dftu_status job_thunk(dftu_ctx* ctx, void* state, const void* in, void* out,
                      dftu_task**) noexcept {
    return to_status(ctx, [&]() -> dftu_status {
        *static_cast<Out*>(out) =
            (*static_cast<Fn*>(state))(*static_cast<const In*>(in));
        return DFTU_OK;
    });
}

template <class Fn>
void job_free(void* state) {
    delete static_cast<Fn*>(state);
}

}  // namespace detail

/// A typed leaf from any callable `Out(const In&)`. In and Out must be
/// trivially copyable. The closure is heap-owned by the job.
template <class In, class Out, class Fn>
dstd::Result<Job<In, Out>> make_job(Host host, Fn fn,
                                    dftu_job_where where = DFTU_JOB_CPU) {
    static_assert(
        std::is_trivially_copyable_v<In> && std::is_trivially_copyable_v<Out>,
        "job values cross the C ABI as bytes; In/Out must be trivially "
        "copyable");
    DFTRACER_UTILS_TRY(const dftu_svc_task* svc, host.tasks());
    Fn* state = new Fn(std::move(fn));
    dftu_job_desc desc{};
    desc.struct_size = sizeof(dftu_job_desc);
    desc.where = where;
    desc.fn = &detail::job_thunk<In, Out, Fn>;
    desc.state = state;
    desc.free_state = &detail::job_free<Fn>;
    desc.in_ty = type_tag<In>();
    desc.in_size = static_cast<std::uint32_t>(sizeof(In));
    desc.out_ty = type_tag<Out>();
    desc.out_size = static_cast<std::uint32_t>(sizeof(Out));
    dftu_job* job = nullptr;
    const dftu_status st = svc->job(host.ctx(), host.raw()->h, &desc, &job);
    if (st < 0) {
        delete state;
        return detail::reported(st).error();
    }
    return Job<In, Out>{host, job};
}

/// Pipe (`|`): the middle type must chain, so `Job<A,B> | Job<B,C>` is valid
/// and a mismatch is a compile error.
template <class In, class Mid, class Out>
dstd::Result<Job<In, Out>> operator|(Job<In, Mid> a, Job<Mid, Out> b) {
    Host host = a.host();
    DFTRACER_UTILS_TRY(const dftu_svc_task* svc, host.tasks());
    dftu_job* out = nullptr;
    DFTRACER_UTILS_TRY_VOID(detail::reported(
        svc->then(host.ctx(), host.raw()->h, a.raw(), b.raw(), &out)));
    return Job<In, Out>{host, out};
}

/// Run every job on the SAME input and concatenate their outputs in order, as
/// a std::array<Out, N>. The first error cancels the rest and is the result.
template <class In, class Out, class... Jobs>
dstd::Result<Job<In, std::array<Out, 1 + sizeof...(Jobs)>>> when_all(
    Job<In, Out> first, Jobs... rest) {
    static_assert((std::is_same_v<Jobs, Job<In, Out>> && ...),
                  "when_all jobs must share In and Out");
    Host host = first.host();
    DFTRACER_UTILS_TRY(const dftu_svc_task* svc, host.tasks());
    dftu_job* jobs[] = {first.raw(), rest.raw()...};
    dftu_job* out = nullptr;
    DFTRACER_UTILS_TRY_VOID(detail::reported(
        svc->all(host.ctx(), host.raw()->h, jobs, 1 + sizeof...(Jobs), &out)));
    return Job<In, std::array<Out, 1 + sizeof...(Jobs)>>{host, out};
}

/// Race every job on the same input: the first to finish decides the output
/// (or the error), and the rest are cancelled.
template <class In, class Out, class... Jobs>
dstd::Result<Job<In, Out>> when_any(Job<In, Out> first, Jobs... rest) {
    static_assert((std::is_same_v<Jobs, Job<In, Out>> && ...),
                  "when_any jobs must share In and Out");
    Host host = first.host();
    DFTRACER_UTILS_TRY(const dftu_svc_task* svc, host.tasks());
    dftu_job* jobs[] = {first.raw(), rest.raw()...};
    dftu_job* out = nullptr;
    DFTRACER_UTILS_TRY_VOID(detail::reported(
        svc->any(host.ctx(), host.raw()->h, jobs, 1 + sizeof...(Jobs), &out)));
    return Job<In, Out>{host, out};
}

/// Start the job over `in`, writing `out`; co_await the result. `in` and
/// `out` are borrowed until the task finishes, so they must outlive the await.
template <class In, class Out>
AsyncOp run(Job<In, Out> job, const In& in, Out& out) {
    Host host = job.host();
    auto svc = host.tasks();
    if (!svc.has_value()) return AsyncOp{std::move(svc).error()};
    dftu_task* t = nullptr;
    return detail::started(
        host.raw(),
        (*svc)->start(host.ctx(), host.raw()->h, job.raw(), &in, &out, &t), t);
}

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_JOB_H */
