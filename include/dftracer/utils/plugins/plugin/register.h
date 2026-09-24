#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_REGISTER_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_REGISTER_H

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
#include <exception>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    std::string_view as_str(std::string_view dflt = {}) const noexcept {
        std::uint32_t n = 0;
        const char* s = dftu_as_str(v_, &n);
        return s ? std::string_view{s, n} : dflt;
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
    ConfigValue operator[](std::string_view key) const noexcept {
        if (!is_object()) return ConfigValue{};
        for (std::uint32_t i = 0; i < v_->count; ++i) {
            const dftu_member& m = v_->as.members[i];
            if (std::string_view{m.key, m.key_len} == key)
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

    const dftu_value* find(std::string_view key) const {
        if (!root_ || root_->kind != DFTU_VAL_OBJECT) return nullptr;
        for (std::uint32_t i = 0; i < root_->count; ++i) {
            const dftu_member& m = root_->as.members[i];
            if (std::string_view{m.key, m.key_len} == key) return m.value;
        }
        return nullptr;
    }

    std::string_view get(std::string_view key,
                         std::string_view dflt = {}) const {
        const dftu_value* v = find(key);
        return (v && v->kind == DFTU_VAL_STR)
                   ? std::string_view{v->as.str, v->count}
                   : dflt;
    }
    std::int64_t get_int(std::string_view key, std::int64_t dflt = 0) const {
        return dftu_as_i64(find(key), dflt);
    }
    double get_double(std::string_view key, double dflt = 0.0) const {
        return dftu_as_f64(find(key), dflt);
    }
    bool get_bool(std::string_view key, bool dflt = false) const {
        return dftu_as_bool(find(key), dflt ? 1 : 0) != 0;
    }
    Config child(std::string_view key) const {
        const dftu_value* v = find(key);
        return Config{(v && v->kind == DFTU_VAL_OBJECT) ? v : nullptr};
    }

    /// Typed view of the value at `key`, of any kind (including ARRAY/OBJECT);
    /// null if absent. Prefer this over raw() for walking nested config.
    ConfigValue value(std::string_view key) const {
        return ConfigValue{find(key)};
    }
    /// Typed view of the ARRAY at `key`; null if absent or not an array.
    /// Iterate elements with ConfigValue::size()/operator[].
    ConfigValue array(std::string_view key) const {
        return ConfigValue{raw_array(key)};
    }
    /// The ARRAY at `key` coerced to int64 per element (dftu_as_i64 rules);
    /// empty if the key is absent or not an array.
    std::vector<std::int64_t> get_int_array(std::string_view key) const {
        std::vector<std::int64_t> out;
        if (const dftu_value* a = raw_array(key)) {
            out.reserve(a->count);
            for (std::uint32_t i = 0; i < a->count; ++i)
                out.push_back(dftu_as_i64(&a->as.items[i], 0));
        }
        return out;
    }
    /// The ARRAY at `key` coerced to double per element (dftu_as_f64 rules);
    /// empty if the key is absent or not an array.
    std::vector<double> get_double_array(std::string_view key) const {
        std::vector<double> out;
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
    std::vector<std::string_view> get_string_array(std::string_view key) const {
        std::vector<std::string_view> out;
        if (const dftu_value* a = raw_array(key)) {
            out.reserve(a->count);
            for (std::uint32_t i = 0; i < a->count; ++i) {
                const dftu_value& e = a->as.items[i];
                out.push_back(e.kind == DFTU_VAL_STR
                                  ? std::string_view{e.as.str, e.count}
                                  : std::string_view{});
            }
        }
        return out;
    }

    explicit operator bool() const { return root_ != nullptr; }
    const dftu_value* raw() const { return root_; }

   private:
    const dftu_value* raw_array(std::string_view key) const {
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
    std::string plan; /**< backs plan_query's const char* */
};

template <class Slice>
Holder<Slice>* holder_of(void* self) {
    return static_cast<Holder<Slice>*>(self);
}

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
/// returns a new dftu_dataframe (the host frees it) or nullptr for unchanged.
/// Takes the place of step().
template <class Slice>
concept TransformStep = requires(Slice& s, const dftu_dataframe* df, Host h) {
    { s.transform(df, h) } -> std::same_as<dftu_dataframe*>;
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

/// Resume the driven coroutine one step; NULL once it has run to completion.
inline dftu_task* step_thunk(void* coro) {
    auto h = std::coroutine_handle<Task::promise_type>::from_address(coro);
    h.resume();
    if (!h.done()) return h.promise().pending;
    if (h.promise().exc) {
        if (const dftu_plugin_host* host = h.promise().host) {
            try {
                std::rethrow_exception(h.promise().exc);
            } catch (const std::exception& e) {
                Host{host}.log(DFTU_LOG_ERROR, e.what());
            } catch (...) {
                Host{host}.log(DFTU_LOG_ERROR, "plugin coroutine threw");
            }
        }
    }
    h.destroy();
    return nullptr;
}

template <class Slice, class Coro>
dftu_task* drive_coro(const dftu_plugin_host* host, Coro&& coro) {
    Task t = std::forward<Coro>(coro);
    auto h = t.release();
    h.promise().host = host;
    const dftu_svc_coro* c =
        host->get_service ? static_cast<const dftu_svc_coro*>(
                                host->get_service(host->h, DFTU_SVC_CORO))
                          : nullptr;
    return c && c->drive ? c->drive(host->h, &step_thunk, h.address())
                         : nullptr;
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
    static const std::vector<const char*> names = [] {
        std::vector<const char*> v;
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
    return names.data();
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
// storage, plus the "query" key every Slice built here reads (make_plugin
// takes plan_query from it), so declaring keys does not make a caller's
// `query` an unknown one.
template <class Slice>
const dftu_config_key* config_key_thunk(void*) {
    static const std::vector<dftu_config_key> keys = [] {
        std::vector<dftu_config_key> v;
        for (const dftu_config_key& k : Slice::config_keys()) v.push_back(k);
        v.push_back({"query", DFTU_VAL_STR, 0,
                     "coarse DSL predicate the scan is pruned by"});
        v.push_back({nullptr, DFTU_CONFIG_ANY, 0, nullptr});
        return v;
    }();
    return keys.data();
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
   slice to the scan's memory budget. Exceptions must not escape the ABI
   boundary, so every callback catches. */
template <class Slice>
dftu_plugin* make_plugin(const dftu_value* config) {
    auto* hd = new detail::Holder<Slice>();
    hd->config = Config(config);
    hd->plan = std::string(hd->config.get("query"));

    dftu_plugin& vt = hd->vt;
    vt.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    vt.self = hd;

    vt.plan_query = [](void* self) -> const char* {
        auto* h = detail::holder_of<Slice>(self);
        return h->plan.empty() ? nullptr : h->plan.c_str();
    };

    vt.make_slice = [](void* self) -> void* {
        try {
            return new Slice(detail::holder_of<Slice>(self)->config);
        } catch (...) {
            return nullptr;
        }
    };

    // dftu_plugin::on_batch is synchronous only (must return NULL); the host
    // frees the dftu_dataframe right after the call.
    if constexpr (detail::TransformStep<Slice>) {
        vt.transform = [](void* slice, const dftu_dataframe* df,
                          const dftu_plugin_host* host) -> dftu_dataframe* {
            try {
                return static_cast<Slice*>(slice)->transform(df, Host{host});
            } catch (const std::exception& e) {
                Host{host}.log(DFTU_LOG_ERROR, e.what());
            } catch (...) {
                Host{host}.log(DFTU_LOG_ERROR, "plugin transform threw");
            }
            return nullptr;
        };
    } else {
        vt.on_batch = [](void* slice, const dftu_dataframe* df,
                         const dftu_plugin_host* host) -> dftu_task* {
            try {
                Slice* sl = static_cast<Slice*>(slice);
                Host h{host};
                if constexpr (detail::BatchViewStep<Slice>) {
                    sl->step(Batch{df}, h);
                } else {
                    sl->step(df, h);
                }
            } catch (const std::exception& e) {
                Host{host}.log(DFTU_LOG_ERROR, e.what());
            } catch (...) {
                Host{host}.log(DFTU_LOG_ERROR, "plugin step threw");
            }
            return nullptr;
        };
    }

    vt.merge = [](void* into, void* other) {
        try {
            static_cast<Slice*>(into)->merge(*static_cast<Slice*>(other));
        } catch (...) {
        }
    };

    vt.on_finalize = [](void* slice,
                        const dftu_plugin_host* host) -> dftu_task* {
        try {
            if constexpr (detail::AsyncFinalize<Slice>) {
                return detail::drive_coro<Slice>(
                    host, static_cast<Slice*>(slice)->on_finalize(Host{host}));
            } else {
                static_cast<Slice*>(slice)->finalize(Host{host});
            }
        } catch (const std::exception& e) {
            Host{host}.log(DFTU_LOG_ERROR, e.what());
        } catch (...) {
            Host{host}.log(DFTU_LOG_ERROR, "plugin finalize threw");
        }
        return nullptr;
    };

    vt.destroy_slice = [](void* slice) { delete static_cast<Slice*>(slice); };
    vt.destroy = [](void* self) { delete detail::holder_of<Slice>(self); };

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
        vt.bytes = [](void* slice) -> std::uint64_t {
            return static_cast<const Slice*>(slice)->bytes();
        };
    if constexpr (detail::ReclaimableSlice<Slice>)
        vt.reclaim = [](void* slice, std::uint64_t want) -> std::uint64_t {
            try {
                return static_cast<Slice*>(slice)->reclaim(want);
            } catch (...) {
                return 0;
            }
        };

    return &vt;
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
concept Spillable = requires(const State& s, std::string_view b) {
    { s.serialize() } -> std::convertible_to<std::string>;
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
    std::string scratch;
};

/// The dftu_state_desc for `State`, filled from what State actually offers.
/// Static so the address of every callback outlives the registration.
template <class State>
const dftu_state_desc& state_desc_of(const char* name) {
    static dftu_state_desc desc = [name] {
        using Box = StateBox<State>;
        dftu_state_desc d{};
        d.name = name;
        d.init = [](void*) -> void* {
            try {
                return new Box();
            } catch (...) {
                return nullptr;
            }
        };
        d.update = [](void* s, const dftu_dataframe* df, dftu_error*) -> int {
            try {
                static_cast<Box*>(s)->state.update(df);
                return 0;
            } catch (...) {
                return -1;
            }
        };
        d.merge = [](void* into, void* other, dftu_error*) -> int {
            try {
                static_cast<Box*>(into)->state.merge(
                    static_cast<Box*>(other)->state);
                return 0;
            } catch (...) {
                return -1;
            }
        };
        if constexpr (Measurable<State>)
            d.bytes = [](const void* s) -> std::uint64_t {
                return static_cast<const Box*>(s)->state.bytes();
            };
        if constexpr (Spillable<State>) {
            d.serialize = [](const void* s, dftu_bytes* out,
                             dftu_error*) -> int {
                try {
                    auto* buf = new std::string(
                        static_cast<const Box*>(s)->state.serialize());
                    out->data = buf->data();
                    out->len = buf->size();
                    out->ud = buf;
                    out->free_fn = [](void*, void* ud) {
                        delete static_cast<std::string*>(ud);
                    };
                    return 0;
                } catch (...) {
                    return -1;
                }
            };
            d.deserialize = [](void*, dftu_bytes in, dftu_error*) -> void* {
                try {
                    auto box = std::make_unique<Box>();
                    std::unique_ptr<State> s{State::deserialize(
                        std::string_view{static_cast<const char*>(in.data),
                                         static_cast<std::size_t>(in.len)})};
                    if (!s) return nullptr;
                    box->state = std::move(*s);
                    return box.release();
                } catch (...) {
                    return nullptr;
                }
            };
        }
        d.finalize = [](void* s, dftu_result_value* out, dftu_error*) -> int {
            try {
                Box& box = *static_cast<Box*>(s);
                if constexpr (FrameFinalize<State>) {
                    dftu_dataframe* frame = box.state.finalize();
                    if (!frame) return -1;
                    out->kind = DFTU_RESULT_KIND_FRAME;
                    out->u.frame = frame;
                } else {
                    box.scratch = box.state.finalize();
                    out->kind = DFTU_RESULT_KIND_BYTES;
                    out->u.bytes.data = box.scratch.data();
                    out->u.bytes.len = box.scratch.size();
                }
                return 0;
            } catch (...) {
                return -1;
            }
        };
        d.destroy = [](void* s) { delete static_cast<Box*>(s); };
        return d;
    }();
    return desc;
}

}  // namespace detail

/** Assemble a plugin from the host the factory was handed. A plugin is no
   longer just one fold: it may also register ops and state types, so the
   descriptor is built up rather than reflected off a single Slice type.

       return plugin(h, config).fold<MySlice>().state<MyGraph>("me.graph")
                               .build();

   Every step is optional and the order does not matter. build() returns NULL
   if any step failed, which fails the load. Unrelated to Plugins::builder(),
   which is how a caller LOADS plugins. */
class PluginBuilder {
   public:
    PluginBuilder(dftu_plugin_host* host, const dftu_value* config)
        : host_(host), config_(config) {}

    /// The fold half, exactly as make_plugin<Slice> builds it. At most one.
    template <class Slice>
    PluginBuilder& fold() {
        if (vt_) {
            fail("a plugin has at most one fold");
            return *this;
        }
        vt_ = make_plugin<Slice>(config_);
        if (!vt_) fail("fold construction failed");
        return *this;
    }

    /// Register a dataframe op, which must be named `<plugin>.<name>`.
    PluginBuilder& op(const dftu_op_desc& desc) {
        const dftu_svc_ops* ops = ext<dftu_svc_ops>(DFTU_SVC_OPS);
        if (!ops || !ops->register_op) return fail("no op registry at load");
        if (ops->register_op(host_->h, &desc) != 0)
            return fail("op registration refused");
        return *this;
    }

    /// Register `src` as the LazyFrame source `name`, which must be
    /// `<plugin>.<name>`; see plugin/source.h for what S provides. The host
    /// owns it once registered and destroys it when the provider goes away.
    template <class S>
    PluginBuilder& source(const char* name, std::unique_ptr<S> src) {
        const dftu_svc_providers* p =
            ext<dftu_svc_providers>(DFTU_SVC_PROVIDERS);
        if (!p || !p->register_provider)
            return fail("no provider registry at load");
        const dftu_source_vt* vt = source_vtable<S>();
        void* self = make_source_self(std::move(src));
        if (p->register_provider(host_->h, name, vt, self) != 0) {
            vt->destroy(self);
            return fail("provider registration refused");
        }
        return *this;
    }

    /// Register `node` as the plan node `name`, which must be
    /// `<plugin>.<name>`; see plugin/node.h for what N provides. The host
    /// owns it once registered and unregisters it before the plugin unloads.
    template <class N>
    PluginBuilder& node(const char* name, std::unique_ptr<N> node) {
        const dftu_svc_nodes* p = ext<dftu_svc_nodes>(DFTU_SVC_NODES);
        if (!p || !p->register_node) return fail("no node registry at load");
        const dftu_node_vt* vt = node_vtable<N>();
        void* self = make_node_self(std::move(node));
        if (p->register_node(host_->h, name, vt, self) != 0) {
            vt->destroy(self);
            return fail("node registration refused");
        }
        return *this;
    }

    /// Register `State` as a mergeable state type under `name`, which must be
    /// `<plugin>.<name>` and outlive the plugin. State needs update(const
    /// dftu_dataframe*), merge(State&) and finalize(); it additionally gets a
    /// memory budget from bytes() and spilling from serialize() plus a static
    /// deserialize(std::string_view) -> State*.
    template <class State>
    PluginBuilder& state(const char* name) {
        const dftu_svc_agg* agg = ext<dftu_svc_agg>(DFTU_SVC_AGG);
        if (!agg || !agg->register_state)
            return fail("no state registry at load");
        if (agg->register_state(host_->h, &detail::state_desc_of<State>(name),
                                nullptr) != 0)
            return fail("state registration refused");
        return *this;
    }

    /// The finished descriptor, or NULL if any step failed. A plugin with no
    /// fold still gets one: the host drives its registered states through it.
    dftu_plugin* build() {
        if (!ok_) return nullptr;
        if (!vt_) fold<detail::EmptySlice>();
        return vt_;
    }

   private:
    template <class Ext>
    const Ext* ext(const char* id) const {
        if (!host_ || !host_->get_service) return nullptr;
        return static_cast<const Ext*>(host_->get_service(host_->h, id));
    }

    PluginBuilder& fail(const char* why) {
        ok_ = false;
        if (host_ && host_->log)
            host_->log(host_->h, DFTU_LOG_ERROR, why,
                       static_cast<std::uint32_t>(std::strlen(why)));
        return *this;
    }

    dftu_plugin_host* host_;
    const dftu_value* config_;
    dftu_plugin* vt_ = nullptr;
    bool ok_ = true;
};

/// Entry point for the builder; see PluginBuilder.
inline PluginBuilder plugin(dftu_plugin_host* host, const dftu_value* config) {
    return PluginBuilder{host, config};
}

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_REGISTER_H */
