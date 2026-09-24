#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/field_ref.h>
#include <dftracer/utils/core/common/hash/fnv1a.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/coro/when_any.h>
#include <dftracer/utils/core/coro/yield.h>
#include <dftracer/utils/core/io/ops.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>
#include <dftracer/utils/plugins/fold_adapter.h>
#include <dftracer/utils/plugins/fold_adapter/ext.h>
#include <dftracer/utils/plugins/fold_adapter/state.h>
#include <dftracer/utils/plugins/plugin/map.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/event_source.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/utilities/common/statistics/ddsketch.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/fileio/parallel/merge.h>
#include <simdjson.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#endif
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
#include <dftracer/utils/utilities/common/arrow/ipc_reader.h>
#include <dftracer/utils/utilities/common/arrow/ipc_writer.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::plugins {

namespace {

using query::Query;
using trace::RecordPhase;
namespace parallel = utilities::fileio::parallel;

coro::CoroTask<void>* as_task(::dftu_task* t) {
    return reinterpret_cast<coro::CoroTask<void>*>(t);
}

const char* host_resolve(void* h, dftu_str id, std::uint32_t* out_len) {
    if (out_len) *out_len = 0;
    if (id == DFTU_STR_NONE ||
        id >= dftracer::utils::StringIntern::FAST_CAPACITY)
        return nullptr;
    auto& intern = static_cast<PluginFold*>(h)->intern_table();
    std::string_view sv = intern.resolve(id);
    if (out_len) *out_len = static_cast<std::uint32_t>(sv.size());
    // Interned bytes are stable for the scan, so they outlive the call.
    return sv.data();
}

dftu_str host_intern(void* h, const char* s, std::uint32_t len) {
    if (!s) return DFTU_STR_NONE;
    auto& intern = static_cast<PluginFold*>(h)->intern_table();
    try {
        return intern.get_or_insert(std::string_view{s, len});
    } catch (...) {
        return DFTU_STR_NONE;
    }
}

// Route a plugin's log line through our logger, tagged with the plugin name, so
// it honors the configured (compile-time and runtime) level.
void host_log(void* h, std::uint8_t level, const char* s, std::uint32_t n) {
    const auto* pf = static_cast<const PluginFold*>(h);
    const char* name =
        pf && !pf->plugin_name().empty() ? pf->plugin_name().c_str() : "plugin";
    const int len = static_cast<int>(n);
    const char* msg = s ? s : "";
    switch (level) {
        case DFTU_LOG_ERROR:
            DFTRACER_UTILS_LOG_ERROR("[plugin:%s] %.*s", name, len, msg);
            break;
        case DFTU_LOG_WARN:
            DFTRACER_UTILS_LOG_WARN("[plugin:%s] %.*s", name, len, msg);
            break;
        case DFTU_LOG_DEBUG:
            DFTRACER_UTILS_LOG_DEBUG("[plugin:%s] %.*s", name, len, msg);
            break;
        case DFTU_LOG_TRACE:
            DFTRACER_UTILS_LOG_TRACE("[plugin:%s] %.*s", name, len, msg);
            break;
        default:
            DFTRACER_UTILS_LOG_INFO("[plugin:%s] %.*s", name, len, msg);
            break;
    }
}

// Each op builds a lazy CoroTask; the caller's out-slot must outlive the task.
// The path is copied into the task: the plugin's buffer need not outlive it.
::dftu_task* io_open(void* h, const char* path, int flags, int mode,
                     int* out_fd) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](std::string p, int fl, int md, int* ofd) -> coro::CoroTask<void> {
            int fd = static_cast<int>(
                co_await io::open(p.c_str(), fl, static_cast<mode_t>(md)));
            if (ofd) *ofd = fd;
        }(std::string(path ? path : ""), flags, mode, out_fd));
}

::dftu_task* io_close(void* h, int fd, int* out_rc) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, int* orc) -> coro::CoroTask<void> {
            int rc = static_cast<int>(co_await io::close(f));
            if (orc) *orc = rc;
        }(fd, out_rc));
}

::dftu_task* io_read(void* h, int fd, void* buf, std::uint64_t len,
                     std::int64_t* out_n) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, void* b, std::uint64_t l,
           std::int64_t* on) -> coro::CoroTask<void> {
            ssize_t n = co_await io::read(f, b, static_cast<std::size_t>(l));
            if (on) *on = n;
        }(fd, buf, len, out_n));
}

::dftu_task* io_write(void* h, int fd, const void* buf, std::uint64_t len,
                      std::int64_t* out_n) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, const void* b, std::uint64_t l,
           std::int64_t* on) -> coro::CoroTask<void> {
            ssize_t n = co_await io::write(f, b, static_cast<std::size_t>(l));
            if (on) *on = n;
        }(fd, buf, len, out_n));
}

::dftu_task* io_pread(void* h, int fd, void* buf, std::uint64_t len,
                      std::uint64_t off, std::int64_t* out_n) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, void* b, std::uint64_t l, std::uint64_t o,
           std::int64_t* on) -> coro::CoroTask<void> {
            ssize_t n = co_await io::pread(f, b, static_cast<std::size_t>(l),
                                           static_cast<off_t>(o));
            if (on) *on = n;
        }(fd, buf, len, off, out_n));
}

::dftu_task* io_pwrite(void* h, int fd, const void* buf, std::uint64_t len,
                       std::uint64_t off, std::int64_t* out_n) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, const void* b, std::uint64_t l, std::uint64_t o,
           std::int64_t* on) -> coro::CoroTask<void> {
            ssize_t n = co_await io::pwrite(f, b, static_cast<std::size_t>(l),
                                            static_cast<off_t>(o));
            if (on) *on = n;
        }(fd, buf, len, off, out_n));
}

::dftu_task* io_fsync(void* h, int fd, int* out_rc) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, int* orc) -> coro::CoroTask<void> {
            int rc = static_cast<int>(co_await io::fsync(f));
            if (orc) *orc = rc;
        }(fd, out_rc));
}

::dftu_task* io_ftruncate(void* h, int fd, std::uint64_t len, int* out_rc) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, std::uint64_t l, int* orc) -> coro::CoroTask<void> {
            int rc = static_cast<int>(
                co_await io::ftruncate(f, static_cast<off_t>(l)));
            if (orc) *orc = rc;
        }(fd, len, out_rc));
}

::dftu_task* io_fstat(void* h, int fd, dftu_stat* out) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, dftu_stat* o) -> coro::CoroTask<void> {
            struct stat st{};
            ssize_t rc = co_await io::fstat(f, &st);
            if (o && rc == 0) {
                o->size = static_cast<std::uint64_t>(st.st_size);
#if defined(__APPLE__)
                const struct timespec& mt = st.st_mtimespec;
#else
                const struct timespec& mt = st.st_mtim;
#endif
                o->mtime_ns =
                    static_cast<std::uint64_t>(mt.tv_sec) * 1000000000ull +
                    static_cast<std::uint64_t>(mt.tv_nsec);
                o->mode = static_cast<std::uint32_t>(st.st_mode);
            }
        }(fd, out));
}

::dftu_task* io_readv(void* h, int fd, const struct iovec* iov, int iovcnt,
                      std::int64_t* out_n) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, const struct iovec* v, int c,
           std::int64_t* on) -> coro::CoroTask<void> {
            ssize_t n = co_await io::readv(f, v, c);
            if (on) *on = n;
        }(fd, iov, iovcnt, out_n));
}

