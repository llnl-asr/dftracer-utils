// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_REGISTER_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_REGISTER_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin/async.h>
#include <dftracer/utils/plugins/plugin/map.h>
#include <dftracer/utils/plugins/plugin/node.h>
#include <dftracer/utils/plugins/plugin/source.h>
#include <dftracer/utils/plugins/plugin/types.h>

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <utility>

namespace dftracer::utils::plugins {

/// Typed, non-owning view over one dftu_value node; a Slice may keep it since
/// the host-owned config tree outlives it. Default-constructed (or an
/// out-of-range/wrong-kind lookup) yields a null ConfigValue: kind() is
/// ValueKind::Null and every predicate but is_null() is false.
class ConfigValue {
   public:
    ConfigValue() = default;
    explicit ConfigValue(const dftu_value* v) noexcept : v_(v) {}

    ValueKind kind() const noexcept {
        return v_ ? static_cast<ValueKind>(v_->kind) : ValueKind::Null;
    }
    bool is_null() const noexcept { return !v_ || v_->kind == DFTU_VAL_NULL; }
    bool is_bool() const noexcept { return v_ && v_->kind == DFTU_VAL_BOOL; }
    bool is_i64() const noexcept { return v_ && v_->kind == DFTU_VAL_I64; }
    bool is_f64() const noexcept { return v_ && v_->kind == DFTU_VAL_F64; }
    bool is_str() const noexcept { return v_ && v_->kind == DFTU_VAL_STR; }
    bool is_array() const noexcept { return v_ && v_->kind == DFTU_VAL_ARRAY; }
    bool is_object() const noexcept {
        return v_ && v_->kind == DFTU_VAL_OBJECT;
    }

    bool as_bool(bool dflt = false) const noexcept {
        return dftu_as_bool(v_, dflt ? 1 : 0) != 0;
    }
    std::int64_t as_i64(std::int64_t dflt = 0) const noexcept {
        return dftu_as_i64(v_, dflt);
    }
    double as_f64(double dflt = 0.0) const noexcept {
        return dftu_as_f64(v_, dflt);
    }
    dstd::StringView as_str(dstd::StringView dflt = {}) const noexcept {
        std::uint32_t n = 0;
        const char* s = dftu_as_str(v_, &n);
        return s ? dstd::StringView{s, n} : dflt;
    }

    /// ARRAY child count or OBJECT member count; 0 for any other kind.
    std::uint32_t size() const noexcept {
        return (is_array() || is_object()) ? v_->count : 0;
    }
    /// Element `i` of an ARRAY; out of range or not an array yields null.
    ConfigValue operator[](std::uint32_t i) const noexcept {
        return (is_array() && i < v_->count) ? ConfigValue{&v_->as.items[i]}
                                             : ConfigValue{};
    }
    /// Member `key` of an OBJECT; absent or not an object yields null.
    ConfigValue operator[](dstd::StringView key) const noexcept {
        if (!is_object()) return ConfigValue{};
        for (std::uint32_t i = 0; i < v_->count; ++i) {
            const dftu_member& m = v_->as.members[i];
            if (dstd::StringView{m.key, m.key_len} == key)
                return ConfigValue{m.value};
        }
        return ConfigValue{};
    }

    const dftu_value* raw() const noexcept { return v_; }

   private:
    const dftu_value* v_ = nullptr;
};

/** View over the config tree; a Slice may keep returned string_views since the
   host-owned tree outlives it. */
class Config {
   public:
    Config() = default;
    explicit Config(const dftu_value* root) : root_(root) {}

    const dftu_value* find(dstd::StringView key) const {
        if (!root_ || root_->kind != DFTU_VAL_OBJECT) return nullptr;
        for (std::uint32_t i = 0; i < root_->count; ++i) {
            const dftu_member& m = root_->as.members[i];
            if (dstd::StringView{m.key, m.key_len} == key) return m.value;
        }
        return nullptr;
    }

