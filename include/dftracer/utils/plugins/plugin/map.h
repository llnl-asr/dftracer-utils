// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_MAP_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_MAP_H

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/dataframe/plugin_error.h>
#include <dftracer/utils/dataframe/plugin_frame.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin/async.h>
#include <dftracer/utils/plugins/plugin/fs.h>
#include <dftracer/utils/plugins/plugin/types.h>
#include <dftracer/utils/plugins/result_registry.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <type_traits>
#include <utility>

namespace dftracer::utils::plugins {

namespace detail {

struct HostAccess;
struct BatchAccess;
struct OpArgsBuilderAccess;

/// The op of a task a service call started; when the call failed, an op that
/// fails with its status when awaited.
/// `t` is taken by reference: callers write started(h, call(..., &t), t), and
/// the order of argument evaluation is unspecified.
inline AsyncOp started(const dftu_plugin_host* host, dftu_status st,
                       dftu_task* const& t) {
    if (st < 0) return AsyncOp{dataframe::error_of(st)};
    return async_op(t, host);
}

}  // namespace detail

/// Typed op argument list; the one C++ spelling of dftu_op_arg. Series and
/// frame slots borrow the views the plugin already holds, and every string or
/// list slot borrows its caller-owned backing storage.
class OpArgsBuilder {
   public:
    OpArgsBuilder() = default;

    OpArgsBuilder& scalar(std::uint32_t i, dataframe::Scalar value) noexcept {
        args_.scalar(i, value);
        return *this;
    }
    OpArgsBuilder& i64(std::uint32_t i, std::int64_t value) noexcept {
        args_.i64(i, value);
        return *this;
    }
    OpArgsBuilder& u64(std::uint32_t i, std::uint64_t value) noexcept {
        args_.u64(i, value);
        return *this;
    }
    OpArgsBuilder& f64(std::uint32_t i, double value) noexcept {
        args_.f64(i, value);
        return *this;
    }
    OpArgsBuilder& i32(std::uint32_t i, std::int32_t value) noexcept {
        args_.i32(i, value);
        return *this;
    }
    OpArgsBuilder& ch(std::uint32_t i, char value) noexcept {
        args_.ch(i, value);
        return *this;
    }
    OpArgsBuilder& series(std::uint32_t i,
                          dataframe::ColumnView value) noexcept {
        args_.series(i, dataframe::detail::column_series(value));
        return *this;
    }
    OpArgsBuilder& frame(std::uint32_t i, dataframe::FrameView value) noexcept {
        args_.frame(i, dataframe::detail::frame_pointer(value));
        return *this;
    }
    OpArgsBuilder& str(std::uint32_t i, dstd::CStringView value) noexcept {
        args_.str(i, value.view());
        return *this;
    }
    OpArgsBuilder& strlist(std::uint32_t i,
                           dstd::Span<const dstd::CStringView> items) noexcept {
        if (c_strings_.size() <= i) c_strings_.resize(i + 1);
        auto& slot = c_strings_[i];
        slot.clear();
        slot.reserve(items.size());
        for (const dstd::CStringView item : items) {
            slot.push_back(item.c_str());
        }
        args_.strlist(i, slot.data(), static_cast<std::int32_t>(slot.size()));
        return *this;
    }
    OpArgsBuilder& i32list(std::uint32_t i,
                           dstd::Span<const std::int32_t> items) noexcept {
        args_.i32list(i, items);
        return *this;
    }
    OpArgsBuilder& i64list(std::uint32_t i,
                           dstd::Span<const std::int64_t> items) noexcept {
        args_.i64list(i, items);
        return *this;
    }

    OpArgsBuilder(const OpArgsBuilder&) = delete;
    OpArgsBuilder& operator=(const OpArgsBuilder&) = delete;
    OpArgsBuilder(OpArgsBuilder&&) noexcept = default;
    OpArgsBuilder& operator=(OpArgsBuilder&&) noexcept = default;

   private:
    friend class Host;
    friend struct detail::OpArgsBuilderAccess;

    const dftu_op_arg* raw() const noexcept { return &args_.raw(); }

    dataframe::OpArgs args_;
    dstd::Vector<dstd::Vector<const char*>> c_strings_;
};

namespace detail {

struct OpArgsBuilderAccess {
    static const dftu_op_arg* raw(const OpArgsBuilder& b) noexcept {
        return b.raw();
    }
};

inline const dftu_op_arg* op_args_raw(const OpArgsBuilder& b) noexcept {
    return OpArgsBuilderAccess::raw(b);
}

}  // namespace detail

class Writer;
class Agg;
class Query;
class TraceWriter;
class Batch;
class Event;
template <class T>
class OutPort;
template <class T>
class InPort;

/// The services the host lends a plugin, bound to the call they run in. `ctx`
/// is the dftu_ctx of the callback the Host was handed to. A service call that
/// fails returns a @ref dstd::Result holding an error with the host's status
/// (the host already holds the message on the ctx), and a host without the
/// service group returns one with DFTU_E_UNSUPPORTED; a call that starts a task
/// gives an @ref AsyncOp that fails when awaited instead. A Host is valid for
/// the call it was handed to, or for a Task's whole run.
class Host {
   public:
    Host() = default;

    dstd::StringView str(StrId id) const {
        if (id.absent()) return {};
        std::uint32_t n = 0;
        const char* p = h_->resolve(h_->h, id.raw(), &n);
        return p ? dstd::StringView{p, n} : dstd::StringView{};
    }
    StrId intern(dstd::StringView s) const {
        return StrId{
            h_->intern(h_->h, s.data(), static_cast<std::uint32_t>(s.size()))};
    }
    void log(LogLevel level, dstd::StringView msg) const {
        log(static_cast<dftu_log_level>(level), msg);
    }

