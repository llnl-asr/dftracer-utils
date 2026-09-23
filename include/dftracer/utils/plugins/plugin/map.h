#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_MAP_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_MAP_H

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/arrow_abi.h>
#include <dftracer/utils/plugins/owned_arrow.h>
#include <dftracer/utils/plugins/plugin/async.h>
#include <dftracer/utils/plugins/plugin/types.h>
#include <dftracer/utils/plugins/result_registry.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace dftracer::utils::plugins {

class Writer;
class Agg;
class Batch;
class Event;
template <class T>
class OutPort;
template <class T>
class InPort;

class Host {
   public:
    explicit Host(const dftu_plugin_host* h) : h_(h) {}

    std::string_view str(StrId id) const {
        if (id.absent()) return {};
        std::uint32_t n = 0;
        const char* p = h_->resolve(h_->h, id.raw(), &n);
        return p ? std::string_view{p, n} : std::string_view{};
    }
    StrId intern(std::string_view s) const {
        return StrId{
            h_->intern(h_->h, s.data(), static_cast<std::uint32_t>(s.size()))};
    }
    void log(dftu_log_level level, std::string_view msg) const {
        h_->log(h_->h, static_cast<std::uint8_t>(level), msg.data(),
                static_cast<std::uint32_t>(msg.size()));
    }
    void log(LogLevel level, std::string_view msg) const {
        log(static_cast<dftu_log_level>(level), msg);
    }

    Io io() const { return Io{h_}; }
    AsyncOp await(dftu_task* t) const { return AsyncOp{t}; }
    AsyncOp all(std::initializer_list<dftu_task*> ts) const {
        const dftu_svc_coro* c = ext(DFTU_SVC_CORO, coro_ext_);
        return AsyncOp{c && c->when_all
                           ? c->when_all(h_->h, ts.begin(),
                                         static_cast<std::uint32_t>(ts.size()))
                           : nullptr};
    }
    AsyncOp any(std::initializer_list<dftu_task*> ts) const {
        const dftu_svc_coro* c = ext(DFTU_SVC_CORO, coro_ext_);
        return AsyncOp{c && c->when_any
                           ? c->when_any(h_->h, ts.begin(),
                                         static_cast<std::uint32_t>(ts.size()))
                           : nullptr};
    }

    /// Spawn `fn(arg)` onto a pool worker and get an AsyncOp to co_await (or
    /// feed to all()/any()); `arg` is plugin-owned and must outlive the await.
    AsyncOp spawn(dftu_work_fn fn, void* arg) const {
        const dftu_svc_coro* c = ext(DFTU_SVC_CORO, coro_ext_);
        return AsyncOp{c && c->spawn ? c->spawn(h_->h, fn, arg) : nullptr};
    }
    /// Spawn any callable onto a pool worker; `fn` is borrowed for the await,
    /// so it must outlive it (a coroutine local naturally does).
    template <class Fn>
    AsyncOp spawn(Fn& fn) const {
        return spawn([](void* p) { (*static_cast<Fn*>(p))(); }, &fn);
    }
    /// Sequence: run `fn(arg)` after `t` completes; returns an AsyncOp to
    /// co_await. `arg` is plugin-owned and must outlive the await.
    AsyncOp then(dftu_task* t, dftu_work_fn fn, void* arg) const {
        const dftu_svc_coro* c = ext(DFTU_SVC_CORO, coro_ext_);
        return AsyncOp{c && c->then ? c->then(h_->h, t, fn, arg) : nullptr};
    }
    /// Sequence a callable after `t`; `fn` is borrowed for the await.
    template <class Fn>
    AsyncOp then(dftu_task* t, Fn& fn) const {
        return then(t, [](void* p) { (*static_cast<Fn*>(p))(); }, &fn);
    }

    /// Run a synchronous blocking call while releasing the worker's run-permit,
    /// so a raw block does not starve the elastic pool. Prefer the async task
    /// path for anything with an awaitable form; use this only when there is
    /// none.
    template <typename Fn>
    void run_blocking(Fn fn) const {
        const dftu_svc_coro* c = ext(DFTU_SVC_CORO, coro_ext_);
        if (c && c->run_blocking)
            c->run_blocking(
                h_->h, [](void* p) { (*static_cast<Fn*>(p))(); }, &fn);
        else
            fn();
    }

    /// Valid for the whole scan; null on parse error. Do not free.
    dftu_query* query_compile(std::string_view src) const {
        const dftu_svc_query* e = ext(DFTU_SVC_QUERY, query_ext_);
        return e && e->query_compile
                   ? e->query_compile(h_->h, src.data(),
                                      static_cast<std::uint32_t>(src.size()))
                   : nullptr;
    }

    /// Compile a predicate built with `F`/`Field`, e.g.
    /// `h.query_compile(F("dur") > 1000)`. Rendered to a DSL string and parsed
    /// host-side, so the plugin links nothing from the query library.
    dftu_query* query_compile(const Expr& e) const {
        return query_compile(e.to_string());
    }
    bool query_matches(const dftu_query* q, const dftu_dataframe* df,
                       std::int64_t row) const {
        const dftu_svc_query* qe = ext(DFTU_SVC_QUERY, query_ext_);
        return q && qe && qe->query_matches &&
               qe->query_matches(h_->h, q, df, row) != 0;
    }
    /// Compile and wrap as a non-owning Query bound to this host; see
    /// query_compile for lifetime.
    class Query compile(std::string_view src) const;
    /// compile() for a predicate built with `F`/`Field`.
    class Query compile(const Expr& e) const;