    dstd::StringView get(dstd::StringView key,
                         dstd::StringView dflt = {}) const {
        const dftu_value* v = find(key);
        return (v && v->kind == DFTU_VAL_STR)
                   ? dstd::StringView{v->as.str, v->count}
                   : dflt;
    }
    std::int64_t get_int(dstd::StringView key, std::int64_t dflt = 0) const {
        return dftu_as_i64(find(key), dflt);
    }
    double get_double(dstd::StringView key, double dflt = 0.0) const {
        return dftu_as_f64(find(key), dflt);
    }
    bool get_bool(dstd::StringView key, bool dflt = false) const {
        return dftu_as_bool(find(key), dflt ? 1 : 0) != 0;
    }
    Config child(dstd::StringView key) const {
        const dftu_value* v = find(key);
        return Config{(v && v->kind == DFTU_VAL_OBJECT) ? v : nullptr};
    }

    /// Typed view of the value at `key`, of any kind (including ARRAY/OBJECT);
    /// null if absent. Prefer this over raw() for walking nested config.
    ConfigValue value(dstd::StringView key) const {
        return ConfigValue{find(key)};
    }
    /// Typed view of the ARRAY at `key`; null if absent or not an array.
    /// Iterate elements with ConfigValue::size()/operator[].
    ConfigValue array(dstd::StringView key) const {
        return ConfigValue{raw_array(key)};
    }
    /// The ARRAY at `key` coerced to int64 per element (dftu_as_i64 rules);
    /// empty if the key is absent or not an array.
    dstd::Vector<std::int64_t> get_int_array(dstd::StringView key) const {
        dstd::Vector<std::int64_t> out;
        if (const dftu_value* a = raw_array(key)) {
            out.reserve(a->count);
            for (std::uint32_t i = 0; i < a->count; ++i)
                out.push_back(dftu_as_i64(&a->as.items[i], 0));
        }
        return out;
    }
    /// The ARRAY at `key` coerced to double per element (dftu_as_f64 rules);
    /// empty if the key is absent or not an array.
    dstd::Vector<double> get_double_array(dstd::StringView key) const {
        dstd::Vector<double> out;
        if (const dftu_value* a = raw_array(key)) {
            out.reserve(a->count);
            for (std::uint32_t i = 0; i < a->count; ++i)
                out.push_back(dftu_as_f64(&a->as.items[i], 0.0));
        }
        return out;
    }
    /// The ARRAY at `key` as string_views (non-STR elements yield an empty
    /// view); the views borrow the host-owned config tree. Empty if the key is
    /// absent or not an array.
    dstd::Vector<dstd::StringView> get_string_array(
        dstd::StringView key) const {
        dstd::Vector<dstd::StringView> out;
        if (const dftu_value* a = raw_array(key)) {
            out.reserve(a->count);
            for (std::uint32_t i = 0; i < a->count; ++i) {
                const dftu_value& e = a->as.items[i];
                out.push_back(e.kind == DFTU_VAL_STR
                                  ? dstd::StringView{e.as.str, e.count}
                                  : dstd::StringView{});
            }
        }
        return out;
    }

    explicit operator bool() const { return root_ != nullptr; }
    const dftu_value* raw() const { return root_; }

   private:
    const dftu_value* raw_array(dstd::StringView key) const {
        const dftu_value* v = find(key);
        return (v && v->kind == DFTU_VAL_ARRAY) ? v : nullptr;
    }