    /// An error with DFTU_E_CANCELLED once the call was cancelled; call it once
    /// per morsel in a long loop.
    dstd::Result<void> check() const {
        const dftu_status st = dftu_check(ctx_);
        if (st < 0) return dataframe::error_of(st, "cancelled");
        return {};
    }

    Io io() const { return Io{h_, ctx_}; }
    Fs fs() const { return Fs{h_, ctx_}; }

   private:
    void log(dftu_log_level level, dstd::StringView msg) const {
        h_->log(h_->h, static_cast<std::uint8_t>(level), msg.data(),
                static_cast<std::uint32_t>(msg.size()));
    }
    AsyncOp await(dftu_task* t) const { return detail::async_op(t, h_); }
    /// The op of a task handed over by @ref detail::release_op; an op that
    /// never started fails with its own error.
    AsyncOp await(dstd::Result<dftu_task*> t) const {
        if (!t.has_value()) return AsyncOp{std::move(t).error()};
        return detail::async_op(*t, h_);
    }
    /// Await every task together; see @ref AsyncAll.
    AsyncAll all(std::initializer_list<dstd::Result<dftu_task*>> ts) const {
        return AsyncAll(ts, h_);
    }
    /// Await every task in `ts` together, taking them over.
    AsyncAll all(dstd::Vector<dstd::Result<dftu_task*>> ts) const {
        return AsyncAll(std::move(ts), h_);
    }

    /// The task service: jobs, spawn and drive; see abi/task.h.
    dstd::Result<const dftu_svc_task*> tasks() const {
        return need(DFTU_SVC_TASK, task_ext_);
    }

    /// Start `fn(ctx, arg)` on a pool worker (or where `where` says) and get an
    /// @ref AsyncOp to co_await; `arg` is plugin-owned and must outlive the
    /// await. A negative status from `fn` fails the task with that status.
    AsyncOp spawn(dftu_status (*fn)(dftu_ctx*, void*), void* arg,
                  dftu_job_where where = DFTU_JOB_CPU) const {
        auto svc = tasks();
        if (!svc.has_value()) return AsyncOp{std::move(svc).error()};
        dftu_task* t = nullptr;
        return detail::started(
            h_, (*svc)->spawn(ctx_, h_->h, fn, arg, where, &t), t);
    }

   public:
    /// Start any callable returning void, dftu_status or a @ref dstd::Result of
    /// void on a pool worker; `fn` is borrowed for the await, so it must
    /// outlive it (a coroutine local naturally does). A failure fails the task
    /// with its status and message. Use JobWhere::Blocking for a call that
    /// blocks, so the worker gives back its run permit.
    template <class Fn>
    AsyncOp spawn(Fn& fn, JobWhere where = JobWhere::Cpu) const {
        return spawn(
            [](dftu_ctx* ctx, void* p) noexcept -> dftu_status {
                return detail::to_status(ctx, *static_cast<Fn*>(p));
            },
            &fn, static_cast<dftu_job_where>(where));
    }

   private:
    /// Valid for the whole scan. DFTU_E_PARSE for a parse error. Do not free.
    dstd::Result<dftu_duql*> duql_compile(dstd::StringView src) const {
        DFTRACER_UTILS_TRY(const dftu_svc_duql* e,
                           need(DFTU_SVC_DUQL, query_ext_));
        dftu_duql* out = nullptr;
        DFTRACER_UTILS_TRY_VOID(detail::reported(
            e->duql_compile(ctx_, h_->h, src.data(),
                            static_cast<std::uint32_t>(src.size()), &out)));
        return out;
    }

    /// Compile a predicate built with `F`/`Field`, e.g.
    /// `h.duql_compile(F("dur") > 1000)`. Rendered to a DSL string and parsed
    /// host-side, so the plugin links nothing from the query library.
    dstd::Result<dftu_duql*> duql_compile(const Expr& e) const {
        return duql_compile(e.to_string());
    }
    bool duql_matches(const dftu_duql* q, const dftu_dataframe* df,
                      std::int64_t row) const {
        const dftu_svc_duql* qe = ext(DFTU_SVC_DUQL, query_ext_);
        return q && qe && qe->duql_matches &&
               qe->duql_matches(h_->h, q, df, row) != 0;
    }

   public:
    /// Compile and wrap as a non-owning Query bound to this host; see
    /// duql_compile for lifetime.
    dstd::Result<Query> compile(dstd::StringView src) const;
    /// compile() for a predicate built with `F`/`Field`.
    dstd::Result<Query> compile(const Expr& e) const;

   private:
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

   public:
    /// RAII sketch bound to this host; frees the handle in its destructor.
    class Sketch make_sketch() const;

   private:
    dstd::Result<dftu_trace_writer*> trace_open_write(
        dstd::CStringView path) const {
        DFTRACER_UTILS_TRY(const dftu_svc_trace* e,
                           need(DFTU_SVC_TRACE, trace_ext_));
        dftu_trace_writer* w = nullptr;
        DFTRACER_UTILS_TRY_VOID(detail::reported(
            e->trace_open_write(ctx_, h_->h, path.c_str(), &w)));
        return w;
    }
    /// Append every row of `df` as a trace event.
    dstd::Result<void> trace_write(dftu_trace_writer* w,
                                   const dftu_dataframe* df) const {
        DFTRACER_UTILS_TRY(const dftu_svc_trace* e,
                           need(DFTU_SVC_TRACE, trace_ext_));
        return detail::reported(e->trace_write(ctx_, h_->h, w, df));
    }
    dstd::Result<void> trace_close(dftu_trace_writer* w) const {
        DFTRACER_UTILS_TRY(const dftu_svc_trace* e,
                           need(DFTU_SVC_TRACE, trace_ext_));
        return detail::reported(e->trace_close(ctx_, h_->h, w));
    }
    /// Scan `path` (auto-indexed) and call on_batch once per scanned batch with
    /// a dftu_dataframe*. A negative status from on_batch stops the read and is
    /// the result.
    dstd::Result<void> trace_read(dstd::CStringView path,
                                  dftu_stream_item_fn on_batch,
                                  void* ud) const {
        DFTRACER_UTILS_TRY(const dftu_svc_trace* e,
                           need(DFTU_SVC_TRACE, trace_ext_));
        return detail::reported(
            e->trace_read(ctx_, h_->h, path.c_str(), on_batch, ud));
    }