    /// Caller owns the handle and must sketch_free it.
    dftu_sketch* sketch_create() const {
        const dftu_svc_sketch* e = ext(DFTU_SVC_SKETCH, sketch_ext_);
        return e && e->sketch_create ? e->sketch_create(h_->h) : nullptr;
    }
    void sketch_add(dftu_sketch* s, double v, double w = 1.0) const {
        const dftu_svc_sketch* e = ext(DFTU_SVC_SKETCH, sketch_ext_);
        if (e && e->sketch_add) e->sketch_add(h_->h, s, v, w);
    }
    void sketch_merge(dftu_sketch* into, const dftu_sketch* other) const {
        const dftu_svc_sketch* e = ext(DFTU_SVC_SKETCH, sketch_ext_);
        if (e && e->sketch_merge) e->sketch_merge(h_->h, into, other);
    }
    dftu_quantiles sketch_result(const dftu_sketch* s) const {
        dftu_quantiles q{};
        const dftu_svc_sketch* e = ext(DFTU_SVC_SKETCH, sketch_ext_);
        return e && e->sketch_result ? e->sketch_result(h_->h, s) : q;
    }
    void sketch_free(dftu_sketch* s) const {
        const dftu_svc_sketch* e = ext(DFTU_SVC_SKETCH, sketch_ext_);
        if (e && e->sketch_free) e->sketch_free(h_->h, s);
    }
    /// RAII sketch bound to this host; frees the handle in its destructor.
    class Sketch make_sketch() const;

    int arrow_write_ipc(ArrowArray* a, ArrowSchema* s, const char* path) const {
        const dftu_svc_arrow* e = ext(DFTU_SVC_ARROW, arrow_ext_);
        return e && e->arrow_write_ipc ? e->arrow_write_ipc(h_->h, a, s, path)
                                       : -1;
    }
    /// Read the first record batch of an IPC file; the caller owns *out and
    /// *out_schema and must call their release. 0 on success.
    int arrow_read_ipc(const char* path, ArrowArray* out,
                       ArrowSchema* out_schema) const {
        const dftu_svc_arrow* e = ext(DFTU_SVC_ARROW, arrow_ext_);
        return e && e->arrow_read_ipc
                   ? e->arrow_read_ipc(h_->h, path, out, out_schema)
                   : -1;
    }
    /// arrow_write_ipc for an already-owning batch.
    int arrow_write_ipc(OwnedArrow& a, const char* path) const {
        return arrow_write_ipc(&a.array, &a.schema, path);
    }
    /// Owning form of arrow_read_ipc; an empty result (`!owned`) signals the
    /// host lacks the Arrow group or the read failed.
    OwnedArrow arrow_read_ipc(const char* path) const {
        OwnedArrow owned;
        arrow_read_ipc(path, &owned.array, &owned.schema);
        return owned;
    }
    dftu_trace_writer* trace_open_write(const char* path) const {
        const dftu_svc_trace* e = ext(DFTU_SVC_TRACE, trace_ext_);
        return e && e->trace_open_write ? e->trace_open_write(h_->h, path)
                                        : nullptr;
    }
    /// Append every row of `df` as a trace event.
    int trace_write(dftu_trace_writer* w, const dftu_dataframe* df) const {
        const dftu_svc_trace* e = ext(DFTU_SVC_TRACE, trace_ext_);
        return e && e->trace_write ? e->trace_write(h_->h, w, df) : -1;
    }
    int trace_close(dftu_trace_writer* w) const {
        const dftu_svc_trace* e = ext(DFTU_SVC_TRACE, trace_ext_);
        return e && e->trace_close ? e->trace_close(h_->h, w) : -1;
    }
    /// Scan `path` (auto-indexed) and call on_batch once per scanned batch with
    /// a dftu_dataframe*. 0 on success.
    int trace_read(const char* path, dftu_stream_item_fn on_batch,
                   void* ud) const {
        const dftu_svc_trace* e = ext(DFTU_SVC_TRACE, trace_ext_);
        return e && e->trace_read ? e->trace_read(h_->h, path, on_batch, ud)
                                  : -1;
    }
    /// Scan `path` (auto-indexed), invoking `fn` once per scanned batch as
    /// either `fn(const Batch&)` or `fn(const dftu_dataframe*)`. `fn` is
    /// borrowed for the call. 0 on success.
    template <class Fn>
    int trace_read(const char* path, Fn&& fn) const;

    /// RAII trace writer for `path` (a gzip .pfw.gz); empty (`!writer`) if the
    /// host lacks the trace group.
    class TraceWriter trace_writer(const char* path) const;

    /// Get a host-owned parallel Writer for `path` with `num_workers` lanes
    /// (`gzip` != 0 compresses). The Writer is host-owned and scan-lifetime;
    /// open it, write chunks per worker, then close it - all co_await forms. An
    /// empty result (`!writer`) means the host lacks the writer group.
    Writer writer(const char* path, std::uint32_t num_workers,
                  bool gzip = false) const;
    /// Merge worker shard files into `target`; co_await the returned AsyncOp.
    AsyncOp merge_shards(const char* target,
                         std::initializer_list<const char*> shards) const {
        const dftu_svc_writer* e = ext(DFTU_SVC_WRITER, writer_ext_);
        return AsyncOp{
            e && e->merge_shards
                ? e->merge_shards(h_->h, target, shards.begin(),
                                  static_cast<std::uint32_t>(shards.size()))
                : nullptr};
    }