    const dftu_value* root_ = nullptr;
};

namespace detail {

template <class Slice>
struct Holder {
    dftu_plugin vt{};
    Config config;
    dstd::String plan; /**< backs plan_duql's const char* */
};

template <class Slice>
Holder<Slice>* holder_of(void* self) {
    return static_cast<Holder<Slice>*>(self);
}

/// A Slice that can fail at creation: `static dstd::Result<Slice>
/// from_config(const Config&)` is used in place of the constructor.
template <class Slice>
concept FromConfigSlice = requires(const Config& c) {
    { Slice::from_config(c) } -> std::same_as<dstd::Result<Slice>>;
};

/// A Slice whose synchronous step takes the ergonomic Batch cursor over the
/// batch's dftu_dataframe columns.
template <class Slice>
concept BatchViewStep =
    requires(Slice& s, const Batch& b, Host h) { s.step(b, h); };

/// A Slice whose synchronous step takes the raw dftu_dataframe batch, the
/// shape that feeds Host::agg directly (its accumulator takes column
/// batches).
template <class Slice>
concept ColumnStep =
    requires(Slice& s, const dftu_dataframe* df, Host h) { s.step(df, h); };

template <class Slice>
concept AsyncFinalize = requires(Slice& s, Host h) {
    { s.on_finalize(h) } -> std::same_as<Task>;
};

/// A Slice that rewrites the batch for every plugin after it in fold order:
/// returns a new dftu_dataframe (the host frees it) or nullptr for unchanged,
/// or the error that failed it. Takes the place of step().
template <class Slice>
concept TransformStep = requires(Slice& s, const dftu_dataframe* df, Host h) {
    { s.transform(df, h) } -> std::same_as<dstd::Result<dftu_dataframe*>>;
};

/// A Slice that can say how big it is; the host's measure of it against the
/// scan's memory budget.
template <class Slice>
concept MeasurableSlice = requires(const Slice& s) {
    { s.bytes() } -> std::convertible_to<std::uint64_t>;
};

/// A Slice that can shrink on request: `reclaim(want)` frees up to `want`
/// bytes however it can and returns how many it freed.
template <class Slice>
concept ReclaimableSlice = requires(Slice& s, std::uint64_t want) {
    { s.reclaim(want) } -> std::convertible_to<std::uint64_t>;
};

/// One host step of the driven coroutine: runs it to its next co_await and
/// hands the host the tasks it awaits, DFTU_END once it ran to completion, or
/// the error it ended with. A task that ended with an error is not resumed: its
/// error, or the failure of a task it awaited, is the step's status. The host
/// destroys the frame through release_thunk.
inline dftu_status step_thunk(dftu_ctx* ctx, void* coro, dftu_task** wait,
                              std::uint32_t cap,
                              std::uint32_t* n_wait) noexcept {
    auto h = std::coroutine_handle<Task::promise_type>::from_address(coro);
    Task::promise_type& p = h.promise();
    p.proxy->real = ctx;
    if (p.n_wait > 0 && !p.tolerant) {
        dftu_status first = DFTU_OK;
        for (std::uint32_t i = 0; i < p.n_wait && first >= 0; ++i)
            first = p.svc->status(ctx, p.host->h, p.wait[i]);
        if (first < 0) return first;
    }
    p.n_wait = 0;
    p.tolerant = false;
    h.resume();
    if (p.failed) return report(ctx, p.error);
    if (!h.done()) {
        if (p.n_wait > cap) {
            for (std::uint32_t i = 0; i < p.n_wait; ++i) {
                if (p.svc->discard) p.svc->discard(p.host->h, p.wait[i]);
            }
            return dftu_fail(ctx, DFTU_E_INTERNAL,
                             "a plugin task awaited more tasks than a drive "
                             "step has room for");
        }
        for (std::uint32_t i = 0; i < p.n_wait; ++i) wait[i] = p.wait[i];
        *n_wait = p.n_wait;
        return DFTU_OK;
    }
    return DFTU_END;
}

inline void release_thunk(void* coro) noexcept {
    std::coroutine_handle<Task::promise_type>::from_address(coro).destroy();
}

/// Hand `coro` to the host's drive; on success the host owns the frame and
/// destroys it through release_thunk.
inline dftu_status drive_coro(dftu_ctx* ctx, const dftu_plugin_host* host,
                              Task&& coro,
                              dstd::UniquePtr<detail::ProxyCtx> proxy,
                              dftu_task** pending) {
    const dftu_svc_task* svc =
        host->get_service ? static_cast<const dftu_svc_task*>(
                                host->get_service(host->h, DFTU_SVC_TASK))
                          : nullptr;
    if (!svc || !svc->drive)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED,
                         "host lacks the service dftu.svc.task");
    auto h = coro.release();
    h.promise().host = host;
    h.promise().svc = svc;
    h.promise().proxy = std::move(proxy);
    const dftu_status st = svc->drive(ctx, host->h, &step_thunk, h.address(),
                                      &release_thunk, pending);
    if (st < 0) h.destroy();
    return st;
}

/// A Slice naming the ports and accumulators it produces via a static
/// `provides()` returning a range of const char*.
template <class Slice>
concept DeclaresProvides = requires {
    { std::begin(Slice::provides()) };
    { std::end(Slice::provides()) };
};

/// A Slice naming the ports and accumulators it reads via a static
/// `consumes()` returning a range of const char*.
template <class Slice>
concept DeclaresConsumes = requires {
    { std::begin(Slice::consumes()) };
    { std::end(Slice::consumes()) };
};

/// A Slice naming the batch columns it reads via a static `reads()` returning
/// a range of const char*. Declaring them projects the batch: only those
/// columns are materialized, instead of every fixed column plus one per arg
/// key in the batch.
template <class Slice>
concept DeclaresReads = requires {
    { std::begin(Slice::reads()) };
    { std::end(Slice::reads()) };
};

enum class NameList { Provides, Consumes, Reads };

// The declared range copied into a NULL-terminated array with static storage,
// which is the lifetime dftu_plugin::provides/consumes/reads require.
template <class Slice, NameList Which>
const char* const* name_list_thunk(void*) {
    static const dstd::Vector<const char*> NAMES = [] {
        dstd::Vector<const char*> v;
        if constexpr (Which == NameList::Provides) {
            for (const char* n : Slice::provides()) v.push_back(n);
        } else if constexpr (Which == NameList::Consumes) {
            for (const char* n : Slice::consumes()) v.push_back(n);
        } else {
            for (const char* n : Slice::reads()) v.push_back(n);
        }
        v.push_back(nullptr);
        return v;
    }();
    return NAMES.data();
}

/// A Slice declaring the config keys it reads via a static `config_keys()`
/// returning a range of dftu_config_key (a NULL name is not needed; the thunk
/// terminates the array).
template <class Slice>
concept DeclaresConfigKeys = requires {
    { std::begin(Slice::config_keys()) };
    { std::end(Slice::config_keys()) };
};

// The declared keys copied into a NULL-name-terminated array with static
// storage, plus the "duql" key every Slice built here reads (make_plugin
// takes plan_duql from it), so declaring keys does not make a caller's
// `query` an unknown one.
template <class Slice>
const dftu_config_key* config_key_thunk(void*) {
    static const dstd::Vector<dftu_config_key> KEYS = [] {
        dstd::Vector<dftu_config_key> v;
        for (const dftu_config_key& k : Slice::config_keys()) v.push_back(k);
        v.push_back({"duql", DFTU_VAL_STR, 0,
                     "coarse DSL predicate the scan is pruned by"});
        v.push_back({nullptr, DFTU_CONFIG_ANY, 0, nullptr});
        return v;
    }();
    return KEYS.data();
}

}  // namespace detail