::dftu_task* io_writev(void* h, int fd, const struct iovec* iov, int iovcnt,
                       std::int64_t* out_n) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, const struct iovec* v, int c,
           std::int64_t* on) -> coro::CoroTask<void> {
            ssize_t n = co_await io::writev(f, v, c);
            if (on) *on = n;
        }(fd, iov, iovcnt, out_n));
}

::dftu_task* io_preadv(void* h, int fd, const struct iovec* iov, int iovcnt,
                       std::uint64_t off, std::int64_t* out_n) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, const struct iovec* v, int c, std::uint64_t o,
           std::int64_t* on) -> coro::CoroTask<void> {
            ssize_t n = co_await io::preadv(f, v, c, static_cast<off_t>(o));
            if (on) *on = n;
        }(fd, iov, iovcnt, off, out_n));
}

::dftu_task* io_pwritev(void* h, int fd, const struct iovec* iov, int iovcnt,
                        std::uint64_t off, std::int64_t* out_n) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, const struct iovec* v, int c, std::uint64_t o,
           std::int64_t* on) -> coro::CoroTask<void> {
            ssize_t n = co_await io::pwritev(f, v, c, static_cast<off_t>(o));
            if (on) *on = n;
        }(fd, iov, iovcnt, off, out_n));
}

::dftu_task* io_lseek(void* h, int fd, std::int64_t off, int whence,
                      std::int64_t* out_off) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, std::int64_t o, int w,
           std::int64_t* oo) -> coro::CoroTask<void> {
            off_t r = co_await io::lseek(f, static_cast<off_t>(o), w);
            if (oo) *oo = static_cast<std::int64_t>(r);
        }(fd, off, whence, out_off));
}

::dftu_task* io_sendfile(void* h, int out_fd, int in_fd, std::uint64_t off,
                         std::uint64_t count, std::int64_t* out_n) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int ofd, int ifd, std::uint64_t o, std::uint64_t c,
           std::int64_t* on) -> coro::CoroTask<void> {
            ssize_t n = co_await io::sendfile(ofd, ifd, static_cast<off_t>(o),
                                              static_cast<std::size_t>(c));
            if (on) *on = n;
        }(out_fd, in_fd, off, count, out_n));
}

::dftu_task* io_accept(void* h, int fd, struct sockaddr* addr,
                       socklen_t* addrlen, int* out_fd) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, struct sockaddr* a, socklen_t* al,
           int* ofd) -> coro::CoroTask<void> {
            int cfd = static_cast<int>(co_await io::accept(f, a, al));
            if (ofd) *ofd = cfd;
        }(fd, addr, addrlen, out_fd));
}

::dftu_task* io_recv(void* h, int fd, void* buf, std::uint64_t len, int flags,
                     std::int64_t* out_n) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, void* b, std::uint64_t l, int fl,
           std::int64_t* on) -> coro::CoroTask<void> {
            ssize_t n =
                co_await io::recv(f, b, static_cast<std::size_t>(l), fl);
            if (on) *on = n;
        }(fd, buf, len, flags, out_n));
}

::dftu_task* io_send(void* h, int fd, const void* buf, std::uint64_t len,
                     int flags, std::int64_t* out_n) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](int f, const void* b, std::uint64_t l, int fl,
           std::int64_t* on) -> coro::CoroTask<void> {
            ssize_t n =
                co_await io::send(f, b, static_cast<std::size_t>(l), fl);
            if (on) *on = n;
        }(fd, buf, len, flags, out_n));
}

const dftu_io g_io = {io_open,   io_close,  io_read,      io_write, io_pread,
                      io_pwrite, io_fsync,  io_ftruncate, io_fstat, io_readv,
                      io_writev, io_preadv, io_pwritev,   io_lseek, io_sendfile,
                      io_accept, io_recv,   io_send};

// yield() reschedules fn onto a pool worker, so when_all over several spawns
// runs them concurrently instead of inline and serial.
::dftu_task* host_spawn(void* h, dftu_work_fn fn, void* arg) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](dftu_work_fn f, void* a) -> coro::CoroTask<void> {
            co_await coro::yield();
            if (f) f(a);
        }(fn, arg));
}

::dftu_task* host_when_all(void* h, ::dftu_task* const* ts, std::uint32_t n) {
    std::vector<coro::CoroTask<void>*> kids;
    kids.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) kids.push_back(as_task(ts[i]));
    return static_cast<PluginFold*>(h)->emplace_task(
        [](std::vector<coro::CoroTask<void>*> ks) -> coro::CoroTask<void> {
            std::vector<coro::CoroTask<void>> v;
            v.reserve(ks.size());
            for (auto* k : ks) v.push_back(std::move(*k));
            co_await coro::when_all(std::move(v));
        }(std::move(kids)));
}

::dftu_task* host_when_any(void* h, ::dftu_task* const* ts, std::uint32_t n) {
    std::vector<coro::CoroTask<void>*> kids;
    kids.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) kids.push_back(as_task(ts[i]));
    return static_cast<PluginFold*>(h)->emplace_task(
        [](std::vector<coro::CoroTask<void>*> ks) -> coro::CoroTask<void> {
            // when_any needs a non-void result, so wrap each void child.
            std::vector<coro::CoroTask<int>> v;
            v.reserve(ks.size());
            for (auto* k : ks)
                v.push_back(
                    [](coro::CoroTask<void>* kk) -> coro::CoroTask<int> {
                        co_await std::move(*kk);
                        co_return 0;
                    }(k));
            co_await coro::when_any(std::move(v));
        }(std::move(kids)));
}

::dftu_task* host_drive(void* h, ::dftu_task* (*step)(void*), void* coro) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](::dftu_task* (*s)(void*), void* c) -> coro::CoroTask<void> {
            while (::dftu_task* t = s(c)) co_await *as_task(t);
        }(step, coro));
}

::dftu_task* host_then(void* h, ::dftu_task* t, dftu_work_fn fn, void* arg) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](coro::CoroTask<void>* pre, dftu_work_fn f,
           void* a) -> coro::CoroTask<void> {
            coro::CoroTask<void> p = std::move(*pre);
            co_await p;
            if (f) f(a);
        }(as_task(t), fn, arg));
}

void host_run_blocking(void* /*h*/, dftu_work_fn fn, void* arg) {
    if (!fn) return;
    BlockingRegion block;
    fn(arg);
}

::dftu_op* host_compose_make(void* h, dftu_op_fn fn, void* state,
                             void (*free_state)(void*), dftu_type in_ty,
                             std::uint32_t in_size, dftu_type out_ty,
                             std::uint32_t out_size) {
    return static_cast<PluginFold*>(h)->compose_make(
        fn, state, free_state, in_ty, in_size, out_ty, out_size);
}
::dftu_op* host_compose_then(void* h, ::dftu_op* a, ::dftu_op* b) {
    return static_cast<PluginFold*>(h)->compose_then(a, b);
}
::dftu_op* host_compose_when_all(void* h, ::dftu_op* const* ops,
                                 std::uint32_t n) {
    return static_cast<PluginFold*>(h)->compose_when_all(ops, n);
}
::dftu_op* host_compose_when_any(void* h, ::dftu_op* const* ops,
                                 std::uint32_t n) {
    return static_cast<PluginFold*>(h)->compose_when_any(ops, n);
}
::dftu_task* host_compose_run(void* h, ::dftu_op* op, const void* in, void* out,
                              int* rc) {
    return static_cast<PluginFold*>(h)->compose_run(op, in, out, rc);
}
// Ops are arena-owned (scan-lifetime); early release is a no-op.
void host_compose_free_op(void* /*h*/, ::dftu_op* /*op*/) {}