    /// Batch-scoped ports: a producer publishes a derived value a later
    /// consumer reads for the same batch. consume returns NULL (and 0 len) when
    /// the producer has not published this batch; the borrow is valid only
    /// until the current on_batch returns.
    std::uint64_t port_key(const char* name) const {
        const dftu_svc_ports* e = ext(DFTU_SVC_PORTS, ports_ext_);
        return e && e->port_key ? e->port_key(h_->h, name) : 0;
    }
    void publish(std::uint64_t key, const void* data, std::uint32_t len) const {
        const dftu_svc_ports* e = ext(DFTU_SVC_PORTS, ports_ext_);
        if (e && e->publish) e->publish(h_->h, key, data, len);
    }
    const void* consume(std::uint64_t key, std::uint32_t* out_len) const {
        const dftu_svc_ports* e = ext(DFTU_SVC_PORTS, ports_ext_);
        return e && e->consume ? e->consume(h_->h, key, out_len) : nullptr;
    }
    dftu_agg* agg_new(const char* name, const char* const* key_names,
                      std::uint32_t key_n, const dftu_agg_col* specs,
                      std::uint32_t spec_n) const {
        const dftu_svc_agg* e = ext(DFTU_SVC_AGG, agg_ext_);
        return e && e->agg_new
                   ? e->agg_new(h_->h, name, key_names, key_n, specs, spec_n)
                   : nullptr;
    }
    void agg_accumulate(dftu_agg* a, const dftu_dataframe* df) const {
        const dftu_svc_agg* e = ext(DFTU_SVC_AGG, agg_ext_);
        if (e && e->agg_accumulate) e->agg_accumulate(h_->h, a, df);
    }
    /// Get-or-create a named cross-batch aggregation accumulator grouping by
    /// `key_names` and computing each of `cols`; empty (`!agg`) if the host
    /// lacks the agg group or agg_new rejects the spec (a bad op code or
    /// missing output name). See dftu_svc_agg::agg_new; a name seen before
    /// returns the existing accumulator and ignores `cols`.
    Agg agg(const char* name, std::initializer_list<const char*> key_names,
            std::initializer_list<AggCol> cols) const;

    /// The cross-worker-merged, finalized result of the accumulator named
    /// `name` (any plugin's), as an owned frame. Call at on_finalize; the
    /// producing plugin must be registered first. Empty (`!frame.handle`) when
    /// no plugin produced that name.
    OwnedDataFrame agg_result(const char* name) const;

    /// Look up and run a registered dataframe column op by name on `in` (see
    /// dftu_svc_ops::run); NULL if the host lacks the ops group, `name` is
    /// unregistered, or the call mismatches the op's kind/arity/shape. The
    /// result is newly owned by the caller (free with dftu_series_free).
    dftu_series* run_op(const char* name,
                        std::initializer_list<const dftu_series*> in,
                        const dftu_op_arg* args = nullptr) const {
        const dftu_svc_ops* e = ext(DFTU_SVC_OPS, ops_ext_);
        if (!e || !e->run) return nullptr;
        std::vector<const dftu_series*> vin(in);
        dftu_result_series res =
            e->run(h_->h, name, vin.data(),
                   static_cast<std::uint32_t>(vin.size()), args);
        return DFTU_RESULT_OK(res) ? DFTU_RESULT_VALUE(res) : nullptr;
    }
    /// run_op for a signature whose return token is FRAME; the result is
    /// newly owned by the caller (free with dftu_dataframe_free).
    dftu_dataframe* run_op_frame(
        const char* name, std::initializer_list<const dftu_dataframe*> in,
        const dftu_op_arg* args = nullptr) const {
        const dftu_svc_ops* e = ext(DFTU_SVC_OPS, ops_ext_);
        if (!e || !e->run_frame) return nullptr;
        std::vector<const dftu_dataframe*> vin(in);
        dftu_result_frame res =
            e->run_frame(h_->h, name, vin.data(),
                         static_cast<std::uint32_t>(vin.size()), args);
        return DFTU_RESULT_OK(res) ? DFTU_RESULT_VALUE(res) : nullptr;
    }
    /// run_op for a signature whose return token is SCALAR/I64/BOOL, reducing
    /// one column; `ok` (if given) is set false on a NULL/kind/shape mismatch
    /// or unregistered name.
    dftu_scalar run_op_aggregate(const char* name, const dftu_series* in,
                                 const dftu_op_arg* args = nullptr,
                                 bool* ok = nullptr) const {
        const dftu_svc_ops* e = ext(DFTU_SVC_OPS, ops_ext_);
        if (!e || !e->run_aggregate) {
            if (ok) *ok = false;
            return dftu_scalar{};
        }
        dftu_result_scalar r = e->run_aggregate(h_->h, name, &in, 1, args);
        if (ok) *ok = DFTU_RESULT_OK(r);
        return DFTU_RESULT_OK(r) ? DFTU_RESULT_VALUE(r) : dftu_scalar{};
    }
    /// The registered op named `name` (built-in or user), or NULL if none or
    /// the host lacks the ops group. See dftu_op_find.
    const dftu_op_desc* find_op(const char* name) const {
        const dftu_svc_ops* e = ext(DFTU_SVC_OPS, ops_ext_);
        return e && e->find ? e->find(h_->h, name) : nullptr;
    }
    /// Register a user op in the host's shared registry; see dftu_op_register.
    /// The op must be named `<plugin>.<name>`: the bare and "dftu." namespaces
    /// are the host's. Returns non-zero on a NULL desc/name, a name the host
    /// keeps, an already-registered name, or if the host lacks the ops group.
    int register_op(const dftu_op_desc* desc) const {
        const dftu_svc_ops* e = ext(DFTU_SVC_OPS, ops_ext_);
        return e && e->register_op ? e->register_op(h_->h, desc) : -1;
    }