/** Build a dftu_plugin from a Slice providing Slice(const Config&), merge, and
   either a synchronous step(const Batch&, Host) / step(const dftu_dataframe*,
   Host), or a transform(const dftu_dataframe*, Host) returning the rewritten
   batch every later plugin receives (dftu_plugin::transform, in place of
   step), and a sync or Task-returning on_finalize. To take part in the fold
   ordering a Slice may also declare `static ... provides()` and `static ...
   consumes()`, each a range of const char* port/accumulator names outliving the
   plugin; make_plugin wires them to dftu_plugin::provides / ::consumes. A
   `static ... reads()` of batch column names becomes dftu_plugin::reads, which
   projects the batch down to those columns. A `bytes()` const on the Slice
   becomes dftu_plugin::bytes and a `reclaim(std::uint64_t want)` returning
   the bytes freed becomes dftu_plugin::reclaim, so the host can hold the
   slice to the scan's memory budget. A Slice that can fail at creation
   provides `static dstd::Result<Slice> from_config(const Config&)`, which
   make_slice uses in place of the constructor. A callback reports failure by
   returning a dstd::Result (step, merge, finalize and the others may also
   return void when they cannot fail); the trampolines turn it into the
   callback's status and message and hold no catch, so a callback that throws
   terminates. */