   public:
    /// Scan `path` (auto-indexed), invoking `fn` once per scanned batch as
    /// either `fn(const Batch&)` or `fn(const dftu_dataframe*)`, returning void
    /// or a @ref dstd::Result of void; an error it returns stops the read and
    /// is the result. `fn` is borrowed for the call.
    template <class Fn>
    dstd::Result<void> trace_read(dstd::CStringView path, Fn&& fn) const;

    /// RAII trace writer for `path` (a gzip .pfw.gz).
    dstd::Result<TraceWriter> trace_writer(dstd::CStringView path) const;

    /// The fileio service: line and chunk readers, the gzip line writer,
    /// whole-file gzip and the file helpers; see plugin/fileio.h.
    class Fileio fileio() const;

    /// The JSON and YAML services: parse text into a dftu_value tree and write
    /// a document; see plugin/format.h.
    class Json json() const;
    class Yaml yaml() const;

    /// The store service: the index root's store of the trace folder the scan
    /// reads, in the plugin's own namespaces; see plugin/store.h.
    class Store store() const;

    /// The memory service: blocks in the plugin's pool and reservations with
    /// reclaimers; see plugin/memory.h.
    class Memory memory() const;

    /// Channels between plugins, latest values and phase results; see
    /// plugin/ports.h.
    class Ports ports() const;

    /// Get a host-owned parallel Writer for `path` with `worker_count` lanes
    /// (`gzip` != 0 compresses). The Writer is host-owned and scan-lifetime;
    /// open it, write chunks per worker, then close it - all co_await forms.
    dstd::Result<Writer> writer(dstd::CStringView path,
                                std::uint32_t worker_count,
                                bool gzip = false) const;
    /// Merge worker shard files into `target`; co_await the returned @ref
    /// AsyncOp.
    AsyncOp merge_shards(
        dstd::CStringView target,
        std::initializer_list<dstd::CStringView> shards) const {
        auto e = need(DFTU_SVC_FILEIO, fileio_ext_);
        if (!e.has_value()) return AsyncOp{std::move(e).error()};
        dstd::Vector<const char*> shard_paths;
        shard_paths.reserve(shards.size());
        for (const dstd::CStringView shard : shards) {
            shard_paths.push_back(shard.c_str());
        }
        dftu_task* t = nullptr;
        return detail::started(
            h_,
            (*e)->merge_shards(ctx_, h_->h, target.c_str(), shard_paths.data(),
                               static_cast<std::uint32_t>(shard_paths.size()),
                               &t),
            t);
    }

   private:
    /// Batch-scoped ports: a producer publishes a derived value a later
    /// consumer reads for the same batch. consume returns NULL (and 0 len) when
    /// the producer has not published this batch; the borrow is valid only
    /// until the current on_batch returns.
    void publish(std::uint64_t key, const void* data, std::uint32_t len) const {
        const dftu_svc_ports* e = ext(DFTU_SVC_PORTS, ports_ext_);
        if (e && e->publish) e->publish(h_->h, key, data, len);
    }
    const void* consume(std::uint64_t key, std::uint32_t* out_size) const {
        const dftu_svc_ports* e = ext(DFTU_SVC_PORTS, ports_ext_);
        return e && e->consume ? e->consume(h_->h, key, out_size) : nullptr;
    }
    dstd::Result<dftu_agg*> agg_new(dstd::CStringView name,
                                    dstd::Span<const char* const> key_names,
                                    const dftu_agg_col* specs,
                                    std::uint32_t spec_n) const {
        DFTRACER_UTILS_TRY(const dftu_svc_agg* e, need(DFTU_SVC_AGG, agg_ext_));
        dftu_agg* out = nullptr;
        DFTRACER_UTILS_TRY_VOID(detail::reported(
            e->agg_new(ctx_, h_->h, name.c_str(), key_names.data(),
                       static_cast<std::uint32_t>(key_names.size()), specs,
                       spec_n, &out)));
        return out;
    }
    dstd::Result<void> agg_accumulate(dftu_agg* a,
                                      const dftu_dataframe* df) const {
        DFTRACER_UTILS_TRY(const dftu_svc_agg* e, need(DFTU_SVC_AGG, agg_ext_));
        return detail::reported(e->agg_accumulate(ctx_, h_->h, a, df));
    }

   public:
    /// Get-or-create a named cross-batch aggregation accumulator grouping by
    /// `key_names` and computing each of `cols`. An error if the host lacks the
    /// agg group or agg_new rejects the spec (a bad op code or missing output
    /// name). A name seen before returns the existing accumulator and ignores
    /// `cols`.
    dstd::Result<Agg> agg(dstd::CStringView name,
                          std::initializer_list<dstd::CStringView> key_names,
                          std::initializer_list<AggCol> cols) const;

    /// The cross-worker-merged, finalized result of the accumulator named
    /// `name` (any plugin's), as an owned frame. Call at on_finalize; the
    /// producing plugin must be registered first. Empty (`!frame`) when
    /// no plugin produced that name.
    dstd::Result<OwnedDataFrame> agg_result(dstd::CStringView name) const;