    /// Named result channel: emit an opaque blob; the host copies `len` bytes.
    /// Surfaces from Plugins::run keyed by name. Best called at on_finalize.
    void emit_result(const char* name, const void* data,
                     std::uint64_t len) const {
        dftu_result_value v{};
        v.kind = DFTU_RESULT_KIND_BYTES;
        v.u.bytes.data = data;
        v.u.bytes.len = len;
        emit(name, &v);
    }
    void emit_result(const char* name, std::string_view data) const {
        emit_result(name, data.data(), data.size());
    }
    /// Moves *a and *s into the host; -1 if the host lacks the result channel.
    int emit_result_arrow(const char* name, ArrowArray* a,
                          ArrowSchema* s) const {
        dftu_result_value v{};
        v.kind = DFTU_RESULT_KIND_ARROW;
        v.u.arrow.array = a;
        v.u.arrow.schema = s;
        return emit(name, &v);
    }
    /// Hands a native dataframe result to the host, which takes ownership; -1
    /// if the host lacks the result channel (`df` is left intact so it frees
    /// normally). Same finalize/collision semantics as emit_result.
    int emit_result_frame(const char* name, OwnedDataFrame&& df) const {
        dftu_result_value v{};
        v.kind = DFTU_RESULT_KIND_FRAME;
        v.u.frame = df.handle;
        int rc = emit(name, &v);
        if (rc == 0) df.release();
        return rc;
    }
    /// Hands a deferred plan to the host, which takes ownership and collects it
    /// at the Python edge; -1 if the host lacks the result channel. The plan
    /// must be self-contained (see the emit's DFTU_RESULT_KIND_LAZYFRAME
    /// contract in abi.h).
    int emit_result_lazyframe(const char* name, dftu_lazyframe* lf) const {
        dftu_result_value v{};
        v.kind = DFTU_RESULT_KIND_LAZYFRAME;
        v.u.lazyframe = lf;
        return emit(name, &v);
    }
    /// OwnedLazyFrame overload of emit_result_lazyframe; -1 leaves `lf` intact
    /// so it frees normally.
    int emit_result_lazyframe(const char* name, OwnedLazyFrame&& lf) const {
        int rc = emit_result_lazyframe(name, lf.handle);
        if (rc == 0) lf.release();
        return rc;
    }

    /// Typed batch-scoped output port: send() publishes a trivially-copyable T
    /// under `name` for a same-batch consumer. See publish/consume.
    template <class T>
    OutPort<T> publish_port(const char* name) const;
    /// Typed batch-scoped input port: recv() reads the T a producer published
    /// under `name` this batch, or nullopt if none. See publish/consume.
    template <class T>
    InPort<T> consume_port(const char* name) const;

    const dftu_plugin_host* raw() const { return h_; }

   private:
    /// Fetch a host-service group once and memoize it in `slot`; a missing
    /// group stays null so every wrapper falls back exactly as a null
    /// fn-pointer did.
    template <class T>
    const T* ext(const char* id, const T*& slot) const {
        if (!slot && h_->get_service)
            slot = static_cast<const T*>(h_->get_service(h_->h, id));
        return slot;
    }

    int emit(const char* name, dftu_result_value* v) const {
        const dftu_svc_result* e = ext(DFTU_SVC_RESULT, result_ext_);
        return e && e->emit ? e->emit(h_->h, name, v) : -1;
    }

    const dftu_plugin_host* h_;
    mutable const dftu_svc_coro* coro_ext_ = nullptr;
    mutable const dftu_svc_query* query_ext_ = nullptr;
    mutable const dftu_svc_writer* writer_ext_ = nullptr;
    mutable const dftu_svc_sketch* sketch_ext_ = nullptr;
    mutable const dftu_svc_arrow* arrow_ext_ = nullptr;
    mutable const dftu_svc_trace* trace_ext_ = nullptr;
    mutable const dftu_svc_ports* ports_ext_ = nullptr;
    mutable const dftu_svc_result* result_ext_ = nullptr;
    mutable const dftu_svc_agg* agg_ext_ = nullptr;
    mutable const dftu_svc_ops* ops_ext_ = nullptr;
};

/// Move-only RAII owner of a host sketch handle; frees it in the destructor.
/// Construct from a Host (`Sketch s{host};`) or via Host::make_sketch().
class Sketch {
   public:
    explicit Sketch(Host host) : host_(host), s_(host_.sketch_create()) {}
    Sketch(Sketch&& o) noexcept : host_(o.host_), s_(o.s_) { o.s_ = nullptr; }
    Sketch& operator=(Sketch&& o) noexcept {
        if (this != &o) {
            reset();
            host_ = o.host_;
            s_ = o.s_;
            o.s_ = nullptr;
        }
        return *this;
    }
    Sketch(const Sketch&) = delete;
    Sketch& operator=(const Sketch&) = delete;
    ~Sketch() { reset(); }

    void add(double value, double weight = 1.0) const {
        host_.sketch_add(s_, value, weight);
    }
    void merge(const Sketch& other) const { host_.sketch_merge(s_, other.s_); }
    dftu_quantiles result() const { return host_.sketch_result(s_); }

    /// Borrowed handle; the Sketch retains ownership. Null if unsupported.
    dftu_sketch* raw() const noexcept { return s_; }
    explicit operator bool() const noexcept { return s_ != nullptr; }