template <class Slice>
dftu_plugin* make_plugin(const dftu_value* config) {
    auto* hd = new detail::Holder<Slice>();
    hd->config = Config(config);
    hd->plan = hd->config.get("duql");

    dftu_plugin& vt = hd->vt;
    vt.struct_size = sizeof(dftu_plugin);
    vt.self = hd;

    vt.plan_duql = [](void* self) -> const char* {
        auto* h = detail::holder_of<Slice>(self);
        return h->plan.empty() ? nullptr : h->plan.c_str();
    };

    vt.make_slice = [](dftu_ctx* ctx, void* self,
                       void** out) noexcept -> dftu_status {
        return detail::to_status(ctx, [&]() -> dftu_status {
            const Config& config = detail::holder_of<Slice>(self)->config;
            if constexpr (detail::FromConfigSlice<Slice>) {
                dstd::Result<Slice> made = Slice::from_config(config);
                if (!made.has_value()) return report(ctx, made.error());
                *out = new Slice(std::move(*made));
            } else {
                *out = new Slice(config);
            }
            return DFTU_OK;
        });
    };

    // dftu_plugin::on_batch is synchronous only; the host frees the
    // dftu_dataframe right after the call.
    if constexpr (detail::TransformStep<Slice>) {
        vt.transform = [](dftu_ctx* ctx, void* slice, const dftu_dataframe* df,
                          const dftu_plugin_host* host,
                          dftu_dataframe** out) noexcept -> dftu_status {
            return detail::to_status(ctx, [&]() -> dftu_status {
                dstd::Result<dftu_dataframe*> r =
                    static_cast<Slice*>(slice)->transform(df, Host{host, ctx});
                if (!r.has_value()) return report(ctx, r.error());
                *out = *r;
                return DFTU_OK;
            });
        };
    } else {
        vt.on_batch = [](dftu_ctx* ctx, void* slice, const dftu_dataframe* df,
                         const dftu_plugin_host* host) noexcept -> dftu_status {
            return detail::to_status(ctx, [&]() {
                Slice* sl = static_cast<Slice*>(slice);
                Host h{host, ctx};
                if constexpr (detail::BatchViewStep<Slice>) {
                    return sl->step(Batch{df}, h);
                } else {
                    return sl->step(df, h);
                }
            });
        };
    }

    vt.merge = [](dftu_ctx* ctx, void* into,
                  void* other) noexcept -> dftu_status {
        return detail::to_status(ctx, [&]() {
            return static_cast<Slice*>(into)->merge(
                *static_cast<Slice*>(other));
        });
    };

    vt.on_finalize = [](dftu_ctx* ctx, void* slice,
                        const dftu_plugin_host* host,
                        dftu_task** pending) noexcept -> dftu_status {
        return detail::to_status(ctx, [&]() -> dftu_status {
            if constexpr (detail::AsyncFinalize<Slice>) {
                auto proxy = dstd::make_unique<detail::ProxyCtx>(ctx);
                Task t = static_cast<Slice*>(slice)->on_finalize(
                    Host{host, &proxy->base});
                return detail::drive_coro(ctx, host, std::move(t),
                                          std::move(proxy), pending);
            } else {
                return detail::to_status(ctx, [&]() {
                    return static_cast<Slice*>(slice)->finalize(
                        Host{host, ctx});
                });
            }
        });
    };

    vt.destroy_slice = [](void* slice) noexcept {
        delete static_cast<Slice*>(slice);
    };
    vt.destroy = [](void* self) noexcept {
        delete detail::holder_of<Slice>(self);
    };

    if constexpr (detail::DeclaresProvides<Slice>)
        vt.provides =
            &detail::name_list_thunk<Slice, detail::NameList::Provides>;
    if constexpr (detail::DeclaresConsumes<Slice>)
        vt.consumes =
            &detail::name_list_thunk<Slice, detail::NameList::Consumes>;
    if constexpr (detail::DeclaresReads<Slice>)
        vt.reads = &detail::name_list_thunk<Slice, detail::NameList::Reads>;
    if constexpr (detail::DeclaresConfigKeys<Slice>)
        vt.config_keys = &detail::config_key_thunk<Slice>;
    if constexpr (detail::MeasurableSlice<Slice>)
        vt.bytes = [](void* slice) noexcept -> std::uint64_t {
            return static_cast<const Slice*>(slice)->bytes();
        };
    if constexpr (detail::ReclaimableSlice<Slice>)
        vt.reclaim = [](void* slice,
                        std::uint64_t want) noexcept -> std::uint64_t {
            return static_cast<Slice*>(slice)->reclaim(want);
        };

    return &vt;
}