::dftu_task* host_merge_shards(void* h, const char* target,
                               const char* const* shards, std::uint32_t n) {
    std::string tgt(target ? target : "");
    std::vector<std::string> sv;
    sv.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i)
        sv.emplace_back(shards[i] ? shards[i] : "");
    return static_cast<PluginFold*>(h)->emplace_task(
        [](std::string t, std::vector<std::string> s) -> coro::CoroTask<void> {
            co_await parallel::merge_shards(t, s);
        }(std::move(tgt), std::move(sv)));
}

::dftu_writer* host_writer_create(void* h, const char* path,
                                  std::uint32_t num_workers, int gzip) {
    return static_cast<PluginFold*>(h)->create_writer(path, num_workers, gzip);
}

::dftu_task* host_writer_open(void* h, ::dftu_writer* w) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](PluginWriter* pw) -> coro::CoroTask<void> {
            co_await pw->writer->open(pw->path, pw->num_workers, pw->gzip,
                                      nullptr);
        }(reinterpret_cast<PluginWriter*>(w)));
}

::dftu_task* host_writer_chunk(void* h, ::dftu_writer* w, std::uint32_t worker,
                               const void* data, std::uint64_t len) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](PluginWriter* pw, std::uint32_t wk, const void* d,
           std::uint64_t l) -> coro::CoroTask<void> {
            ByteView bv(static_cast<const unsigned char*>(d),
                        static_cast<std::size_t>(l));
            co_await pw->writer->write_chunk(static_cast<std::size_t>(wk), bv);
        }(reinterpret_cast<PluginWriter*>(w), worker, data, len));
}

::dftu_task* host_writer_close(void* h, ::dftu_writer* w) {
    return static_cast<PluginFold*>(h)->emplace_task(
        [](PluginWriter* pw) -> coro::CoroTask<void> {
            co_await pw->writer->close();
        }(reinterpret_cast<PluginWriter*>(w)));
}

// DDSketch lacks a mean, so the box tracks the running weighted sum.
struct SketchBox {
    utilities::common::statistics::DDSketch sketch;
    double sum = 0.0;
};

::dftu_sketch* host_sketch_create(void*) {
    return reinterpret_cast<::dftu_sketch*>(new (std::nothrow) SketchBox());
}

void host_sketch_add(void*, ::dftu_sketch* s, double v, double w) {
    if (!s) return;
    auto* b = reinterpret_cast<SketchBox*>(s);
    b->sketch.add(v, w);
    b->sum += v * w;
}

void host_sketch_merge(void*, ::dftu_sketch* into, const ::dftu_sketch* other) {
    if (!into || !other) return;
    auto* a = reinterpret_cast<SketchBox*>(into);
    const auto* b = reinterpret_cast<const SketchBox*>(other);
    a->sketch.merge(b->sketch);
    a->sum += b->sum;
}

::dftu_quantiles host_sketch_result(void*, const ::dftu_sketch* s) {
    ::dftu_quantiles q{};
    if (!s) return q;
    const auto* b = reinterpret_cast<const SketchBox*>(s);
    q.count = b->sketch.count();
    q.min = b->sketch.min();
    q.max = b->sketch.max();
    q.mean = q.count ? b->sum / static_cast<double>(q.count) : 0.0;
    q.p50 = b->sketch.quantile(0.5);
    q.p90 = b->sketch.quantile(0.9);
    q.p95 = b->sketch.quantile(0.95);
    q.p99 = b->sketch.quantile(0.99);
    return q;
}

void host_sketch_free(void*, ::dftu_sketch* s) {
    delete reinterpret_cast<SketchBox*>(s);
}

void append_json_escaped(std::string& out, std::string_view s) {
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x",
                                  static_cast<unsigned>(c));
                    out += b;
                } else {
                    out += c;
                }
        }
    }
}

// Named-column access to a batch's dyn arg columns, resolved once per
// dftu_dataframe (not per row): each ("args.<key>", column) pair the frame
// carries. Used only for serializing a whole row's args (trace_write), which
// needs every dyn column, not a lookup by name (see plugins::Batch's
// per-batch column cache for the per-row-loop counterpart).
struct DynCols {
    std::vector<std::pair<std::string_view, dftu_series*>> cols;

    explicit DynCols(const dftu_dataframe* df) {
        static constexpr std::string_view ARGS_PREFIX = "args.";
        const std::int32_t n = dftu_dataframe_num_columns(df);
        for (std::int32_t i = 0; i < n; ++i) {
            const char* name = dftu_dataframe_column_name(df, i);
            if (!name) continue;
            std::string_view nv{name};
            if (nv.substr(0, ARGS_PREFIX.size()) != ARGS_PREFIX) continue;
            cols.emplace_back(nv.substr(ARGS_PREFIX.size()),
                              dftu_dataframe_column(df, name));
        }
    }
    ~DynCols() {
        for (auto& [name, c] : cols) dftu_series_free(c);
    }
    DynCols(const DynCols&) = delete;
    DynCols& operator=(const DynCols&) = delete;
};