   private:
    void reset() {
        if (s_) host_.sketch_free(s_);
        s_ = nullptr;
    }
    Host host_;
    dftu_sketch* s_ = nullptr;
};

inline Sketch Host::make_sketch() const { return Sketch{*this}; }

/// Non-owning view over a host-owned cross-batch aggregation accumulator (see
/// Host::agg). The host owns the handle (freed at fold teardown), so there is
/// nothing for the plugin to free.
class Agg {
   public:
    Agg() = default;

    /// Fold one batch's key/value columns (looked up by name in `df`) into
    /// this accumulator. Serial per accumulator; one slice's accumulator is
    /// touched by one thread.
    void accumulate(const dftu_dataframe* df) const {
        host_.agg_accumulate(a_, df);
    }
    void accumulate(const OwnedDataFrame& df) const { accumulate(df.handle); }

    /// Borrowed handle; the host retains ownership. Null if unsupported.
    dftu_agg* raw() const noexcept { return a_; }
    explicit operator bool() const noexcept { return a_ != nullptr; }

   private:
    friend class Host;
    Agg(Host host, dftu_agg* a) noexcept : host_(host), a_(a) {}

    Host host_{nullptr};
    dftu_agg* a_ = nullptr;
};

inline Agg Host::agg(const char* name,
                     std::initializer_list<const char*> key_names,
                     std::initializer_list<AggCol> cols) const {
    std::vector<const char*> keys(key_names.begin(), key_names.end());
    std::vector<dftu_agg_col> specs;
    specs.reserve(cols.size());
    for (const AggCol& c : cols) specs.push_back(c.raw());
    dftu_agg* a =
        agg_new(name, keys.data(), static_cast<std::uint32_t>(keys.size()),
                specs.data(), static_cast<std::uint32_t>(specs.size()));
    return Agg{*this, a};
}

inline OwnedDataFrame Host::agg_result(const char* name) const {
    const dftu_svc_agg* e = ext(DFTU_SVC_AGG, agg_ext_);
    return OwnedDataFrame{e && e->agg_result ? e->agg_result(h_->h, name)
                                             : nullptr};
}

/// Typed, non-owning view over a host-owned parallel Writer (see Host::writer).
/// The writer is host-owned and scan-lifetime, so there is nothing to free; the
/// lifecycle is open() once, chunk() per worker lane, then close(), each a
/// co_await-able AsyncOp. Worker lanes write concurrently, so chunks for
/// distinct `worker` indices may be in flight at once.
class Writer {
   public:
    Writer() = default;

    /// Open the underlying output; co_await before writing any chunk.
    AsyncOp open() const {
        return AsyncOp{e_ && e_->writer_open && w_ ? e_->writer_open(h(), w_)
                                                   : nullptr};
    }
    /// Append `len` bytes to lane `worker`; co_await to complete the write.
    AsyncOp chunk(std::uint32_t worker, const void* data,
                  std::uint64_t len) const {
        return AsyncOp{e_ && e_->writer_chunk && w_
                           ? e_->writer_chunk(h(), w_, worker, data, len)
                           : nullptr};
    }
    AsyncOp chunk(std::uint32_t worker, std::string_view data) const {
        return chunk(worker, data.data(), data.size());
    }
    /// Flush and finalize; co_await after all chunks are written.
    AsyncOp close() const {
        return AsyncOp{e_ && e_->writer_close && w_ ? e_->writer_close(h(), w_)
                                                    : nullptr};
    }

    /// Borrowed handle; the host retains ownership. Null if unsupported.
    dftu_writer* raw() const noexcept { return w_; }
    explicit operator bool() const noexcept { return w_ != nullptr; }

   private:
    friend class Host;
    Writer(Host host, const dftu_svc_writer* e, dftu_writer* w)
        : host_(host), e_(e), w_(w) {}
    void* h() const noexcept { return host_.raw()->h; }

    Host host_{nullptr};
    const dftu_svc_writer* e_ = nullptr;
    dftu_writer* w_ = nullptr;
};

inline Writer Host::writer(const char* path, std::uint32_t num_workers,
                           bool gzip) const {
    const dftu_svc_writer* e = ext(DFTU_SVC_WRITER, writer_ext_);
    dftu_writer* w =
        e && e->writer_create
            ? e->writer_create(h_->h, path, num_workers, gzip ? 1 : 0)
            : nullptr;
    return Writer{*this, e, w};
}

/// Typed batch-scoped output port (see Host::publish_port). send() publishes a
/// trivially-copyable T for a same-batch consumer under the resolved port key.
template <class T>
class OutPort {
    static_assert(std::is_trivially_copyable_v<T>,
                  "a typed port carries a trivially-copyable value");

   public:
    OutPort() = default;

    /// Publish `v` for this batch; the host copies sizeof(T) bytes.
    void send(const T& v) const {
        if (key_ != 0) host_.publish(key_, &v, sizeof(T));
    }

    std::uint64_t key() const noexcept { return key_; }
    explicit operator bool() const noexcept { return key_ != 0; }

   private:
    friend class Host;
    OutPort(Host host, std::uint64_t key) : host_(host), key_(key) {}
    Host host_{nullptr};
    std::uint64_t key_ = 0;
};

/// Typed batch-scoped input port (see Host::consume_port). recv() reads the T a
/// producer published this batch under the resolved port key.
template <class T>
class InPort {
    static_assert(std::is_trivially_copyable_v<T>,
                  "a typed port carries a trivially-copyable value");

   public:
    InPort() = default;