    /// Look up and run a registered dataframe column op by name on `in` (see
    /// dftu_svc_ops::run). DFTU_E_NOT_FOUND for an unregistered `name` and
    /// DFTU_E_INVALID when the call mismatches the op's kind/arity/shape. The
    /// result is newly owned by the caller.
    dstd::Result<Column> run_op(dstd::CStringView name,
                                std::initializer_list<ColumnView> in,
                                const OpArgsBuilder* args = nullptr) const;
    /// run_op for a signature whose return token is FRAME; the result is
    /// newly owned by the caller.
    dstd::Result<OwnedDataFrame> run_op_frame(
        dstd::CStringView name, std::initializer_list<FrameView> in,
        const OpArgsBuilder* args = nullptr) const;
    /// run_op for a signature whose return token is SCALAR/I64/BOOL, reducing
    /// one column.
    dstd::Result<dataframe::Scalar> run_op_aggregate(
        dstd::CStringView name, ColumnView in,
        const OpArgsBuilder* args = nullptr) const;
    /// The registered op named `name` (built-in or user), or none if the host
    /// lacks the ops group.
    dstd::Optional<OpRef> find_op(dstd::CStringView name) const;
    /// Register a user op in the host's shared registry. The op must be named
    /// `<plugin>.<name>`: the bare and "dftu." namespaces are the host's. An
    /// error for a null desc/name, a name the host keeps, or an already-
    /// registered name.
    dstd::Result<void> register_op(const OpSpec& spec) const;

    /// Named result channel: emit an opaque blob; the host copies `len` bytes.
    /// Surfaces from Plugins::run keyed by name. Best called at on_finalize.
    /// `name` must not be null (@ref dstd::CStringView); an empty `name` is
    /// refused with an error, and so is it in the frame and lazyframe forms.
    dstd::Result<void> emit_result(dstd::CStringView name,
                                   dstd::ByteView data) const {
        dftu_result_value v{};
        v.kind = DFTU_RESULT_KIND_BYTES;
        v.u.bytes.data = data.data();
        v.u.bytes.len = static_cast<std::uint64_t>(data.size());
        return emit(name, &v);
    }
    dstd::Result<void> emit_result(dstd::CStringView name,
                                   dstd::StringView data) const {
        return emit_result(name, dstd::ByteView{data.data(), data.size()});
    }
    /// Hands a native dataframe result to the host, which takes ownership on
    /// success; on an error `df` is left intact so it frees normally. Same
    /// finalize/collision semantics as emit_result.
    dstd::Result<void> emit_result_frame(dstd::CStringView name,
                                         OwnedDataFrame&& df) const {
        dftu_result_value v{};
        v.kind = DFTU_RESULT_KIND_FRAME;
        v.u.frame = df.get();
        DFTRACER_UTILS_TRY_VOID(emit(name, &v));
        df.release();
        return {};
    }
    /// Hands a deferred plan to the host, which takes ownership and collects it
    /// at the Python edge. The plan must be self-contained.
    dstd::Result<void> emit_result_lazyframe(dstd::CStringView name,
                                             OwnedLazyFrame&& lf) const {
        DFTRACER_UTILS_TRY_VOID(emit_result_lazyframe(name, lf.get()));
        lf.release();
        return {};
    }

    /// Typed batch-scoped output port: send() publishes a trivially-copyable T
    /// under `name` for a same-batch consumer. See publish/consume. `name` must
    /// not be null (@ref dstd::CStringView); an empty `name` is a name, not
    /// "no port".
    template <class T>
    OutPort<T> publish_port(dstd::CStringView name) const;
    /// Typed batch-scoped input port: recv() reads the T a producer published
    /// under `name` this batch, or nullopt if none. See publish/consume.
    template <class T>
    InPort<T> consume_port(dstd::CStringView name) const;

    std::uint64_t port_key(dstd::CStringView name) const;
    void publish(std::uint64_t key, dstd::ByteView data) const {
        publish(key, data.data(), static_cast<std::uint32_t>(data.size()));
    }
    dstd::ByteView consume(std::uint64_t key) const {
        std::uint32_t size = 0;
        const void* data = consume(key, &size);
        return data != nullptr
                   ? dstd::ByteView{static_cast<const char*>(data), size}
                   : dstd::ByteView{};
    }

   private:
    friend struct detail::HostAccess;
    friend class Sketch;
    friend class Agg;
    friend class Query;
    friend class TraceWriter;
    friend class Writer;

    Host(const dftu_plugin_host* h, dftu_ctx* ctx) : h_(h), ctx_(ctx) {}

    dstd::Result<void> emit_result_lazyframe(dstd::CStringView name,
                                             dftu_lazyframe* lf) const {
        dftu_result_value v{};
        v.kind = DFTU_RESULT_KIND_LAZYFRAME;
        v.u.lazyframe = lf;
        return emit(name, &v);
    }

    /// Fetch a host-service group once and memoize it in `slot`; a missing
    /// group stays null.
    template <class T>
    const T* ext(dstd::CStringView id, const T*& slot) const {
        if (!slot && h_->get_service)
            slot = static_cast<const T*>(h_->get_service(h_->h, id.c_str()));
        return slot;
    }
    template <class T>
    dstd::Result<const T*> need(dstd::CStringView id, const T*& slot) const {
        const T* e = ext(id, slot);
        if (!e)
            return make_error(
                dstd::Condition::Unsupported,
                dstd::String("host lacks the service ") + id.c_str());
        return e;
    }

    dstd::Result<void> emit(dstd::CStringView name,
                            dftu_result_value* v) const {
        DFTRACER_UTILS_TRY(const dftu_svc_result* e,
                           need(DFTU_SVC_RESULT, result_ext_));
        return detail::reported(e->emit(ctx_, h_->h, name.c_str(), v));
    }