void serialize_row(std::string& out, const dftu_dataframe* df,
                   const DynCols& dyn, std::int64_t row, std::uint64_t id) {
    auto str_col = [&](const char* name, std::string_view* out_sv) {
        dftu_series* c = dftu_dataframe_column(df, name);
        if (!c) return;
        if (dftu_series_type(c) == DFTU_TYPE_STRING &&
            !dftu_series_is_null(c, row)) {
            const auto* off = dftu_series_offsets(c);
            const char* base = static_cast<const char*>(dftu_series_data(c));
            if (off && base)
                *out_sv = {base + off[row],
                           static_cast<std::size_t>(off[row + 1] - off[row])};
        }
        dftu_series_free(c);
    };
    auto u64_col = [&](const char* name) -> std::uint64_t {
        dftu_series* c = dftu_dataframe_column(df, name);
        if (!c) return 0;
        std::uint64_t v = 0;
        if (dftu_series_type(c) == DFTU_TYPE_UINT64 &&
            !dftu_series_is_null(c, row))
            v = static_cast<const std::uint64_t*>(dftu_series_data(c))[row];
        dftu_series_free(c);
        return v;
    };
    auto i64_col = [&](const char* name, bool* present) -> std::int64_t {
        dftu_series* c = dftu_dataframe_column(df, name);
        if (!c) return 0;
        std::int64_t v = 0;
        if (dftu_series_type(c) == DFTU_TYPE_INT64 &&
            !dftu_series_is_null(c, row)) {
            v = static_cast<const std::int64_t*>(dftu_series_data(c))[row];
            if (present) *present = true;
        }
        dftu_series_free(c);
        return v;
    };

    std::string_view name_v, cat_v;
    str_col("name", &name_v);
    str_col("cat", &cat_v);
    bool has_ph = false;
    const std::int64_t ph = i64_col("ph", &has_ph);
    const std::uint64_t pid = u64_col("pid");
    const std::uint64_t tid = u64_col("tid");
    const std::uint64_t ts = u64_col("ts");
    const std::uint64_t dur = u64_col("dur");

    out += "{\"id\":";
    out += std::to_string(id);
    out += ",\"name\":\"";
    append_json_escaped(out, name_v);
    out += "\",\"cat\":\"";
    append_json_escaped(out, cat_v);
    out += "\",\"ph\":";
    out += std::to_string(has_ph ? ph
                                 : static_cast<std::int64_t>(DFTU_PH_UNKNOWN));
    out += ",\"pid\":";
    out += std::to_string(pid);
    out += ",\"tid\":";
    out += std::to_string(tid);
    out += ",\"ts\":";
    out += std::to_string(ts);
    if (has_ph && ph == DFTU_PH_COMPLETE) {
        out += ",\"dur\":";
        out += std::to_string(dur);
    }
    bool args_open = false;
    for (const auto& [key, c] : dyn.cols) {
        if (dftu_series_is_null(c, row)) continue;
        if (!args_open) {
            out += ",\"args\":{";
            args_open = true;
        } else {
            out += ',';
        }
        out += '"';
        append_json_escaped(out, key);
        out += "\":";
        switch (dftu_series_type(c)) {
            case DFTU_TYPE_STRING: {
                const auto* off = dftu_series_offsets(c);
                const char* base =
                    static_cast<const char*>(dftu_series_data(c));
                std::string_view sv =
                    off && base ? std::string_view{base + off[row],
                                                   static_cast<std::size_t>(
                                                       off[row + 1] - off[row])}
                                : std::string_view{};
                out += '"';
                append_json_escaped(out, sv);
                out += '"';
                break;
            }
            case DFTU_TYPE_INT64:
                out += std::to_string(
                    static_cast<const std::int64_t*>(dftu_series_data(c))[row]);
                break;
            case DFTU_TYPE_FLOAT64: {
                char b[32];
                std::snprintf(
                    b, sizeof(b), "%.17g",
                    static_cast<const double*>(dftu_series_data(c))[row]);
                out += b;
                break;
            }
            default:
                out += "null";
                break;
        }
    }
    if (args_open) out += '}';
    out += "}\n";
}

int host_arrow_write_ipc(void*, ::ArrowArray* a, ::ArrowSchema* s,
                         const char* path) {
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    if (!a || !s || !path || !a->release || !s->release) return -1;
    namespace arr = utilities::common::arrow;
    try {
        nanoarrow::UniqueSchema schema;
        ArrowSchemaMove(s, schema.get());
        nanoarrow::UniqueArray array;
        ArrowArrayMove(a, array.get());
        arr::ArrowExportResult res(std::move(schema), std::move(array));
        int rc = -1;
        std::string p(path);
        dftracer::utils::default_runtime().run_blocking(
            "dft-plugin-ipc", [&](CoroScope&) -> coro::CoroTask<void> {
                arr::IpcWriter writer;
                if (co_await writer.open(p) != 0) co_return;
                if (co_await writer.write_batch(res) != 0) co_return;
                if (co_await writer.close() != 0) co_return;
                rc = 0;
            });
        ArrowSchemaMove(res.get_schema(), s);
        ArrowArrayMove(res.get_array(), a);
        return rc;
    } catch (...) {
        return -1;
    }
#else
    (void)a;
    (void)s;
    (void)path;
    return -1;
#endif
}

// Read the first record batch of an IPC file into out/out_schema; the caller
// must release both.
int host_arrow_read_ipc(void*, const char* path, ::ArrowArray* out,
                        ::ArrowSchema* out_schema) {
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    if (!path || !out || !out_schema) return -1;
    namespace arr = utilities::common::arrow;
    try {
        arr::IpcReader reader;
        if (reader.open(std::string(path)) != 0) return -1;
        if (reader.num_batches() == 0) return -1;
        arr::ArrowExportResult res = reader.read_batch(0);
        if (!res.get_array() || !res.get_schema() ||
            !res.get_array()->release || !res.get_schema()->release)
            return -1;
        ArrowArrayMove(res.get_array(), out);
        ArrowSchemaMove(res.get_schema(), out_schema);
        return 0;
    } catch (...) {
        return -1;
    }
#else
    (void)path;
    (void)out;
    (void)out_schema;
    return -1;
#endif
}

::dftu_trace_writer* host_trace_open_write(void* h, const char* path) {
    return static_cast<PluginFold*>(h)->create_trace_writer(path);
}

int host_trace_write(void*, ::dftu_trace_writer* w, const dftu_dataframe* df) {
    if (!w || !df) return -1;
    auto* tw = reinterpret_cast<PluginTraceWriter*>(w);
    try {
        DynCols dyn(df);
        const std::int64_t n = dftu_dataframe_num_rows(df);
        for (std::int64_t i = 0; i < n; ++i)
            serialize_row(tw->buffer, df, dyn, i, tw->next_id++);
        return 0;
    } catch (...) {
        return -1;
    }
}

int host_trace_close(void* h, ::dftu_trace_writer* w) {
    return static_cast<PluginFold*>(h)->close_trace_writer(w);
}

const dftu_svc_writer g_writer = {host_merge_shards, host_writer_create,
                                  host_writer_open, host_writer_chunk,
                                  host_writer_close};

const dftu_svc_sketch g_sketch = {host_sketch_create, host_sketch_add,
                                  host_sketch_merge, host_sketch_result,
                                  host_sketch_free};

const dftu_svc_arrow g_arrow = {host_arrow_write_ipc, host_arrow_read_ipc};

int host_trace_read(void* h, const char* path, dftu_stream_item_fn on_batch,
                    void* ud) {
    if (!path || !on_batch) return -1;
    auto& intern = static_cast<PluginFold*>(h)->intern_table();
    namespace views = trace::views;
    try {
        std::string p(path);
        dftracer::utils::default_runtime().run_blocking(
            "dft-plugin-trace-read", [&](CoroScope&) -> coro::CoroTask<void> {
                simdjson::dom::parser parser;
                std::vector<views::detail::FoldEvent> batch_events;
                views::View v = views::View::from_file(p);
                co_await v.for_each_batch(
                    [&](std::size_t,
                        const std::vector<std::string_view>& events) {
                        batch_events.clear();
                        batch_events.reserve(events.size());
                        for (std::string_view ev : events) {
                            simdjson::padded_string ps(ev);
                            simdjson::dom::element root;
                            if (parser.parse(ps).get(root)) continue;
                            batch_events.push_back(
                                views::detail::extract_fold_event(
                                    root, intern, /*needs_args=*/true));
                        }
                        if (batch_events.empty()) return;
                        dataframe::DataFrame df =
                            views::detail::build_row_frame(batch_events, intern,
                                                           {}, 1.0, nullptr);
                        std::vector<dftu_series*> handles;
                        handles.reserve(df.columns.size());
                        std::vector<const char*> names;
                        names.reserve(df.names.size());
                        for (const auto& c : df.columns)
                            handles.push_back(dftu_series_share(c.handle()));
                        for (const auto& n : df.names)
                            names.push_back(n.c_str());
                        dftu_dataframe* cdf = dftu_dataframe_new(
                            names.data(), handles.data(),
                            static_cast<std::int32_t>(handles.size()));
                        on_batch(cdf, ud);
                        dftu_dataframe_free(cdf);
                    },
                    /*num_slots=*/1, /*limit=*/0);
            });
        return 0;
    } catch (...) {
        return -1;
    }
}