    /// The value a producer published this batch, or nullopt if none was
    /// published or its length did not match sizeof(T). The read is a copy, so
    /// the result outlives the borrowed port buffer.
    std::optional<T> recv() const {
        if (key_ == 0) return std::nullopt;
        std::uint32_t len = 0;
        const void* p = host_.consume(key_, &len);
        if (!p || len != sizeof(T)) return std::nullopt;
        T out;
        std::memcpy(&out, p, sizeof(T));
        return out;
    }

    std::uint64_t key() const noexcept { return key_; }
    explicit operator bool() const noexcept { return key_ != 0; }

   private:
    friend class Host;
    InPort(Host host, std::uint64_t key) : host_(host), key_(key) {}
    Host host_{nullptr};
    std::uint64_t key_ = 0;
};

template <class T>
inline OutPort<T> Host::publish_port(const char* name) const {
    return OutPort<T>{*this, port_key(name)};
}
template <class T>
inline InPort<T> Host::consume_port(const char* name) const {
    return InPort<T>{*this, port_key(name)};
}

namespace detail {

/// A resolved column, cached once per Batch. `type` is -1 when the column is
/// absent from the frame; every accessor treats an absent column as always
/// null/zero rather than re-resolving it.
struct ColBuf {
    dftu_series* handle = nullptr;
    std::int32_t type = -1;                 // dftu_dtype
    const void* data = nullptr;
    const std::int32_t* offsets = nullptr;  // String/Binary only
};

}  // namespace detail

/// Non-owning typed view over one row of a dftu_dataframe batch: a batch
/// pointer plus a row index. Zero-copy and cheap to copy; valid only for the
/// on_batch call that delivered the batch. Every fixed-column accessor reads a
/// pointer the owning Batch resolved once at construction, so a `for (Event e
/// : batch)` loop touches no column-name lookup and allocates nothing.
class Event {
   public:
    Event() = default;
    Event(const Batch* b, std::int64_t row) noexcept : b_(b), row_(row) {}

    // Forward-iterator interface so Batch::begin()/end() can both return
    // Event and a range-for works directly over it.
    const Event& operator*() const noexcept { return *this; }
    Event& operator++() noexcept {
        ++row_;
        return *this;
    }
    bool operator==(const Event& o) const noexcept { return row_ == o.row_; }
    bool operator!=(const Event& o) const noexcept { return row_ != o.row_; }

    std::int64_t row() const noexcept { return row_; }
    const dftu_dataframe* frame() const noexcept;

    std::uint64_t pid() const noexcept;
    std::uint64_t tid() const noexcept;
    std::uint64_t ts() const noexcept;
    std::uint64_t dur() const noexcept;
    /// Whether the event carries a meaningful duration; the row-fold engine
    /// does not track a per-row null for `dur`, so this is phase() ==
    /// Complete rather than a true null check.
    bool has_dur() const noexcept;
    Phase phase() const noexcept;

    bool has_cat() const noexcept;
    bool has_name() const noexcept;
    bool has_fhash() const noexcept;
    bool has_hhash() const noexcept;

    /// Zero-copy views into the frame's string buffers; empty when absent.
    std::string_view cat() const noexcept;
    std::string_view name() const noexcept;
    std::string_view fhash() const noexcept;
    std::string_view hhash() const noexcept;

    /// A dyn (per-key) arg column, present when any event in the batch carried
    /// that flattened dotted key ("args.ret", or the bare key "ret").
    bool has_arg(std::string_view key) const noexcept;
    bool arg_is_i64(std::string_view key) const noexcept;
    bool arg_is_f64(std::string_view key) const noexcept;
    bool arg_is_str(std::string_view key) const noexcept;
    std::int64_t arg_i64(std::string_view key) const noexcept;
    double arg_f64(std::string_view key) const noexcept;
    std::string_view arg_str(std::string_view key) const noexcept;

   private:
    const Batch* b_ = nullptr;
    std::int64_t row_ = 0;
};

/// Non-owning, zero-copy cursor over a dftu_dataframe batch: resolves the
/// fixed columns (cat/name/fhash/hhash/pid/tid/ts/dur/ph) and every dyn arg
/// column once, in the constructor, so a `for (Event e : batch)` loop is a
/// direct memory read per field with no per-row column-name lookup. Valid only
/// for the on_batch call that delivered `df`.
class Batch {
   public:
    explicit Batch(const dftu_dataframe* df) noexcept;
    ~Batch();
    Batch(Batch&&) noexcept;
    Batch& operator=(Batch&&) noexcept;
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

    std::int64_t size() const noexcept { return n_; }
    bool empty() const noexcept { return n_ == 0; }

    Event begin() const noexcept { return Event{this, 0}; }
    Event end() const noexcept { return Event{this, n_}; }
    Event operator[](std::int64_t i) const noexcept { return Event{this, i}; }

    const dftu_dataframe* raw() const noexcept { return df_; }

   private:
    friend class Event;

    static detail::ColBuf resolve(const dftu_dataframe* df, const char* name);
    static bool is_str(const detail::ColBuf& c) noexcept {
        return c.type == DFTU_TYPE_STRING;
    }
    static std::string_view str_at(const detail::ColBuf& c,
                                   std::int64_t row) noexcept {
        if (!c.data || !c.offsets || dftu_series_is_null(c.handle, row))
            return {};
        const char* base = static_cast<const char*>(c.data);
        return {base + c.offsets[row],
                static_cast<std::size_t>(c.offsets[row + 1] - c.offsets[row])};
    }
    const detail::ColBuf* find_dyn(std::string_view key) const noexcept;

    void reset() noexcept;

