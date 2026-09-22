#ifndef DFTRACER_UTILS_PLUGINS_COMPOSE_H
#define DFTRACER_UTILS_PLUGINS_COMPOSE_H

#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin.h>

#include <array>
#include <coroutine>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

// The columnar handles (defined in dftracer/utils/dataframe/abi.h). Forward
// declared so a column/table crosses a compose pipe as a first-class typed
// value (DFTU_T_SERIES / DFTU_T_TABLE) without this header depending on the
// dataframe ABI; the value is the handle pointer, so it is zero-copy.
struct dftu_series;
struct dftu_dataframe;

// Ranges-style composition of already-spawned dftu_task handles, mirroring the
// in-tree compose.h combinators but lowering to the host coro vtable
// (dftu_svc_coro: when_all / when_any). This is the SDK sugar over the C ABI's
// task combinators; `Host::all` / `Host::any` are the same lowering as method
// calls.
//
//   auto both  = compose(host, a) && compose(host, b);  // run both, await both
//   auto first = compose(host, a) || compose(host, b);  // race, first wins
//   co_await both;                                       // suspend until done
//
// Sequential composition needs no operator: a C ABI cannot thread a C++ value
// through a `|` pipe, so run tasks in order with `co_await a; co_await b;`
// inside a plugin::Task and pass values through your own captured state.
namespace dftracer::utils::plugins {

/// A dftu_task carrying its host, so the free operators below can reach the
/// coro vtable. co_await it to run the (possibly combined) task and suspend
/// until it completes; a NULL task (missing coro extension) resumes
/// immediately.
class Composed {
   public:
    Composed(const dftu_plugin_host* host, dftu_task* task)
        : host_(host), task_(task) {}

    dftu_task* task() const noexcept { return task_; }
    const dftu_plugin_host* host() const noexcept { return host_; }

    bool await_ready() const noexcept { return task_ == nullptr; }
    void await_suspend(
        std::coroutine_handle<Task::promise_type> h) const noexcept {
        h.promise().pending = task_;
    }
    void await_resume() const noexcept {}

   private:
    const dftu_plugin_host* host_;
    dftu_task* task_;
};

namespace detail {
inline const dftu_svc_coro* coro_ext(const dftu_plugin_host* h) {
    return h && h->get_service ? static_cast<const dftu_svc_coro*>(
                                     h->get_service(h->h, DFTU_SVC_CORO))
                               : nullptr;
}
inline const dftu_svc_compose* compose_ext(const dftu_plugin_host* h) {
    return h && h->get_service ? static_cast<const dftu_svc_compose*>(
                                     h->get_service(h->h, DFTU_SVC_COMPOSE))
                               : nullptr;
}
}  // namespace detail

/// Lift a task into the composable wrapper.
inline Composed compose(const dftu_plugin_host* host, dftu_task* task) {
    return Composed{host, task};
}

/// Join (`&&`): run both, await both. Lowers to when_all.
inline Composed operator&&(Composed a, Composed b) {
    const dftu_svc_coro* c = detail::coro_ext(a.host());
    dftu_task* ts[2] = {a.task(), b.task()};
    return Composed{
        a.host(), c && c->when_all ? c->when_all(a.host()->h, ts, 2) : nullptr};
}

/// Race (`||`): first to finish wins. Lowers to when_any.
inline Composed operator||(Composed a, Composed b) {
    const dftu_svc_coro* c = detail::coro_ext(a.host());
    dftu_task* ts[2] = {a.task(), b.task()};
    return Composed{
        a.host(), c && c->when_any ? c->when_any(a.host()->h, ts, 2) : nullptr};
}

/// map: build one task per item with `make_task(item) -> dftu_task*` and join
/// them all concurrently (when_all). Mirrors compose.h's map over the C ABI.
template <class Range, class MakeTask>
Composed map(const dftu_plugin_host* host, Range&& items, MakeTask make_task) {
    const dftu_svc_coro* c = detail::coro_ext(host);
    std::vector<dftu_task*> ts;
    for (auto&& item : items) ts.push_back(make_task(item));
    return Composed{host,
                    c && c->when_all
                        ? c->when_all(host->h, ts.data(),
                                      static_cast<std::uint32_t>(ts.size()))
                        : nullptr};
}

// ---- Typed compose ---------------------------------------------------------
//
// A compile-time-typed layer over dftu_svc_compose so a C++ plugin author
// writes real values instead of void*+size, and a mismatched pipe is a compile
// error instead of a runtime null. Values are POD (they cross the C ABI as
// bytes).
//
//   Op<int64_t,int64_t> a = make_op<int64_t,int64_t>(host, [](int64_t x){
//   return x*2; }); Op<int64_t,int64_t> b = make_op<int64_t,int64_t>(host,
//   [](int64_t x){ return x+10; }); int64_t in = 5, out = 0; int rc = -1;
//   co_await run(a | b, in, out, rc);          // out == 20; a|c mismatch won't
//   compile

/// Map a C++ POD type to its dftu_type; anything without a fixed-width mapping
/// crosses as an opaque DFTU_T_BYTES blob (still size-checked).
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

/// A typed op In -> Out over a dftu_op handle. Move/copy is just the handle;
/// the op itself is scan-lifetime, owned by the host.
template <class In, class Out>
class Op {
   public:
    Op(const dftu_plugin_host* host, dftu_op* op) : host_(host), op_(op) {}
    const dftu_plugin_host* host() const noexcept { return host_; }
    dftu_op* raw() const noexcept { return op_; }
    bool valid() const noexcept { return op_ != nullptr; }