    const dftu_plugin_host* h_ = nullptr;
    dftu_ctx* ctx_ = nullptr;
    mutable const dftu_svc_task* task_ext_ = nullptr;
    mutable const dftu_svc_duql* query_ext_ = nullptr;
    mutable const dftu_svc_fileio* fileio_ext_ = nullptr;
    mutable const dftu_svc_sketch* sketch_ext_ = nullptr;
    mutable const dftu_svc_trace* trace_ext_ = nullptr;
    mutable const dftu_svc_ports* ports_ext_ = nullptr;
    mutable const dftu_svc_result* result_ext_ = nullptr;
    mutable const dftu_svc_agg* agg_ext_ = nullptr;
    mutable const dftu_svc_ops* ops_ext_ = nullptr;
};

namespace detail {

struct HostAccess {
    static const dftu_plugin_host* raw(const Host& host) noexcept {
        return host.h_;
    }
    static dftu_ctx* ctx(const Host& host) noexcept { return host.ctx_; }
    static dstd::Result<const dftu_svc_task*> tasks(const Host& host) {
        return host.tasks();
    }
    static Host from_raw(const dftu_plugin_host* host, dftu_ctx* ctx) noexcept {
        return Host{host, ctx};
    }
    static AsyncOp spawn_raw(const Host& host,
                             dftu_status (*fn)(dftu_ctx*, void*), void* arg,
                             dftu_job_where where = DFTU_JOB_CPU) {
        return host.spawn(fn, arg, where);
    }
    static AsyncOp await_raw(const Host& host, dftu_task* t) {
        return host.await(t);
    }
    static AsyncOp await_result(const Host& host, dstd::Result<dftu_task*> t) {
        return host.await(std::move(t));
    }
    static AsyncAll all_raw(
        const Host& host, std::initializer_list<dstd::Result<dftu_task*>> ts) {
        return host.all(ts);
    }
    static AsyncAll all_raw(const Host& host,
                            dstd::Vector<dstd::Result<dftu_task*>> ts) {
        return host.all(std::move(ts));
    }
};

inline Host host_from_raw(const dftu_plugin_host* host,
                          dftu_ctx* ctx) noexcept {
    return HostAccess::from_raw(host, ctx);
}

inline AsyncOp spawn_raw(const Host& host, dftu_status (*fn)(dftu_ctx*, void*),
                         void* arg, dftu_job_where where = DFTU_JOB_CPU) {
    return HostAccess::spawn_raw(host, fn, arg, where);
}

inline const dftu_plugin_host* host_raw(const Host& host) noexcept {
    return HostAccess::raw(host);
}

inline dftu_ctx* host_ctx(const Host& host) noexcept {
    return HostAccess::ctx(host);
}

inline dstd::Result<const dftu_svc_task*> task_service(const Host& host) {
    return HostAccess::tasks(host);
}

inline AsyncOp await_raw(const Host& host, dftu_task* t) {
    return HostAccess::await_raw(host, t);
}

inline AsyncOp await_result(const Host& host, dstd::Result<dftu_task*> t) {
    return HostAccess::await_result(host, std::move(t));
}

inline AsyncAll all_raw(const Host& host,
                        std::initializer_list<dstd::Result<dftu_task*>> ts) {
    return HostAccess::all_raw(host, ts);
}

inline AsyncAll all_raw(const Host& host,
                        dstd::Vector<dstd::Result<dftu_task*>> ts) {
    return HostAccess::all_raw(host, std::move(ts));
}

inline Quantiles quantiles_from_raw(dftu_quantiles q) noexcept {
    return Quantiles{q.count, q.min, q.max, q.mean, q.p50, q.p90, q.p95, q.p99};
}

}  // namespace detail

inline dstd::Result<Column> Host::run_op(dstd::CStringView name,
                                         std::initializer_list<ColumnView> in,
                                         const OpArgsBuilder* args) const {
    DFTRACER_UTILS_TRY(const dftu_svc_ops* e, need(DFTU_SVC_OPS, ops_ext_));
    dstd::Vector<const dftu_series*> vin;
    vin.reserve(in.size());
    for (const ColumnView column : in) {
        vin.push_back(dataframe::detail::column_series(column));
    }
    dftu_series* out = nullptr;
    DFTRACER_UTILS_TRY_VOID(detail::reported(
        e->run(ctx_, h_->h, name.c_str(), vin.data(),
               static_cast<std::uint32_t>(vin.size()),
               args != nullptr ? args->raw() : nullptr, &out)));
    return Column{out};
}

inline dstd::Result<OwnedDataFrame> Host::run_op_frame(
    dstd::CStringView name, std::initializer_list<FrameView> in,
    const OpArgsBuilder* args) const {
    DFTRACER_UTILS_TRY(const dftu_svc_ops* e, need(DFTU_SVC_OPS, ops_ext_));
    dstd::Vector<const dftu_dataframe*> vin;
    vin.reserve(in.size());
    for (const FrameView frame : in) {
        vin.push_back(dataframe::detail::frame_pointer(frame));
    }
    dftu_dataframe* out = nullptr;
    DFTRACER_UTILS_TRY_VOID(detail::reported(
        e->run_frame(ctx_, h_->h, name.c_str(), vin.data(),
                     static_cast<std::uint32_t>(vin.size()),
                     args != nullptr ? args->raw() : nullptr, &out)));
    return OwnedDataFrame{out};
}

inline dstd::Result<dataframe::Scalar> Host::run_op_aggregate(
    dstd::CStringView name, ColumnView in, const OpArgsBuilder* args) const {
    DFTRACER_UTILS_TRY(const dftu_svc_ops* e, need(DFTU_SVC_OPS, ops_ext_));
    const dftu_series* input = dataframe::detail::column_series(in);
    dftu_scalar out{};
    DFTRACER_UTILS_TRY_VOID(detail::reported(
        e->run_aggregate(ctx_, h_->h, name.c_str(), &input, 1,
                         args != nullptr ? args->raw() : nullptr, &out)));
    return dataframe::Scalar{out};
}

inline dstd::Optional<OpRef> Host::find_op(dstd::CStringView name) const {
    const dftu_svc_ops* const e = ext(DFTU_SVC_OPS, ops_ext_);
    if (!e || !e->find) return dstd::nullopt;
    const dftu_op_desc* const desc = e->find(h_->h, name.c_str());
    return desc != nullptr ? dstd::Optional<OpRef>{OpRef{desc}} : dstd::nullopt;
}

inline dstd::Result<void> Host::register_op(const OpSpec& spec) const {
    DFTRACER_UTILS_TRY(const dftu_svc_ops* e, need(DFTU_SVC_OPS, ops_ext_));
    return detail::reported(
        e->register_op(ctx_, h_->h, detail::op_spec_raw(spec)));
}

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
    Quantiles result() const {
        return detail::quantiles_from_raw(host_.sketch_result(s_));
    }

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
    dstd::Result<void> accumulate(dataframe::FrameView df) const {
        return host_.agg_accumulate(a_, dataframe::detail::frame_pointer(df));
    }
    dstd::Result<void> accumulate(const OwnedDataFrame& df) const {
        return accumulate(df.view());
    }