/// The factory body for a plugin that is one fold: sets `*out` to
/// make_plugin<Slice>(config), or fails the load with the reason.
///
///     extern "C" DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(
///         dftu_ctx* ctx, dftu_plugin_host*, const dftu_value* config,
///         dftu_plugin** out) {
///         return make_plugin<MySlice>(ctx, config, out);
///     }
template <class Slice>
dftu_status make_plugin(dftu_ctx* ctx, const dftu_value* config,
                        dftu_plugin** out) noexcept {
    return detail::to_status(ctx, [&]() -> dftu_status {
        *out = make_plugin<Slice>(config);
        return DFTU_OK;
    });
}

namespace detail {

/// A state type whose whole-scan answer is a native dataframe the host takes
/// ownership of; anything else must produce a byte string.
template <class State>
concept FrameFinalize = requires(State& s) {
    { s.finalize() } -> std::same_as<dftu_dataframe*>;
};

/// A state that can be written out and read back, which is what lets the host
/// spill it under the scan's memory budget.
template <class State>
concept Spillable = requires(const State& s, dstd::StringView b) {
    { s.serialize() } -> std::convertible_to<dstd::String>;
    { State::deserialize(b) } -> std::same_as<State*>;
};

/// A state that can say how big it is, which is the host's only measure of it
/// against the memory budget.
template <class State>
concept Measurable = requires(const State& s) {
    { s.bytes() } -> std::convertible_to<std::uint64_t>;
};

/// The fold a state-only plugin gets, so the host still has a slice to drive
/// its registered states through.
struct EmptySlice {
    explicit EmptySlice(const Config&) {}
    void step(const dftu_dataframe*, Host) {}
    void merge(EmptySlice&) {}
    void finalize(Host) {}
};

/// The state as the host holds it: the plugin's type plus the buffer a bytes
/// finalize or a serialize hands back, so the pointer the host reads outlives
/// the call that produced it.
template <class State>
struct StateBox {
    State state;
    dstd::String scratch;
};

/// The dftu_state_desc for `State`, filled from what State actually offers.
/// Static so the address of every callback outlives the registration.
template <class State>
const dftu_state_desc& state_desc_of(const char* name) {
    static dftu_state_desc desc = [name] {
        using Box = StateBox<State>;
        dftu_state_desc d{};
        d.struct_size = sizeof(dftu_state_desc);
        d.name = name;
        d.init = [](dftu_ctx* ctx, void*, void** out) noexcept -> dftu_status {
            return to_status(ctx, [&]() -> dftu_status {
                *out = new Box();
                return DFTU_OK;
            });
        };
        d.update = [](dftu_ctx* ctx, void* s,
                      const dftu_dataframe* df) noexcept -> dftu_status {
            return to_status(
                ctx, [&]() { return static_cast<Box*>(s)->state.update(df); });
        };
        d.merge = [](dftu_ctx* ctx, void* into,
                     void* other) noexcept -> dftu_status {
            return to_status(ctx, [&]() {
                return static_cast<Box*>(into)->state.merge(
                    static_cast<Box*>(other)->state);
            });
        };
        if constexpr (Measurable<State>)
            d.bytes = [](const void* s) noexcept -> std::uint64_t {
                return static_cast<const Box*>(s)->state.bytes();
            };
        if constexpr (Spillable<State>) {
            d.serialize = [](dftu_ctx* ctx, const void* s,
                             dftu_bytes* out) noexcept -> dftu_status {
                return to_status(ctx, [&]() -> dftu_status {
                    auto* buf = new dstd::String(
                        static_cast<const Box*>(s)->state.serialize());
                    out->data = buf->data();
                    out->len = buf->size();
                    out->ud = buf;
                    out->free_fn = [](void*, void* ud) {
                        delete static_cast<dstd::String*>(ud);
                    };
                    return DFTU_OK;
                });
            };
            d.deserialize = [](dftu_ctx* ctx, void*, dftu_bytes in,
                               void** out) noexcept -> dftu_status {
                return to_status(ctx, [&]() -> dftu_status {
                    auto box = dstd::make_unique<Box>();
                    dstd::UniquePtr<State> s{State::deserialize(
                        dstd::StringView{static_cast<const char*>(in.data),
                                         static_cast<std::size_t>(in.len)})};
                    if (!s)
                        return dftu_fail(ctx, DFTU_E_PARSE,
                                         "state deserialize returned no state");
                    box->state = std::move(*s);
                    *out = box.release();
                    return DFTU_OK;
                });
            };
        }
        d.finalize = [](dftu_ctx* ctx, void* s,
                        dftu_result_value* out) noexcept -> dftu_status {
            return to_status(ctx, [&]() -> dftu_status {
                Box& box = *static_cast<Box*>(s);
                if constexpr (FrameFinalize<State>) {
                    dftu_dataframe* frame = box.state.finalize();
                    if (!frame)
                        return dftu_fail(ctx, DFTU_E_INTERNAL,
                                         "state finalize returned no frame");
                    out->kind = DFTU_RESULT_KIND_FRAME;
                    out->u.frame = frame;
                } else {
                    box.scratch = box.state.finalize();
                    out->kind = DFTU_RESULT_KIND_BYTES;
                    out->u.bytes.data = box.scratch.data();
                    out->u.bytes.len = box.scratch.size();
                }
                return DFTU_OK;
            });
        };
        d.destroy = [](void* s) noexcept { delete static_cast<Box*>(s); };
        return d;
    }();
    return desc;
}

}  // namespace detail