const dftu_svc_trace g_trace = {host_trace_open_write, host_trace_write,
                                host_trace_close, host_trace_read};

dftu_query* host_query_compile(void* h, const char* src, std::uint32_t len) {
    return static_cast<PluginFold*>(h)->compile_query(src, len);
}

int host_query_matches(void* h, const dftu_query* q, const dftu_dataframe* df,
                       std::int64_t row) {
    if (!q || !df) return 0;
    try {
        return static_cast<PluginFold*>(h)->match_query(
            *reinterpret_cast<const Query*>(q), df, row);
    } catch (...) {
        return 0;
    }
}

std::uint64_t host_port_key(void*, const char* cap_id) {
    return cap_id ? dftracer::utils::hash::fnv1a_hash(cap_id) : 0;
}

void host_port_publish(void* h, std::uint64_t key, const void* data,
                       std::uint32_t len) {
    static_cast<PluginFold*>(h)->port_publish(key, data, len);
}

const void* host_port_consume(void* h, std::uint64_t key,
                              std::uint32_t* out_len) {
    return static_cast<PluginFold*>(h)->port_consume(key, out_len);
}

const dftu_svc_ports g_ports = {host_port_key, host_port_publish,
                                host_port_consume};

const dftu_svc_coro g_coro = {host_spawn, host_when_all, host_when_any,
                              host_then,  host_drive,    host_run_blocking};

const dftu_svc_query g_query = {host_query_compile, host_query_matches};

const dftu_svc_compose g_compose = {
    host_compose_make,     host_compose_then, host_compose_when_all,
    host_compose_when_any, host_compose_run,  host_compose_free_op};

// Stateless ext tables share one instance across all folds (each fn takes h).
const void* host_get_service(void*, const char* ext_id) {
    if (!ext_id) return nullptr;
    if (std::strcmp(ext_id, DFTU_SVC_IO) == 0) return &g_io;
    if (std::strcmp(ext_id, DFTU_SVC_CORO) == 0) return &g_coro;
    if (std::strcmp(ext_id, DFTU_SVC_QUERY) == 0) return &g_query;
    if (std::strcmp(ext_id, DFTU_SVC_COMPOSE) == 0) return &g_compose;
    if (std::strcmp(ext_id, DFTU_SVC_WRITER) == 0) return &g_writer;
    if (std::strcmp(ext_id, DFTU_SVC_SKETCH) == 0) return &g_sketch;
    if (std::strcmp(ext_id, DFTU_SVC_ARROW) == 0) return &g_arrow;
    if (std::strcmp(ext_id, DFTU_SVC_TRACE) == 0) return &g_trace;
    if (std::strcmp(ext_id, DFTU_SVC_PORTS) == 0) return &g_ports;
    if (std::strcmp(ext_id, DFTU_SVC_RESULT) == 0)
        return detail::result_ext_vtable();
    if (std::strcmp(ext_id, DFTU_SVC_AGG) == 0) return detail::agg_ext_vtable();
    if (std::strcmp(ext_id, DFTU_SVC_OPS) == 0) return detail::ops_ext_vtable();
    if (std::strcmp(ext_id, DFTU_SVC_PROVIDERS) == 0)
        return detail::providers_ext_vtable();
    if (std::strcmp(ext_id, DFTU_SVC_NODES) == 0)
        return detail::nodes_ext_vtable();
    return nullptr;
}

}  // namespace

::dftu_task* PluginFold::emplace_task(coro::CoroTask<void>&& task) {
    try {
        task_arena_.push_back(
            std::make_unique<coro::CoroTask<void>>(std::move(task)));
        return reinterpret_cast<::dftu_task*>(task_arena_.back().get());
    } catch (...) {
        return nullptr;
    }
}

namespace {
// Evaluate a compose graph over `in`, writing `out`; returns the leaf rc, with
// the first non-zero short-circuiting a pipe. Intermediate/child buffers are
// sized by the ops' declared value sizes.
coro::CoroTask<int> eval_compose(PluginFold* self, ComposeOp* op,
                                 const void* in, void* out) {
    using Kind = ComposeOp::Kind;
    if (op->kind == Kind::LEAF) {
        int rc = 0;
        ::dftu_task* t = op->fn ? op->fn(op->state, in, out, &rc) : nullptr;
        if (t) co_await *as_task(t);
        co_return rc;
    }
    if (op->kind == Kind::THEN) {
        ComposeOp* a = op->children[0];
        ComposeOp* b = op->children[1];
        std::vector<char> mid(a->out_size);
        int rc = co_await eval_compose(self, a, in, mid.data());
        if (rc != 0) co_return rc;
        co_return co_await eval_compose(self, b, mid.data(), out);
    }
    if (op->kind == Kind::WHEN_ALL) {
        // Each child writes its own disjoint slice of the concatenated output.
        std::vector<coro::CoroTask<int>> kids;
        kids.reserve(op->children.size());
        std::size_t off = 0;
        for (ComposeOp* c : op->children) {
            char* slot = static_cast<char*>(out) + off;
            kids.push_back(eval_compose(self, c, in, slot));
            off += c->out_size;
        }
        std::vector<int> rcs = co_await coro::when_all(std::move(kids));
        for (int r : rcs)
            if (r != 0) co_return r;
        co_return 0;
    }
    // WHEN_ANY: each child writes its own buffer; copy the winner into out.
    std::vector<std::vector<char>> bufs(op->children.size());
    std::vector<coro::CoroTask<int>> kids;
    kids.reserve(op->children.size());
    for (std::size_t i = 0; i < op->children.size(); ++i) {
        bufs[i].resize(op->children[i]->out_size);
        kids.push_back(eval_compose(self, op->children[i], in, bufs[i].data()));
    }
    auto winner = co_await coro::when_any(std::move(kids));
    if (op->out_size) std::memcpy(out, bufs[winner.index].data(), op->out_size);
    co_return winner.result;
}

}  // namespace

::dftu_op* PluginFold::compose_make(dftu_op_fn fn, void* state,
                                    void (*free_state)(void*), dftu_type in_ty,
                                    std::uint32_t in_size, dftu_type out_ty,
                                    std::uint32_t out_size) {
    try {
        compose_ops_.emplace_back();
        ComposeOp& op = compose_ops_.back();
        op.kind = ComposeOp::Kind::LEAF;
        op.fn = fn;
        op.state = state;
        op.free_state = free_state;
        op.in_ty = in_ty;
        op.out_ty = out_ty;
        op.in_size = in_size;
        op.out_size = out_size;
        return reinterpret_cast<::dftu_op*>(&op);
    } catch (...) {
        if (free_state) free_state(state);  // never leak the plugin's state
        return nullptr;
    }
}