    explicit operator bool() const noexcept { return a_ != nullptr; }

   private:
    friend class Host;
    Agg(Host host, dftu_agg* a) noexcept : host_(host), a_(a) {}

    Host host_;
    dftu_agg* a_ = nullptr;
};

inline dstd::Result<Agg> Host::agg(
    dstd::CStringView name, std::initializer_list<dstd::CStringView> key_names,
    std::initializer_list<AggCol> cols) const {
    dstd::Vector<const char*> keys;
    keys.reserve(key_names.size());
    for (const dstd::CStringView key : key_names) {
        keys.push_back(key.c_str());
    }
    dstd::Vector<dftu_agg_col> specs;
    specs.reserve(cols.size());
    for (const AggCol& c : cols) specs.push_back(detail::agg_col_raw(c));
    DFTRACER_UTILS_TRY(dftu_agg * a,
                       agg_new(name, keys, specs.data(),
                               static_cast<std::uint32_t>(specs.size())));
    return Agg{*this, a};
}

inline dstd::Result<OwnedDataFrame> Host::agg_result(
    dstd::CStringView name) const {
    DFTRACER_UTILS_TRY(const dftu_svc_agg* e, need(DFTU_SVC_AGG, agg_ext_));
    dftu_dataframe* out = nullptr;
    DFTRACER_UTILS_TRY_VOID(
        detail::reported(e->agg_result(ctx_, h_->h, name.c_str(), &out)));
    return OwnedDataFrame{out};
}

/// Typed, non-owning view over a host-owned parallel Writer (see Host::writer).
/// The writer is host-owned and scan-lifetime, so there is nothing to free; the
/// lifecycle is open() once, chunk() per worker lane, then close(), each a
/// co_await-able @ref AsyncOp. Worker lanes write concurrently, so chunks for
/// distinct `worker` indices may be in flight at once.
class Writer {
   public:
    Writer() = default;

    /// Open the underlying output; co_await before writing any chunk.
    AsyncOp open() const {
        dftu_task* t = nullptr;
        return detail::started(
            detail::host_raw(host_),
            e_->writer_open(detail::host_ctx(host_), h(), w_, &t), t);
    }
    /// Append `len` bytes to lane `worker`; co_await to complete the write.
    AsyncOp chunk(std::uint32_t worker, dstd::ByteView data) const {
        return chunk(worker, data.data(), data.size());
    }
    AsyncOp chunk(std::uint32_t worker, dstd::StringView data) const {
        return chunk(worker, dstd::ByteView{data.data(), data.size()});
    }
    /// Flush and finalize; co_await after all chunks are written.
    AsyncOp close() const {
        dftu_task* t = nullptr;
        return detail::started(
            detail::host_raw(host_),
            e_->writer_close(detail::host_ctx(host_), h(), w_, &t), t);
    }

    explicit operator bool() const noexcept { return w_ != nullptr; }

   private:
    friend class Host;
    Writer(Host host, const dftu_svc_fileio* e, dftu_writer* w)
        : host_(host), e_(e), w_(w) {}
    void* h() const noexcept { return detail::host_raw(host_)->h; }
    dftu_ctx* ctx() const noexcept { return detail::host_ctx(host_); }
    AsyncOp chunk(std::uint32_t worker, const void* data,
                  std::uint64_t len) const {
        dftu_task* t = nullptr;
        return detail::started(detail::host_raw(host_),
                               e_->writer_chunk(detail::host_ctx(host_), h(),
                                                w_, worker, data, len, &t),
                               t);
    }

    Host host_;
    const dftu_svc_fileio* e_ = nullptr;
    dftu_writer* w_ = nullptr;
};