/** Assemble a plugin from the host the factory was handed. A plugin is no
   longer just one fold: it may also register ops and state types, so the
   descriptor is built up rather than reflected off a single Slice type.

       return plugin(ctx, h, config).fold<MySlice>().state<MyGraph>("me.graph")
                                    .build(out);

   Every step is optional and the order does not matter. build() returns the
   first error any step hit, with its message on `ctx`, which fails the load.
   Unrelated to Plugins::builder(), which is how a caller LOADS plugins. */
class PluginBuilder {
   public:
    PluginBuilder(dftu_ctx* ctx, dftu_plugin_host* host,
                  const dftu_value* config)
        : ctx_(ctx), host_(host), config_(config) {}

    /// The fold half, exactly as make_plugin<Slice> builds it. At most one.
    template <class Slice>
    PluginBuilder& fold() {
        if (vt_) return fail(DFTU_E_INVALID, "a plugin has at most one fold");
        vt_ = make_plugin<Slice>(config_);
        return *this;
    }

    /// Register a dataframe op, which must be named `<plugin>.<name>`.
    PluginBuilder& op(const dftu_op_desc& desc) {
        const dftu_svc_ops* ops = ext<dftu_svc_ops>(DFTU_SVC_OPS);
        if (!ops || !ops->register_op) return fail("no op registry at load");
        return record(ops->register_op(ctx_, host_->h, &desc));
    }