    const dftu_dataframe* df_ = nullptr;
    std::int64_t n_ = 0;
    detail::ColBuf cat_, name_, fhash_, hhash_, pid_, tid_, ts_, dur_, ph_;
    std::vector<std::pair<std::string, detail::ColBuf>> dyn_;
};

inline const dftu_dataframe* Event::frame() const noexcept { return b_->raw(); }
inline std::uint64_t Event::pid() const noexcept {
    return b_->pid_.data
               ? static_cast<const std::uint64_t*>(b_->pid_.data)[row_]
               : 0;
}
inline std::uint64_t Event::tid() const noexcept {
    return b_->tid_.data
               ? static_cast<const std::uint64_t*>(b_->tid_.data)[row_]
               : 0;
}
inline std::uint64_t Event::ts() const noexcept {
    return b_->ts_.data ? static_cast<const std::uint64_t*>(b_->ts_.data)[row_]
                        : 0;
}
inline std::uint64_t Event::dur() const noexcept {
    return b_->dur_.data
               ? static_cast<const std::uint64_t*>(b_->dur_.data)[row_]
               : 0;
}
inline Phase Event::phase() const noexcept {
    if (!b_->ph_.data) return Phase::Unknown;
    return static_cast<Phase>(static_cast<dftu_phase>(
        static_cast<const std::int64_t*>(b_->ph_.data)[row_]));
}
inline bool Event::has_dur() const noexcept {
    return phase() == Phase::Complete;
}
inline bool Event::has_cat() const noexcept { return b_->cat_.data != nullptr; }
inline bool Event::has_name() const noexcept {
    return b_->name_.data != nullptr;
}
inline bool Event::has_fhash() const noexcept {
    return b_->fhash_.data != nullptr;
}
inline bool Event::has_hhash() const noexcept {
    return b_->hhash_.data != nullptr;
}
inline std::string_view Event::cat() const noexcept {
    return Batch::str_at(b_->cat_, row_);
}
inline std::string_view Event::name() const noexcept {
    return Batch::str_at(b_->name_, row_);
}
inline std::string_view Event::fhash() const noexcept {
    return Batch::str_at(b_->fhash_, row_);
}
inline std::string_view Event::hhash() const noexcept {
    return Batch::str_at(b_->hhash_, row_);
}
inline bool Event::has_arg(std::string_view key) const noexcept {
    return b_->find_dyn(key) != nullptr;
}
inline bool Event::arg_is_i64(std::string_view key) const noexcept {
    const detail::ColBuf* c = b_->find_dyn(key);
    return c && c->type == DFTU_TYPE_INT64;
}
inline bool Event::arg_is_f64(std::string_view key) const noexcept {
    const detail::ColBuf* c = b_->find_dyn(key);
    return c && c->type == DFTU_TYPE_FLOAT64;
}
inline bool Event::arg_is_str(std::string_view key) const noexcept {
    const detail::ColBuf* c = b_->find_dyn(key);
    return c && c->type == DFTU_TYPE_STRING;
}
inline std::int64_t Event::arg_i64(std::string_view key) const noexcept {
    const detail::ColBuf* c = b_->find_dyn(key);
    if (!c || c->type != DFTU_TYPE_INT64 || !c->data ||
        dftu_series_is_null(c->handle, row_))
        return 0;
    return static_cast<const std::int64_t*>(c->data)[row_];
}
inline double Event::arg_f64(std::string_view key) const noexcept {
    const detail::ColBuf* c = b_->find_dyn(key);
    if (!c || c->type != DFTU_TYPE_FLOAT64 || !c->data ||
        dftu_series_is_null(c->handle, row_))
        return 0.0;
    return static_cast<const double*>(c->data)[row_];
}
inline std::string_view Event::arg_str(std::string_view key) const noexcept {
    const detail::ColBuf* c = b_->find_dyn(key);
    return c ? Batch::str_at(*c, row_) : std::string_view{};
}

inline detail::ColBuf Batch::resolve(const dftu_dataframe* df,
                                     const char* name) {
    detail::ColBuf c;
    dftu_series* col = dftu_dataframe_column(df, name);
    if (!col) return c;
    c.handle = col;
    c.type = dftu_series_type(col);
    c.data = dftu_series_data(col);
    c.offsets = dftu_series_offsets(col);
    return c;
}

inline Batch::Batch(const dftu_dataframe* df) noexcept
    : df_(df), n_(df ? dftu_dataframe_num_rows(df) : 0) {
    if (!df_) return;
    cat_ = resolve(df_, "cat");
    name_ = resolve(df_, "name");
    fhash_ = resolve(df_, "fhash");
    hhash_ = resolve(df_, "hhash");
    pid_ = resolve(df_, "pid");
    tid_ = resolve(df_, "tid");
    ts_ = resolve(df_, "ts");
    dur_ = resolve(df_, "dur");
    ph_ = resolve(df_, "ph");
    const std::int32_t ncols = dftu_dataframe_num_columns(df_);
    static constexpr std::string_view ARGS_PREFIX = "args.";
    for (std::int32_t i = 0; i < ncols; ++i) {
        const char* n = dftu_dataframe_column_name(df_, i);
        if (!n) continue;
        std::string_view nv{n};
        if (nv.substr(0, ARGS_PREFIX.size()) != ARGS_PREFIX) continue;
        std::string bare(nv.substr(ARGS_PREFIX.size()));
        dyn_.emplace_back(std::move(bare), resolve(df_, n));
    }
}