::dftu_op* PluginFold::compose_then(::dftu_op* a, ::dftu_op* b) {
    if (!a || !b) return nullptr;
    ComposeOp* oa = reinterpret_cast<ComposeOp*>(a);
    ComposeOp* ob = reinterpret_cast<ComposeOp*>(b);
    if (oa->out_ty != ob->in_ty || oa->out_size != ob->in_size) {
        // Return null per the C ABI, but log so a mistyped pipe is not silent.
        DFTRACER_UTILS_LOG_WARN(
            "compose then: cannot pipe, value mismatch (a out ty=%d/%uB -> b "
            "in "
            "ty=%d/%uB)",
            static_cast<int>(oa->out_ty), oa->out_size,
            static_cast<int>(ob->in_ty), ob->in_size);
        return nullptr;
    }
    try {
        compose_ops_.emplace_back();
        ComposeOp& op = compose_ops_.back();
        op.kind = ComposeOp::Kind::THEN;
        op.in_ty = oa->in_ty;
        op.out_ty = ob->out_ty;
        op.in_size = oa->in_size;
        op.out_size = ob->out_size;
        op.children = {oa, ob};
        return reinterpret_cast<::dftu_op*>(&op);
    } catch (...) {
        return nullptr;
    }
}

::dftu_op* PluginFold::compose_when_all(::dftu_op* const* ops,
                                        std::uint32_t n) {
    if (!ops || n == 0) return nullptr;
    std::vector<ComposeOp*> kids;
    kids.reserve(n);
    ComposeOp* first = reinterpret_cast<ComposeOp*>(ops[0]);
    std::uint32_t out_total = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        ComposeOp* c = reinterpret_cast<ComposeOp*>(ops[i]);
        // All fan the same input; the concatenated output is an opaque blob.
        if (!c || c->in_ty != first->in_ty || c->in_size != first->in_size) {
            DFTRACER_UTILS_LOG_WARN(
                "compose when_all: op %u does not share the input type/size "
                "(want ty=%d/%uB)",
                i, static_cast<int>(first->in_ty), first->in_size);
            return nullptr;
        }
        kids.push_back(c);
        out_total += c->out_size;
    }
    try {
        compose_ops_.emplace_back();
        ComposeOp& op = compose_ops_.back();
        op.kind = ComposeOp::Kind::WHEN_ALL;
        op.in_ty = first->in_ty;
        op.out_ty = DFTU_T_BYTES;
        op.in_size = first->in_size;
        op.out_size = out_total;
        op.children = std::move(kids);
        return reinterpret_cast<::dftu_op*>(&op);
    } catch (...) {
        return nullptr;
    }
}

::dftu_op* PluginFold::compose_when_any(::dftu_op* const* ops,
                                        std::uint32_t n) {
    if (!ops || n == 0) return nullptr;
    ComposeOp* first = reinterpret_cast<ComposeOp*>(ops[0]);
    std::vector<ComposeOp*> kids;
    kids.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        ComposeOp* c = reinterpret_cast<ComposeOp*>(ops[i]);
        if (!c || c->in_ty != first->in_ty || c->in_size != first->in_size ||
            c->out_ty != first->out_ty || c->out_size != first->out_size) {
            DFTRACER_UTILS_LOG_WARN(
                "compose when_any: op %u does not share the input+output "
                "type/size of the racers",
                i);
            return nullptr;  // racers must share one input and output type/size
        }
        kids.push_back(c);
    }
    try {
        compose_ops_.emplace_back();
        ComposeOp& op = compose_ops_.back();
        op.kind = ComposeOp::Kind::WHEN_ANY;
        op.in_ty = first->in_ty;
        op.out_ty = first->out_ty;
        op.in_size = first->in_size;
        op.out_size = first->out_size;
        op.children = std::move(kids);
        return reinterpret_cast<::dftu_op*>(&op);
    } catch (...) {
        return nullptr;
    }
}

::dftu_task* PluginFold::compose_run(::dftu_op* op, const void* in, void* out,
                                     int* rc) {
    if (!op) {
        if (rc) *rc = -1;
        return nullptr;
    }
    ComposeOp* graph = reinterpret_cast<ComposeOp*>(op);
    return emplace_task([](PluginFold* self, ComposeOp* g, const void* i,
                           void* o, int* r) -> coro::CoroTask<void> {
        int res = co_await eval_compose(self, g, i, o);
        if (r) *r = res;
    }(this, graph, in, out, rc));
}

::dftu_writer* PluginFold::create_writer(const char* path,
                                         std::uint32_t num_workers, int gzip) {
    try {
        auto pw = std::make_unique<PluginWriter>();
        pw->writer = parallel::make_sharded_writer();
        pw->path = path ? path : "";
        pw->num_workers = num_workers ? num_workers : 1;
        pw->gzip = gzip != 0;
        writers_.push_back(std::move(pw));
        return reinterpret_cast<::dftu_writer*>(writers_.back().get());
    } catch (...) {
        return nullptr;
    }
}

::dftu_trace_writer* PluginFold::create_trace_writer(const char* path) {
    if (!path) return nullptr;
    try {
        auto tw = std::make_unique<PluginTraceWriter>();
        tw->path = path;
        trace_writers_.push_back(std::move(tw));
        return reinterpret_cast<::dftu_trace_writer*>(
            trace_writers_.back().get());
    } catch (...) {
        return nullptr;
    }
}

// Compress the buffered NDJSON into whole-line gzip members and write a
// re-indexable .pfw.gz. The index is built lazily on first read, not here.
int PluginFold::close_trace_writer(::dftu_trace_writer* w) {
    auto* tw = reinterpret_cast<PluginTraceWriter*>(w);
    if (!tw) return -1;
    namespace pfw = utilities::fileio::parallel;
    namespace cmp = utilities::fileio::compress;
    int result = -1;
    try {
        constexpr std::size_t DEFAULT_FLUSH =
            constants::indexer::DEFAULT_CHECKPOINT_SIZE;
        constexpr std::size_t HEADROOM = 1 * 1024 * 1024;
        auto cw = pfw::make_writer_for_path(
            {tw->path, 1, DEFAULT_FLUSH, HEADROOM, /*gzip=*/true});
        const std::size_t member_size = cw.sizing.flush_threshold;
        auto writer = std::move(cw.writer);
        const std::string& buf = tw->buffer;
        bool ok = true;
        dftracer::utils::default_runtime().run_blocking(
            "dft-plugin-trace-write",
            [&](CoroScope& scope) -> coro::CoroTask<void> {
                if (co_await writer->open(tw->path, 1, true, &scope) != 0) {
                    ok = false;
                    co_return;
                }
                cmp::GzipMemberCompressor comp(6);
                std::vector<std::uint8_t> scratch;
                auto emit = [&](std::size_t begin,
                                std::size_t len) -> coro::CoroTask<bool> {
                    if (!comp.compress_member_into(scratch, buf.data() + begin,
                                                   len))
                        co_return false;
                    co_return co_await writer->write_chunk(
                        0,
                        ByteView(reinterpret_cast<const char*>(scratch.data()),
                                 scratch.size())) == 0;
                };
                std::size_t pos = 0;
                while (buf.size() - pos >= member_size) {
                    std::size_t cut = pos + member_size;
                    while (cut < buf.size() && buf[cut] != '\n') ++cut;
                    if (cut >= buf.size()) break;
                    ++cut;
                    if (!co_await emit(pos, cut - pos)) {
                        ok = false;
                        co_return;
                    }
                    pos = cut;
                }
                if (pos < buf.size() && !co_await emit(pos, buf.size() - pos))
                    ok = false;
                if (co_await writer->close() != 0) ok = false;
                if (ok && cw.layout.layout == pfw::FileLayout::SHARDED) {
                    auto shards = writer->output_paths();
                    if (co_await pfw::merge_shards(tw->path, shards) != 0)
                        ok = false;
                }
            });
        result = ok ? 0 : -1;
    } catch (...) {
        result = -1;
    }
    for (auto it = trace_writers_.begin(); it != trace_writers_.end(); ++it) {
        if (it->get() == tw) {
            trace_writers_.erase(it);
            break;
        }
    }
    return result;
}