inline dstd::Result<Writer> Host::writer(dstd::CStringView path,
                                         std::uint32_t worker_count,
                                         bool gzip) const {
    DFTRACER_UTILS_TRY(const dftu_svc_fileio* e,
                       need(DFTU_SVC_FILEIO, fileio_ext_));
    dftu_writer* w = nullptr;
    DFTRACER_UTILS_TRY_VOID(detail::reported(e->writer_create(
        ctx_, h_->h, path.c_str(), worker_count, gzip ? 1 : 0, &w)));
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
        if (key_ != 0) {
            host_.publish(
                key_,
                dstd::ByteView{reinterpret_cast<const char*>(&v), sizeof(T)});
        }
    }

    std::uint64_t key() const noexcept { return key_; }
    explicit operator bool() const noexcept { return key_ != 0; }

   private:
    friend class Host;
    OutPort(Host host, std::uint64_t key) : host_(host), key_(key) {}
    Host host_;
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
    dstd::Optional<T> recv() const {
        if (key_ == 0) return dstd::nullopt;
        const dstd::ByteView data = host_.consume(key_);
        if (data.size() != sizeof(T)) return dstd::nullopt;
        T out;
        std::memcpy(&out, data.data(), sizeof(T));
        return out;
    }

    std::uint64_t key() const noexcept { return key_; }
    explicit operator bool() const noexcept { return key_ != 0; }

   private:
    friend class Host;
    InPort(Host host, std::uint64_t key) : host_(host), key_(key) {}
    Host host_;
    std::uint64_t key_ = 0;
};

template <class T>
inline OutPort<T> Host::publish_port(dstd::CStringView name) const {
    return OutPort<T>{*this, port_key(name)};
}
inline std::uint64_t Host::port_key(dstd::CStringView name) const {
    const dftu_svc_ports* e = ext(DFTU_SVC_PORTS, ports_ext_);
    return e && e->port_key ? e->port_key(h_->h, name.c_str()) : 0;
}
template <class T>
inline InPort<T> Host::consume_port(dstd::CStringView name) const {
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
    dataframe::FrameView frame() const noexcept;

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
    dstd::StringView cat() const noexcept;
    dstd::StringView name() const noexcept;
    dstd::StringView fhash() const noexcept;
    dstd::StringView hhash() const noexcept;

    /// A dyn (per-key) arg column, present when any event in the batch carried
    /// that flattened dotted key ("args.ret", or the bare key "ret").
    bool has_arg(dstd::StringView key) const noexcept;
    bool arg_is_i64(dstd::StringView key) const noexcept;
    bool arg_is_f64(dstd::StringView key) const noexcept;
    bool arg_is_str(dstd::StringView key) const noexcept;
    std::int64_t arg_i64(dstd::StringView key) const noexcept;
    double arg_f64(dstd::StringView key) const noexcept;
    dstd::StringView arg_str(dstd::StringView key) const noexcept;

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
    ~Batch();
    Batch(Batch&&) noexcept;
    Batch& operator=(Batch&&) noexcept;
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

    std::int64_t size() const noexcept { return n_; }
    bool empty() const noexcept { return n_ == 0; }

    dataframe::FrameView frame() const noexcept {
        return dataframe::FrameView{df_};
    }

    Event begin() const noexcept { return Event{this, 0}; }
    Event end() const noexcept { return Event{this, n_}; }
    Event operator[](std::int64_t i) const noexcept { return Event{this, i}; }

   private:
    friend class Event;
    friend struct detail::BatchAccess;

    explicit Batch(const dftu_dataframe* df) noexcept;
    static detail::ColBuf resolve(const dftu_dataframe* df,
                                  dstd::CStringView name);
    static bool is_str(const detail::ColBuf& c) noexcept {
        return c.type == DFTU_TYPE_STRING;
    }
    static dstd::StringView str_at(const detail::ColBuf& c,
                                   std::int64_t row) noexcept {
        if (!c.data || !c.offsets) {
            if (c.type != DFTU_TYPE_STRING) return {};
            std::int64_t n = 0;
            const char* p = dftu_series_string_at(c.handle, row, &n);
            return p ? dstd::StringView{p, static_cast<std::size_t>(n)}
                     : dstd::StringView{};
        }
        if (dftu_series_is_null(c.handle, row)) return {};
        const char* base = static_cast<const char*>(c.data);
        return {base + c.offsets[row],
                static_cast<std::size_t>(c.offsets[row + 1] - c.offsets[row])};
    }
    const detail::ColBuf* find_dyn(dstd::StringView key) const noexcept;

    void reset() noexcept;

    const dftu_dataframe* df_ = nullptr;
    std::int64_t n_ = 0;
    detail::ColBuf cat_, name_, fhash_, hhash_, pid_, tid_, ts_, dur_, ph_;
    dstd::Vector<dstd::Pair<dstd::String, detail::ColBuf>> dyn_;
};

namespace detail {

struct BatchAccess {
    static Batch from_raw(const dftu_dataframe* df) noexcept {
        return Batch{df};
    }
};

inline Batch batch_from_raw(const dftu_dataframe* df) noexcept {
    return BatchAccess::from_raw(df);
}

}  // namespace detail

inline dataframe::FrameView Event::frame() const noexcept {
    return b_ != nullptr ? b_->frame() : dataframe::FrameView{};
}
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
inline dstd::StringView Event::cat() const noexcept {
    return Batch::str_at(b_->cat_, row_);
}
inline dstd::StringView Event::name() const noexcept {
    return Batch::str_at(b_->name_, row_);
}
inline dstd::StringView Event::fhash() const noexcept {
    return Batch::str_at(b_->fhash_, row_);
}
inline dstd::StringView Event::hhash() const noexcept {
    return Batch::str_at(b_->hhash_, row_);
}
inline bool Event::has_arg(dstd::StringView key) const noexcept {
    return b_->find_dyn(key) != nullptr;
}
inline bool Event::arg_is_i64(dstd::StringView key) const noexcept {
    const detail::ColBuf* c = b_->find_dyn(key);
    return c && c->type == DFTU_TYPE_INT64;
}
inline bool Event::arg_is_f64(dstd::StringView key) const noexcept {
    const detail::ColBuf* c = b_->find_dyn(key);
    return c && c->type == DFTU_TYPE_FLOAT64;
}
inline bool Event::arg_is_str(dstd::StringView key) const noexcept {
    const detail::ColBuf* c = b_->find_dyn(key);
    return c && c->type == DFTU_TYPE_STRING;
}
inline std::int64_t Event::arg_i64(dstd::StringView key) const noexcept {
    const detail::ColBuf* c = b_->find_dyn(key);
    if (!c || c->type != DFTU_TYPE_INT64 || !c->data ||
        dftu_series_is_null(c->handle, row_))
        return 0;
    return static_cast<const std::int64_t*>(c->data)[row_];
}
inline double Event::arg_f64(dstd::StringView key) const noexcept {
    const detail::ColBuf* c = b_->find_dyn(key);
    if (!c || c->type != DFTU_TYPE_FLOAT64 || !c->data ||
        dftu_series_is_null(c->handle, row_))
        return 0.0;
    return static_cast<const double*>(c->data)[row_];
}
inline dstd::StringView Event::arg_str(dstd::StringView key) const noexcept {
    const detail::ColBuf* c = b_->find_dyn(key);
    return c ? Batch::str_at(*c, row_) : dstd::StringView{};
}