inline void Batch::reset() noexcept {
    if (cat_.handle) dftu_series_free(cat_.handle);
    if (name_.handle) dftu_series_free(name_.handle);
    if (fhash_.handle) dftu_series_free(fhash_.handle);
    if (hhash_.handle) dftu_series_free(hhash_.handle);
    if (pid_.handle) dftu_series_free(pid_.handle);
    if (tid_.handle) dftu_series_free(tid_.handle);
    if (ts_.handle) dftu_series_free(ts_.handle);
    if (dur_.handle) dftu_series_free(dur_.handle);
    if (ph_.handle) dftu_series_free(ph_.handle);
    for (auto& [name, c] : dyn_)
        if (c.handle) dftu_series_free(c.handle);
    dyn_.clear();
}

inline Batch::~Batch() { reset(); }

inline Batch::Batch(Batch&& o) noexcept
    : df_(o.df_),
      n_(o.n_),
      cat_(o.cat_),
      name_(o.name_),
      fhash_(o.fhash_),
      hhash_(o.hhash_),
      pid_(o.pid_),
      tid_(o.tid_),
      ts_(o.ts_),
      dur_(o.dur_),
      ph_(o.ph_),
      dyn_(std::move(o.dyn_)) {
    o.cat_ = o.name_ = o.fhash_ = o.hhash_ = {};
    o.pid_ = o.tid_ = o.ts_ = o.dur_ = o.ph_ = {};
    o.dyn_.clear();
}
inline Batch& Batch::operator=(Batch&& o) noexcept {
    if (this != &o) {
        reset();
        df_ = o.df_;
        n_ = o.n_;
        cat_ = o.cat_;
        name_ = o.name_;
        fhash_ = o.fhash_;
        hhash_ = o.hhash_;
        pid_ = o.pid_;
        tid_ = o.tid_;
        ts_ = o.ts_;
        dur_ = o.dur_;
        ph_ = o.ph_;
        dyn_ = std::move(o.dyn_);
        o.cat_ = o.name_ = o.fhash_ = o.hhash_ = {};
        o.pid_ = o.tid_ = o.ts_ = o.dur_ = o.ph_ = {};
        o.dyn_.clear();
    }
    return *this;
}

inline const detail::ColBuf* Batch::find_dyn(
    std::string_view key) const noexcept {
    for (const auto& [name, c] : dyn_)
        if (name == key) return &c;
    return nullptr;
}

template <class Fn>
inline int Host::trace_read(const char* path, Fn&& fn) const {
    auto thunk = [](const void* item, void* ud) {
        const auto* df = static_cast<const dftu_dataframe*>(item);
        auto& f = *static_cast<std::decay_t<Fn>*>(ud);
        if constexpr (std::is_invocable_v<std::decay_t<Fn>&, const Batch&>)
            f(Batch{df});
        else
            f(df);
    };
    return trace_read(
        path, thunk,
        const_cast<void*>(static_cast<const void*>(std::addressof(fn))));
}

/// Non-owning view over a compiled dftu_query (see Host::compile). Host-owned
/// and valid for the scan; the plugin never frees it.
class Query {
   public:
    Query() = default;

    explicit operator bool() const noexcept { return q_ != nullptr; }

    bool matches(const dftu_dataframe* df, std::int64_t row) const {
        return host_.query_matches(q_, df, row);
    }
    bool matches(const Event& e) const { return matches(e.frame(), e.row()); }

    /// Borrowed handle; the host retains ownership. Null if unsupported.
    dftu_query* raw() const noexcept { return q_; }

   private:
    friend class Host;
    Query(Host host, dftu_query* q) noexcept : host_(host), q_(q) {}

    Host host_{nullptr};
    dftu_query* q_ = nullptr;
};

inline Query Host::compile(std::string_view src) const {
    return Query{*this, query_compile(src)};
}
inline Query Host::compile(const Expr& e) const {
    return Query{*this, query_compile(e)};
}

/// Move-only RAII owner of a host trace writer (see Host::trace_writer):
/// opens a gzip .pfw.gz for writing, appends events, then closes exactly once,
/// in close() or the destructor, whichever runs first.
class TraceWriter {
   public:
    TraceWriter() = default;
    TraceWriter(TraceWriter&& o) noexcept : host_(o.host_), w_(o.w_) {
        o.w_ = nullptr;
    }
    TraceWriter& operator=(TraceWriter&& o) noexcept {
        if (this != &o) {
            close();
            host_ = o.host_;
            w_ = o.w_;
            o.w_ = nullptr;
        }
        return *this;
    }
    TraceWriter(const TraceWriter&) = delete;
    TraceWriter& operator=(const TraceWriter&) = delete;
    ~TraceWriter() { close(); }

    explicit operator bool() const noexcept { return w_ != nullptr; }

    int write(const dftu_dataframe* df) const {
        return w_ ? host_.trace_write(w_, df) : -1;
    }
    int write(const Batch& b) const { return write(b.raw()); }
    /// Idempotent: a no-op returning 0 once already closed.
    int close() {
        int rc = w_ ? host_.trace_close(w_) : 0;
        w_ = nullptr;
        return rc;
    }

    /// Borrowed handle; the TraceWriter retains ownership. Null if unsupported.
    dftu_trace_writer* raw() const noexcept { return w_; }

   private:
    friend class Host;
    TraceWriter(Host host, dftu_trace_writer* w) noexcept
        : host_(host), w_(w) {}

    Host host_{nullptr};
    dftu_trace_writer* w_ = nullptr;
};

inline TraceWriter Host::trace_writer(const char* path) const {
    return TraceWriter{*this, trace_open_write(path)};
}

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_MAP_H */