PluginFold::PluginFold(const dftu_plugin* plugin,
                       dftracer::utils::StringIntern& intern,
                       SharedResultRegistry* results,
                       NamedResultRegistry* named_results,
                       std::string plugin_name, std::uint64_t memory_budget,
                       const StateRegistry* states)
    : plugin_(plugin),
      plugin_name_(std::move(plugin_name)),
      intern_(&intern),
      slice_(plugin->make_slice(plugin->self)),
      results_(results),
      named_results_(named_results),
      memory_budget_(memory_budget),
      states_(states) {
    if (states_) {
        state_accums_.reserve(states_->size());
        for (const RegisteredState& reg : *states_)
            state_accums_.push_back(
                std::make_unique<StateAccum>(reg, memory_budget_));
    }

    host_.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    host_.h = this;
    host_.get_service = host_get_service;
    host_.resolve = host_resolve;
    host_.intern = host_intern;
    host_.log = host_log;

    // An invalid filter is logged and left unset rather than aborting.
    const char* q =
        plugin_->plan_query ? plugin_->plan_query(plugin_->self) : nullptr;
    if (q && *q) {
        auto r = Query::from_string(q);
        if (r)
            query_ = std::move(*r);
        else
            DFTRACER_UTILS_LOG_ERROR("Plugin query '%s' is invalid: %s", q,
                                     r.error().message.c_str());
    }

    // Registered states are handed the SAME frame as the slice, and what they
    // read is invisible from the slice that declared the projection. A state
    // whose column was projected away would just see a NULL lookup and answer
    // wrongly, so a plugin with states keeps every column: a projection is an
    // optimisation, and losing it must never cost correctness.
    if (plugin_->reads && state_accums_.empty()) {
        const char* const* cols = plugin_->reads(plugin_->self);
        for (; cols && *cols; ++cols) projection_.emplace_back(*cols);
    }
}

PluginFold::~PluginFold() {
    if (slice_) plugin_->destroy_slice(slice_);
}

::dftu_query* PluginFold::compile_query(const char* src, std::uint32_t len) {
    if (!src) return nullptr;
    auto r = Query::from_string(std::string_view{src, len});
    if (!r) return nullptr;
    try {
        compiled_queries_.push_back(std::move(*r));
    } catch (...) {
        return nullptr;
    }
    return reinterpret_cast<::dftu_query*>(&compiled_queries_.back());
}

namespace {
// One-shot column lookup for dftu_svc_query::query_matches: this is called at
// most a few times per fold (a plugin testing an ad hoc predicate), not in the
// hot per-row loop, so resolving by name per call is fine here (contrast
// plugins::Batch, which caches columns once per batch for the on_batch loop).
std::string_view df_str_field(const dftu_dataframe* df, const char* name,
                              std::int64_t row) {
    dftu_series* c = dftu_dataframe_column(df, name);
    if (!c) return {};
    std::string_view out;
    if (dftu_series_type(c) == DFTU_TYPE_STRING &&
        !dftu_series_is_null(c, row)) {
        const auto* off = dftu_series_offsets(c);
        const char* base = static_cast<const char*>(dftu_series_data(c));
        if (off && base)
            out = {base + off[row],
                   static_cast<std::size_t>(off[row + 1] - off[row])};
    }
    dftu_series_free(c);
    return out;
}
}  // namespace

int PluginFold::match_query(const Query& q, const dftu_dataframe* df,
                            std::int64_t row) {
    match_qmap_.clear();
    for (std::string_view f : q.fields()) {
        if (f == "cat" || f == "name" || f == "fhash" || f == "hhash") {
            std::string s(df_str_field(df, std::string(f).c_str(), row));
            if (!s.empty()) match_qmap_[f] = std::move(s);
        } else if (f == "pid" || f == "tid" || f == "ts" || f == "dur") {
            dftu_series* c = dftu_dataframe_column(df, std::string(f).c_str());
            if (c) {
                if (dftu_series_type(c) == DFTU_TYPE_UINT64 &&
                    !dftu_series_is_null(c, row))
                    match_qmap_[f] =
                        static_cast<double>(static_cast<const std::uint64_t*>(
                            dftu_series_data(c))[row]);
                dftu_series_free(c);
            }
        } else {
            std::string key(strip_args_prefix(f));
            std::string col = std::string("args.") + key;
            dftu_series* c = dftu_dataframe_column(df, col.c_str());
            if (c) {
                if (dftu_series_is_null(c, row)) {
                    // no value for this row
                } else if (dftu_series_type(c) == DFTU_TYPE_FLOAT64) {
                    match_qmap_[f] =
                        static_cast<const double*>(dftu_series_data(c))[row];
                } else if (dftu_series_type(c) == DFTU_TYPE_INT64) {
                    match_qmap_[f] =
                        static_cast<double>(static_cast<const std::int64_t*>(
                            dftu_series_data(c))[row]);
                } else if (dftu_series_type(c) == DFTU_TYPE_STRING) {
                    const auto* off = dftu_series_offsets(c);
                    const char* base =
                        static_cast<const char*>(dftu_series_data(c));
                    if (off && base)
                        match_qmap_[f] = std::string(
                            base + off[row],
                            static_cast<std::size_t>(off[row + 1] - off[row]));
                }
                dftu_series_free(c);
            }
        }
    }
    return q.evaluate(match_qmap_) ? 1 : 0;
}

// Field coverage must match match_query() so plan_query and query_matches
// agree.
bool PluginFold::passes_query(const FoldEvent& ev) {
    trace::views::detail::PodSource src(ev, *intern_);
    qmap_.clear();
    for (std::string_view f : query_->fields()) {
        if (f == "cat" || f == "name" || f == "fhash" || f == "hhash") {
            std::string s = src.value(f);
            if (!s.empty()) qmap_[f] = std::move(s);
        } else if (auto n = src.number(f)) {
            qmap_[f] = *n;
        } else {
            std::string s = src.value(f);
            if (!s.empty()) qmap_[f] = std::move(s);
        }
    }
    return query_->evaluate(qmap_);
}