   private:
    const dftu_plugin_host* host_;
    dftu_op* op_;
};

namespace detail {
template <class In, class Out, class Fn>
::dftu_task* op_thunk(void* state, const void* in, void* out, int* rc) {
    *static_cast<Out*>(out) =
        (*static_cast<Fn*>(state))(*static_cast<const In*>(in));
    if (rc) *rc = 0;
    return nullptr;  // a plain C++ transform completes synchronously
}
template <class Fn>
void op_free(void* state) {
    delete static_cast<Fn*>(state);
}
}  // namespace detail

/// A typed leaf from any callable `Out(const In&)`. In/Out must be trivially
/// copyable (they cross the ABI as bytes). The closure is heap-owned by the op.
template <class In, class Out, class Fn>
Op<In, Out> make_op(Host host, Fn fn) {
    static_assert(
        std::is_trivially_copyable_v<In> && std::is_trivially_copyable_v<Out>,
        "compose Op values cross the C ABI as bytes; In/Out must be "
        "trivially copyable");
    const dftu_plugin_host* raw = host.raw();
    const dftu_svc_compose* c = detail::compose_ext(raw);
    if (!c) return {raw, nullptr};
    Fn* state = new Fn(std::move(fn));
    dftu_op* op = c->make_op(
        raw->h, &detail::op_thunk<In, Out, Fn>, state, &detail::op_free<Fn>,
        type_tag<In>(), static_cast<std::uint32_t>(sizeof(In)), type_tag<Out>(),
        static_cast<std::uint32_t>(sizeof(Out)));
    if (!op) {
        delete state;
        return {raw, nullptr};
    }
    return {raw, op};
}

/// Optional early release of an op's resources; ops are otherwise freed at fold
/// teardown, so this is only needed to reclaim a large op mid-scan.
template <class In, class Out>
void free_op(Op<In, Out> op) {
    const dftu_svc_compose* c = detail::compose_ext(op.host());
    if (c && c->free_op && op.raw()) c->free_op(op.host()->h, op.raw());
}

/// Pipe (`|`): the middle type must chain, so `Op<A,B> | Op<C,D>` is only valid
/// when B==C - a mismatch is a compile error, not a runtime null.
template <class In, class Mid, class Out>
Op<In, Out> operator|(Op<In, Mid> a, Op<Mid, Out> b) {
    const dftu_svc_compose* c = detail::compose_ext(a.host());
    return {a.host(), c ? c->then(a.host()->h, a.raw(), b.raw()) : nullptr};
}

/// Op-level join: run every op on the SAME input and concatenate their outputs
/// in order. All ops must share In and Out, so the result value is a
/// std::array<Out, N> (N contiguous Out with no padding, exactly the
/// concatenation the host produces).
template <class In, class Out, class... Ops>
Op<In, std::array<Out, 1 + sizeof...(Ops)>> when_all(Op<In, Out> first,
                                                     Ops... rest) {
    static_assert((std::is_same_v<Ops, Op<In, Out>> && ...),
                  "when_all ops must share In and Out");
    const dftu_svc_compose* c = detail::compose_ext(first.host());
    dftu_op* ops[] = {first.raw(), rest.raw()...};
    return {first.host(), c && c->when_all ? c->when_all(first.host()->h, ops,
                                                         1 + sizeof...(Ops))
                                           : nullptr};
}

/// Op-level race: run every op on the same input; the result is whichever
/// finishes first. All ops must share In and Out (one out_size).
template <class In, class Out, class... Ops>
Op<In, Out> when_any(Op<In, Out> first, Ops... rest) {
    static_assert((std::is_same_v<Ops, Op<In, Out>> && ...),
                  "when_any ops must share In and Out");
    const dftu_svc_compose* c = detail::compose_ext(first.host());
    dftu_op* ops[] = {first.raw(), rest.raw()...};
    return {first.host(), c && c->when_any ? c->when_any(first.host()->h, ops,
                                                         1 + sizeof...(Ops))
                                           : nullptr};
}

/// Run a typed op over `in`, writing `out` and `rc`; co_await the result. `in`
/// and `out` are borrowed for the await, so they must outlive it.
template <class In, class Out>
Composed run(Op<In, Out> op, const In& in, Out& out, int& rc) {
    const dftu_svc_compose* c = detail::compose_ext(op.host());
    return {op.host(),
            c ? c->run(op.host()->h, op.raw(), &in, &out, &rc) : nullptr};
}

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_COMPOSE_H