inline detail::ColBuf Batch::resolve(const dftu_dataframe* df,
                                     dstd::CStringView name) {
    detail::ColBuf c;
    dftu_series* col = dftu_dataframe_column(df, name.c_str());
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
    static constexpr dstd::StringView ARGS_PREFIX = "args.";
    for (std::int32_t i = 0; i < ncols; ++i) {
        const char* n = dftu_dataframe_column_name(df_, i);
        if (!n) continue;
        dstd::StringView nv{n};
        if (nv.substr(0, ARGS_PREFIX.size()) != ARGS_PREFIX) continue;
        dstd::String bare(nv.substr(ARGS_PREFIX.size()));
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
    dstd::StringView key) const noexcept {
    for (const auto& [name, c] : dyn_)
        if (name == key) return &c;
    return nullptr;
}

template <class Fn>
inline dstd::Result<void> Host::trace_read(dstd::CStringView path,
                                           Fn&& fn) const {
    struct State {
        std::decay_t<Fn>* f;
    } state{const_cast<std::decay_t<Fn>*>(std::addressof(fn))};
    auto thunk = [](dftu_ctx* ctx, const void* item,
                    void* ud) noexcept -> dftu_status {
        const auto* df = static_cast<const dftu_dataframe*>(item);
        auto& st = *static_cast<State*>(ud);
        return detail::to_status(ctx, [&]() -> decltype(auto) {
            if constexpr (std::is_invocable_v<std::decay_t<Fn>&, const Batch&>)
                return (*st.f)(detail::batch_from_raw(df));
            else if constexpr (std::is_invocable_v<std::decay_t<Fn>&,
                                                   dataframe::FrameView>)
                return (*st.f)(dataframe::FrameView{df});
            else
                return (*st.f)(df);
        });
    };
    return trace_read(path, thunk, &state);
}

/// Non-owning view over a compiled dftu_duql (see Host::compile). Host-owned
/// and valid for the scan; the plugin never frees it.
class Query {
   public:
    Query() = default;

    explicit operator bool() const noexcept { return q_ != nullptr; }

    bool matches(dataframe::FrameView df, std::int64_t row) const {
        return host_.duql_matches(q_, dataframe::detail::frame_pointer(df),
                                  row);
    }
    bool matches(const Event& e) const { return matches(e.frame(), e.row()); }

   private:
    friend class Host;
    Query(Host host, dftu_duql* q) noexcept : host_(host), q_(q) {}

    Host host_;
    dftu_duql* q_ = nullptr;
};

inline dstd::Result<Query> Host::compile(dstd::StringView src) const {
    DFTRACER_UTILS_TRY(dftu_duql * q, duql_compile(src));
    return Query{*this, q};
}
inline dstd::Result<Query> Host::compile(const Expr& e) const {
    DFTRACER_UTILS_TRY(dftu_duql * q, duql_compile(e));
    return Query{*this, q};
}

/// Move-only RAII owner of a host trace writer (see Host::trace_writer):
/// opens a gzip .pfw.gz for writing, appends events, then closes exactly once,
/// in close() or the destructor, whichever runs first. The destructor drops a
/// close error; call close() to see it.
class TraceWriter {
   public:
    TraceWriter() = default;
    TraceWriter(TraceWriter&& o) noexcept : host_(o.host_), w_(o.w_) {
        o.w_ = nullptr;
    }
    TraceWriter& operator=(TraceWriter&& o) noexcept {
        if (this != &o) {
            release();
            host_ = o.host_;
            w_ = o.w_;
            o.w_ = nullptr;
        }
        return *this;
    }
    TraceWriter(const TraceWriter&) = delete;
    TraceWriter& operator=(const TraceWriter&) = delete;
    ~TraceWriter() { release(); }

    explicit operator bool() const noexcept { return w_ != nullptr; }

    dstd::Result<void> write(dataframe::FrameView df) const {
        return host_.trace_write(w_, dataframe::detail::frame_pointer(df));
    }
    dstd::Result<void> write(const Batch& b) const { return write(b.frame()); }
    /// Idempotent: a no-op once already closed. The writer is closed even if
    /// the host reports an error.
    dstd::Result<void> close() {
        dftu_trace_writer* w = w_;
        w_ = nullptr;
        if (!w) return {};
        return host_.trace_close(w);
    }

   private:
    friend class Host;
    TraceWriter(Host host, dftu_trace_writer* w) noexcept
        : host_(host), w_(w) {}

    void release() noexcept { close().discard(); }

    Host host_;
    dftu_trace_writer* w_ = nullptr;
};

inline dstd::Result<TraceWriter> Host::trace_writer(
    dstd::CStringView path) const {
    DFTRACER_UTILS_TRY(dftu_trace_writer * w, trace_open_write(path.c_str()));
    return TraceWriter{*this, w};
}

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_MAP_H */