// Materialize the (query-passing, non-metadata) events of this batch into a
// native DataFrame and hand it across the ABI as a dftu_dataframe, so the
// plugin runs SIMD column ops instead of a per-event loop. build_row_frame is
// the same columnar materializer NativeRowFold uses.
void PluginFold::step(const FoldBatch& batch) {
    // A transform steps by rewriting the batch; on_batch is not called
    // beside it (the transform accumulates inside its own call).
    const bool has_transform = slice_ && plugin_->transform;
    const bool has_on_batch = slice_ && plugin_->on_batch && !has_transform;
    if (!has_on_batch && !has_transform && state_accums_.empty()) return;

    // The previous step's tasks were awaited by the fuse worker before this
    // call, so freeing them now is safe.
    task_arena_.clear();

    // A transform sees the whole batch, whatever it declared in reads.
    static const std::vector<std::string> every_column;
    const std::vector<std::string>& projection =
        has_transform ? every_column : projection_;

    dataframe::DataFrame df;
    if (port_bus_ && port_bus_->frame) {
        df = rewritten_batch(*port_bus_->frame, projection);
    } else {
        namespace views = dftracer::utils::trace::views::detail;
        col_scratch_.clear();
        for (const auto& fe : batch.events) {
            if (fe.phase == trace::RecordPhase::METADATA ||
                fe.phase == trace::RecordPhase::UNKNOWN)
                continue;
            if (query_ && !passes_query(fe)) continue;
            col_scratch_.push_back(fe);
        }
        if (col_scratch_.empty()) return;
        // An empty projection is build_row_frame's "every column", which for
        // a batch with many arg keys is a Series per key.
        df = views::build_row_frame(col_scratch_, *intern_, projection, 1.0,
                                    nullptr);
    }
    if (df.num_rows() == 0) return;

    // dftu_dataframe_new takes ownership of the column handles, so pass shared
    // copies (a refcount bump, no data copy); df keeps its own.
    std::vector<dftu_series*> handles;
    handles.reserve(df.columns.size());
    std::vector<const char*> names;
    names.reserve(df.names.size());
    for (const auto& c : df.columns)
        handles.push_back(dftu_series_share(c.handle()));
    for (const auto& n : df.names) names.push_back(n.c_str());

    dftu_dataframe* cdf =
        dftu_dataframe_new(names.data(), handles.data(),
                           static_cast<std::int32_t>(handles.size()));
    states_update(cdf);
    // The seam is contractually synchronous (abi.h: must return NULL); a
    // returned task would reference the now-freed cdf and cannot be driven.
    ::dftu_task* t = nullptr;
    if (has_transform) {
        if (dftu_dataframe* out = plugin_->transform(slice_, cdf, &host_)) {
            // Every fold after this one in the batch reads the rewrite; with
            // no bus (a fold driven alone) there is no one to hand it to.
            if (port_bus_)
                port_bus_->frame = dataframe::dataframe_handle_take(out);
            else
                dftu_dataframe_free(out);
        }
    } else if (has_on_batch) {
        t = plugin_->on_batch(slice_, cdf, &host_);
    }
    dftu_dataframe_free(cdf);
    if (t)
        DFTRACER_UTILS_LOG_ERROR(
            "[plugin:%s] on_batch returned a non-null task; the seam is "
            "synchronous, dropping it",
            plugin_name().empty() ? "plugin" : plugin_name().c_str());
    pending_ = nullptr;
    reclaim_slice();
}

// The batch a transform earlier in this step rewrote, cut down to this
// plugin's projection (by name; a column the rewrite dropped is absent, as
// reads documents) and to its plan_query, which the events path applied per
// event and here runs as a frame mask. A query the frame cannot evaluate
// (a field the rewrite removed, a pattern match) is reported once and the
// rows pass unfiltered rather than being dropped.
dataframe::DataFrame PluginFold::rewritten_batch(
    const dataframe::DataFrame& frame,
    const std::vector<std::string>& projection) {
    dataframe::DataFrame df;
    if (projection.empty()) {
        df.names = frame.names;
        for (const dataframe::Series& c : frame.columns)
            df.columns.push_back(c.share());
    } else {
        for (const std::string& name : projection) {
            for (std::size_t i = 0; i < frame.names.size(); ++i) {
                if (frame.names[i] != name) continue;
                df.names.push_back(name);
                df.columns.push_back(frame.columns[i].share());
                break;
            }
        }
    }
    if (!query_ || df.columns.empty()) return df;
    try {
        return df.filter(df.mask(*query_));
    } catch (const std::exception& e) {
        if (!query_on_rewrite_refused_) {
            query_on_rewrite_refused_ = true;
            DFTRACER_UTILS_LOG_ERROR(
                "[plugin:%s] its query cannot be applied to a batch an "
                "earlier plugin rewrote (%s); the rewritten rows pass "
                "unfiltered",
                plugin_name().empty() ? "plugin" : plugin_name().c_str(),
                e.what());
        }
        return df;
    }
}

void PluginFold::reclaim_slice() {
    if (!slice_ || !plugin_->bytes) return;
    const std::uint64_t budget =
        dftracer::utils::resolve_spill_budget(memory_budget_);
    if (budget == dftracer::utils::NO_SPILL_BUDGET) return;
    const std::uint64_t held = plugin_->bytes(slice_);
    if (held <= budget) return;
    const std::uint64_t freed =
        plugin_->reclaim ? plugin_->reclaim(slice_, held - budget) : 0;
    ++slice_reclaims_;
    if (freed == 0 && !reclaim_refused_) {
        reclaim_refused_ = true;
        DFTRACER_UTILS_LOG_ERROR(
            "[plugin:%s] slice holds %llu bytes past the %llu byte memory "
            "budget and %s, so it stays in memory and the budget is not "
            "honoured",
            plugin_name().empty() ? "plugin" : plugin_name().c_str(),
            static_cast<unsigned long long>(held),
            static_cast<unsigned long long>(budget),
            plugin_->reclaim ? "reclaim freed nothing" : "declares no reclaim");
    }
}

void PluginFold::merge(Fold& other) {
    auto& o = static_cast<PluginFold&>(other);
    states_merge(o);
    if (!slice_) return;
    if (o.slice_) plugin_->merge(slice_, o.slice_);
    slice_reclaims_ += o.slice_reclaims_;
    reclaim_refused_ = reclaim_refused_ || o.reclaim_refused_;
    reclaim_slice();

    // Fold each worker's aggregation accumulators into this master by name via
    // the engine's single merge path (agg_merge). An accumulator new to the
    // master is moved in whole; a shared name merges the two AggStates.
    for (const auto& [key, idx] : o.aggs_.index()) {
        std::unique_ptr<AggAccum>& src = o.aggs_[idx];
        if (!src || !src->state) continue;
        std::unique_ptr<AggAccum>* dst = aggs_.find(key);
        if (!dst) {
            aggs_.push(key, std::move(src));
        } else if ((*dst)->state) {
            dataframe::agg_merge(*(*dst)->state, *src->state);
            // The worker's spilled runs are part of its aggregate, so the
            // master takes them over with the in-memory half.
            (*dst)->spiller.adopt(src->spiller);
            (*dst)->spiller.maybe_spill((*dst)->state);
        }
    }
}

coro::CoroTask<bool> PluginFold::finalize(const CoverageSet&) {
    // Publish before on_finalize so a plugin finalizing later in fold order can
    // read this one's merged accumulators via agg_result().
    publish_aggs();
    materialize_aggs();
    states_finalize();
    if (slice_) {
        if (::dftu_task* t = plugin_->on_finalize(slice_, &host_))
            co_await *as_task(t);
        task_arena_.clear();
    }
    co_return false;
}

}  // namespace dftracer::utils::plugins