    /// Register `src` as the @ref dftracer::utils::dataframe::LazyFrame
    /// "LazyFrame" source `name`, which must be
    /// `<plugin>.<name>`; see plugin/source.h for what S provides. The host
    /// owns it once registered and destroys it when the provider goes away.
    template <class S>
    PluginBuilder& source(const char* name, dstd::UniquePtr<S> src) {
        const dftu_svc_providers* p =
            ext<dftu_svc_providers>(DFTU_SVC_PROVIDERS);
        if (!p || !p->register_provider)
            return fail("no provider registry at load");
        const dftu_source_vt* vt = source_vtable<S>();
        void* self = make_source_self(std::move(src));
        const dftu_status st =
            p->register_provider(ctx_, host_->h, name, vt, self);
        if (st < 0) vt->destroy(self);
        return record(st);
    }

    /// Register `node` as the plan node `name`, which must be
    /// `<plugin>.<name>`; see plugin/node.h for what N provides. The host
    /// owns it once registered and unregisters it before the plugin unloads.
    template <class N>
    PluginBuilder& node(const char* name, dstd::UniquePtr<N> node) {
        const dftu_svc_nodes* p = ext<dftu_svc_nodes>(DFTU_SVC_NODES);
        if (!p || !p->register_node) return fail("no node registry at load");
        const dftu_node_vt* vt = node_vtable<N>();
        void* self = make_node_self(std::move(node), host_);
        const dftu_status st = p->register_node(ctx_, host_->h, name, vt, self);
        if (st < 0) vt->destroy(self);
        return record(st);
    }

    /// Register `State` as a mergeable state type under `name`, which must be
    /// `<plugin>.<name>` and outlive the plugin. State needs update(const
    /// dftu_dataframe*), merge(State&) and finalize(); it additionally gets a
    /// memory budget from bytes() and spilling from serialize() plus a static
    /// deserialize(dstd::StringView) -> State*.
    template <class State>
    PluginBuilder& state(const char* name) {
        const dftu_svc_agg* agg = ext<dftu_svc_agg>(DFTU_SVC_AGG);
        if (!agg || !agg->register_state)
            return fail("no state registry at load");
        return record(agg->register_state(
            ctx_, host_->h, &detail::state_desc_of<State>(name), nullptr));
    }

    /// Set `*out` to the finished descriptor and return DFTU_OK, or return
    /// the first error a step hit. A plugin with no fold still gets one: the
    /// host drives its registered states through it.
    dftu_status build(dftu_plugin** out) {
        if (status_ >= 0 && !vt_) fold<detail::EmptySlice>();
        if (status_ < 0) {
            if (vt_) vt_->destroy(vt_->self);
            vt_ = nullptr;
            return status_;
        }
        *out = vt_;
        return DFTU_OK;
    }

   private:
    template <class Ext>
    const Ext* ext(const char* id) const {
        if (!host_ || !host_->get_service) return nullptr;
        return static_cast<const Ext*>(host_->get_service(host_->h, id));
    }

    PluginBuilder& record(dftu_status st) {
        if (st < 0 && status_ >= 0) status_ = st;
        return *this;
    }

    PluginBuilder& fail(const char* why) {
        return fail(DFTU_E_UNSUPPORTED, why);
    }
    PluginBuilder& fail(dftu_status code, const char* why) {
        if (status_ >= 0) status_ = dftu_fail(ctx_, code, why);
        return *this;
    }

    dftu_ctx* ctx_;
    dftu_plugin_host* host_;
    const dftu_value* config_;
    dftu_plugin* vt_ = nullptr;
    dftu_status status_ = DFTU_OK;
};

/// Entry point for the builder; see @ref PluginBuilder.
inline PluginBuilder plugin(dftu_ctx* ctx, dftu_plugin_host* host,
                            const dftu_value* config) {
    return PluginBuilder{ctx, host, config};
}

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_REGISTER_H */
