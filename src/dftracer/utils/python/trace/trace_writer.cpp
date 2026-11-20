// SPDX-License-Identifier: MIT
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/writer.h>
#include <dftracer/utils/python/core/call.h>
#include <dftracer/utils/python/core/errors.h>
#include <dftracer/utils/python/core/method.h>
#include <dftracer/utils/python/core/type_helpers.h>
#include <dftracer/utils/python/trace/trace_writer.h>
#include <dftracer/utils/trace/writer/trace_writer.h>

#include <cstdint>

using dftracer::utils::python::core::guard;
using dftracer::utils::python::core::parse_kw;
using dftracer::utils::python::core::parse_kw_for;
using dftracer::utils::python::core::py_unwrap;
using dftracer::utils::python::core::release_gil;
using namespace dftracer::utils;
using namespace dftracer::utils::trace::writer;

namespace {

constexpr int MAX_NESTING = 200;

// The args of one call: the Arg list the writer takes, and the JSON text of
// the nested values, which the list refers to by offset until it is complete.
struct ArgsScratch {
    struct Nested {
        std::size_t arg;
        std::size_t offset;
        std::size_t size;
    };
    dstd::Vector<Arg> args;
    dstd::String json;
    dstd::Vector<Nested> nested;

    void clear() {
        args.clear();
        json.clear();
        nested.clear();
    }
};

struct WriterState {
    dstd::Optional<TraceWriter> writer;
    ArgsScratch scratch;
    bool busy = false;
};

typedef struct {
    PyObject_HEAD dstd::UniquePtr<WriterState> impl;
} TraceWriterObject;

// Marks the writer in use while the GIL is released, so a second thread that
// calls it meanwhile gets an error instead of a data race.
class BusyScope {
   public:
    explicit BusyScope(WriterState &impl) : impl_(impl) { impl_.busy = true; }
    ~BusyScope() { impl_.busy = false; }
    BusyScope(const BusyScope &) = delete;
    BusyScope &operator=(const BusyScope &) = delete;

   private:
    WriterState &impl_;
};

bool is_int(PyObject *v) { return PyLong_Check(v) || PyIndex_Check(v); }

// Reads an int that fits int64 or uint64; false with a Python error set.
bool read_int(PyObject *v, ArgValue &out) {
    PyObject *index = PyNumber_Index(v);
    if (!index) return false;
    int overflow = 0;
    const long long s = PyLong_AsLongLongAndOverflow(index, &overflow);
    if (overflow == 0) {
        Py_DECREF(index);
        if (s == -1 && PyErr_Occurred()) return false;
        out = ArgValue(static_cast<std::int64_t>(s));
        return true;
    }
    if (overflow < 0) {
        Py_DECREF(index);
        PyErr_SetString(PyExc_OverflowError, "int out of range for 64 bits");
        return false;
    }
    const unsigned long long u = PyLong_AsUnsignedLongLong(index);
    Py_DECREF(index);
    if (u == static_cast<unsigned long long>(-1) && PyErr_Occurred())
        return false;
    out = ArgValue(static_cast<std::uint64_t>(u));
    return true;
}

bool read_text(PyObject *v, dstd::StringView &out) {
    Py_ssize_t size = 0;
    const char *text = PyUnicode_AsUTF8AndSize(v, &size);
    if (!text) return false;
    out = dstd::StringView(text, static_cast<std::size_t>(size));
    return true;
}

PyObject *seq_item(PyObject *seq, Py_ssize_t i) {
    return PyList_Check(seq) ? PyList_GET_ITEM(seq, i)
                             : PyTuple_GET_ITEM(seq, i);
}

Py_ssize_t seq_size(PyObject *seq) {
    return PyList_Check(seq) ? PyList_GET_SIZE(seq) : PyTuple_GET_SIZE(seq);
}

// Checks that write_json can write `v`; false with a Python error set. The
// json::Writer ends the process when destroyed with a container open, so a
// failure must be found before the first one opens.
bool check_json(PyObject *v, int depth) {
    if (v == Py_None || PyBool_Check(v) || PyFloat_Check(v)) return true;
    if (is_int(v)) {
        ArgValue n;
        return read_int(v, n);
    }
    if (PyUnicode_Check(v)) {
        dstd::StringView s;
        return read_text(v, s);
    }
    const bool is_seq = PyList_Check(v) || PyTuple_Check(v);
    if (is_seq || PyDict_Check(v)) {
        if (depth >= MAX_NESTING) {
            PyErr_SetString(PyExc_ValueError, "args nested too deeply");
            return false;
        }
        if (is_seq) {
            const Py_ssize_t n = seq_size(v);
            for (Py_ssize_t i = 0; i < n; ++i)
                if (!check_json(seq_item(v, i), depth + 1)) return false;
            return true;
        }
        PyObject *key = nullptr;
        PyObject *item = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(v, &pos, &key, &item)) {
            dstd::StringView k;
            if (!PyUnicode_Check(key)) {
                PyErr_SetString(PyExc_TypeError, "args keys must be str");
                return false;
            }
            if (!read_text(key, k) || !check_json(item, depth + 1))
                return false;
        }
        return true;
    }
    PyErr_Format(PyExc_TypeError, "args value of type %.200s is not JSON",
                 Py_TYPE(v)->tp_name);
    return false;
}

// Writes a value that check_json accepted.
void write_json(json::Writer &w, PyObject *v) {
    if (v == Py_None) {
        w.null();
    } else if (PyBool_Check(v)) {
        w.value(v == Py_True);
    } else if (is_int(v)) {
        ArgValue n;
        read_int(v, n);
        if (n.kind == ArgValue::Kind::Int)
            w.value(n.i);
        else
            w.value(n.u);
    } else if (PyFloat_Check(v)) {
        write_double(w, PyFloat_AS_DOUBLE(v));
    } else if (PyUnicode_Check(v)) {
        dstd::StringView s;
        read_text(v, s);
        w.value(s);
    } else if (PyList_Check(v) || PyTuple_Check(v)) {
        w.begin_array();
        const Py_ssize_t n = seq_size(v);
        for (Py_ssize_t i = 0; i < n; ++i) write_json(w, seq_item(v, i));
        w.end_array();
    } else {
        w.begin_object();
        PyObject *key = nullptr;
        PyObject *item = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(v, &pos, &key, &item)) {
            dstd::StringView k;
            read_text(key, k);
            w.key(k);
            write_json(w, item);
        }
        w.end_object();
    }
}

// Fills the scratch from the args dict; false with a Python error set. The
// views the Arg list holds point into the dict's own strings, so the list is
// used only while the GIL is held.
bool read_args(PyObject *args, ArgsScratch &s) {
    s.clear();
    if (!args || args == Py_None) return true;
    if (!PyDict_Check(args)) {
        PyErr_SetString(PyExc_TypeError, "args must be a dict or None");
        return false;
    }
    PyObject *key = nullptr;
    PyObject *item = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(args, &pos, &key, &item)) {
        if (!PyUnicode_Check(key)) {
            PyErr_SetString(PyExc_TypeError, "args keys must be str");
            return false;
        }
        Arg arg;
        if (!read_text(key, arg.key)) return false;
        if (item == Py_None) {
            arg.value = ArgValue();
        } else if (PyBool_Check(item)) {
            arg.value = ArgValue(item == Py_True);
        } else if (is_int(item)) {
            if (!read_int(item, arg.value)) return false;
        } else if (PyFloat_Check(item)) {
            arg.value = ArgValue(PyFloat_AS_DOUBLE(item));
        } else if (PyUnicode_Check(item)) {
            dstd::StringView text;
            if (!read_text(item, text)) return false;
            arg.value = ArgValue(text);
        } else if (PyList_Check(item) || PyTuple_Check(item) ||
                   PyDict_Check(item)) {
            if (!check_json(item, 0)) return false;
            const std::size_t offset = s.json.size();
            {
                json::Writer w(s.json);
                write_json(w, item);
            }
            s.nested.push_back({s.args.size(), offset, s.json.size() - offset});
        } else {
            PyErr_Format(PyExc_TypeError,
                         "args value of type %.200s is not supported",
                         Py_TYPE(item)->tp_name);
            return false;
        }
        s.args.push_back(arg);
    }
    for (const auto &n : s.nested)
        s.args[n.arg].value =
            ArgValue::raw(dstd::StringView(s.json.data() + n.offset, n.size));
    return true;
}

bool read_str(PyObject *v, const char *what, dstd::StringView &out) {
    if (!PyUnicode_Check(v)) {
        PyErr_Format(PyExc_TypeError, "%s must be str", what);
        return false;
    }
    return read_text(v, out);
}

bool read_u64(PyObject *v, std::uint64_t &out) {
    if (!v) return true;
    const unsigned long long n = PyLong_AsUnsignedLongLong(v);
    if (n == static_cast<unsigned long long>(-1) && PyErr_Occurred())
        return false;
    out = n;
    return true;
}

WriterState *open_impl(TraceWriterObject *self) {
    if (!self->impl || !self->impl->writer) {
        PyErr_SetString(PyExc_ValueError, "TraceWriter is closed");
        return nullptr;
    }
    if (self->impl->busy) {
        PyErr_SetString(PyExc_RuntimeError,
                        "TraceWriter is in use by another thread");
        return nullptr;
    }
    return self->impl.get();
}

void TraceWriter_dealloc(TraceWriterObject *self) {
    self->impl.reset();
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

PyObject *TraceWriter_new(PyTypeObject *type, PyObject *, PyObject *) {
    auto *self = reinterpret_cast<TraceWriterObject *>(type->tp_alloc(type, 0));
    if (self) new (&self->impl) dstd::UniquePtr<WriterState>();
    return reinterpret_cast<PyObject *>(self);
}

int TraceWriter_init(TraceWriterObject *self, PyObject *args, PyObject *kwds) {
    PyObject *path_obj = nullptr;
    int level = 6;
    Py_ssize_t threads = 0;
    Py_ssize_t member_size = 4 * 1024 * 1024;
    int compress = 1;
    if (!parse_kw<"O|$innp:TraceWriter">(
            args, kwds, {"path", "level", "threads", "member_size", "compress"},
            &path_obj, &level, &threads, &member_size, &compress))
        return -1;
    if (threads < 0 || member_size <= 0) {
        PyErr_SetString(PyExc_ValueError,
                        "threads must be >= 0 and member_size > 0");
        return -1;
    }
    dstd::String path;
    {
        PyObject *fs = PyOS_FSPath(path_obj);
        if (!fs) return -1;
        dstd::StringView text;
        const bool ok = PyUnicode_Check(fs) && read_text(fs, text);
        if (ok) path.assign(text.data(), text.size());
        if (!ok && !PyErr_Occurred())
            PyErr_SetString(PyExc_TypeError, "path must be str or PathLike");
        Py_DECREF(fs);
        if (!ok) return -1;
    }

    TraceWriterOptions opts;
    opts.level = level;
    opts.workers = static_cast<std::size_t>(threads);
    opts.member_size = static_cast<std::size_t>(member_size);
    opts.compress = compress != 0;
    return guard([&] {
        auto impl = dstd::make_unique<WriterState>();
        impl->writer.emplace(py_unwrap(TraceWriter::open(path, opts)));
        self->impl = std::move(impl);
        return 0;
    });
}

// Runs the pending file writes of the writer with the GIL released.
bool drain_if_needed(WriterState &impl) {
    if (!impl.writer->needs_drain()) return true;
    return guard([&] {
        auto drained = [&] {
            BusyScope scope(impl);
            return release_gil([&] { return impl.writer->drain(); });
        }();
        py_unwrap(std::move(drained));
        return true;
    });
}

PyObject *TraceWriter_write_event(TraceWriterObject *self, PyObject *args,
                                  PyObject *kwds) {
    PyObject *name_obj = nullptr, *cat_obj = nullptr;
    PyObject *ts_obj = nullptr, *dur_obj = nullptr, *pid_obj = nullptr,
             *tid_obj = nullptr, *args_obj = nullptr;
    if (!parse_kw<"OOOO|OOO:write_event">(
            args, kwds, {"name", "cat", "ts", "dur", "pid", "tid", "args"},
            &name_obj, &cat_obj, &ts_obj, &dur_obj, &pid_obj, &tid_obj,
            &args_obj))
        return nullptr;
    dstd::StringView name, cat;
    std::uint64_t ts = 0, dur = 0, pid = 0, tid = 0;
    if (!read_str(name_obj, "name", name) || !read_str(cat_obj, "cat", cat) ||
        !read_u64(ts_obj, ts) || !read_u64(dur_obj, dur) ||
        !read_u64(pid_obj, pid) || !read_u64(tid_obj, tid))
        return nullptr;
    WriterState *impl = open_impl(self);
    // The drain releases the GIL, so it comes before read_args takes views
    // into the caller's strings; nothing releases it from there to the write.
    if (!impl || !drain_if_needed(*impl) || !read_args(args_obj, impl->scratch))
        return nullptr;
    return guard([&]() -> PyObject * {
        py_unwrap(impl->writer->write_event(name, cat, ts, dur, pid, tid,
                                            impl->scratch.args));
        Py_RETURN_NONE;
    });
}

// write_counter and write_aggregated share the argument shape.
template <bool Aggregated>
PyObject *write_series(TraceWriterObject *self, PyObject *args,
                       PyObject *kwds) {
    const char *func = Aggregated ? "write_aggregated" : "write_counter";
    PyObject *name_obj = nullptr, *cat_obj = nullptr, *ts_obj = nullptr,
             *pid_obj = nullptr, *tid_obj = nullptr, *args_obj = nullptr;
    if (!parse_kw_for<"OOO|OOO">(
            func, args, kwds, {"name", "cat", "ts", "args", "pid", "tid"},
            &name_obj, &cat_obj, &ts_obj, &args_obj, &pid_obj, &tid_obj))
        return nullptr;
    dstd::StringView name, cat;
    std::uint64_t ts = 0, pid = 0, tid = 0;
    if (!read_str(name_obj, "name", name) || !read_str(cat_obj, "cat", cat) ||
        !read_u64(ts_obj, ts) || !read_u64(pid_obj, pid) ||
        !read_u64(tid_obj, tid))
        return nullptr;
    WriterState *impl = open_impl(self);
    // Drain before read_args: see write_event.
    if (!impl || !drain_if_needed(*impl) || !read_args(args_obj, impl->scratch))
        return nullptr;
    return guard([&]() -> PyObject * {
        if constexpr (Aggregated)
            py_unwrap(impl->writer->write_aggregated(name, cat, ts, pid, tid,
                                                     impl->scratch.args));
        else
            py_unwrap(impl->writer->write_counter(name, cat, ts, pid, tid,
                                                  impl->scratch.args));
        Py_RETURN_NONE;
    });
}

PyObject *TraceWriter_write_counter(TraceWriterObject *self, PyObject *args,
                                    PyObject *kwds) {
    return write_series<false>(self, args, kwds);
}

PyObject *TraceWriter_write_aggregated(TraceWriterObject *self, PyObject *args,
                                       PyObject *kwds) {
    return write_series<true>(self, args, kwds);
}

PyObject *TraceWriter_write_metadata(TraceWriterObject *self, PyObject *args,
                                     PyObject *kwds) {
    PyObject *name_obj = nullptr;
    PyObject *pid_obj = nullptr, *tid_obj = nullptr, *args_obj = nullptr;
    if (!parse_kw<"O|OOO:write_metadata">(
            args, kwds, {"name", "args", "pid", "tid"}, &name_obj, &args_obj,
            &pid_obj, &tid_obj))
        return nullptr;
    dstd::StringView name;
    std::uint64_t pid = 0, tid = 0;
    if (!read_str(name_obj, "name", name) || !read_u64(pid_obj, pid) ||
        !read_u64(tid_obj, tid))
        return nullptr;
    WriterState *impl = open_impl(self);
    // Drain before read_args: see write_event.
    if (!impl || !drain_if_needed(*impl) || !read_args(args_obj, impl->scratch))
        return nullptr;
    return guard([&]() -> PyObject * {
        py_unwrap(
            impl->writer->write_metadata(name, pid, tid, impl->scratch.args));
        Py_RETURN_NONE;
    });
}

PyObject *TraceWriter_write_raw(TraceWriterObject *self, PyObject *data) {
    WriterState *impl = open_impl(self);
    if (!impl) return nullptr;
    Py_buffer view{};
    dstd::StringView lines;
    bool has_buffer = false;
    if (PyUnicode_Check(data)) {
        if (!read_text(data, lines)) return nullptr;
    } else {
        if (PyObject_GetBuffer(data, &view, PyBUF_SIMPLE) < 0) return nullptr;
        has_buffer = true;
        lines = dstd::StringView(static_cast<const char *>(view.buf),
                                 static_cast<std::size_t>(view.len));
    }
    PyObject *result = guard([&]() -> PyObject * {
        BusyScope scope(*impl);
        py_unwrap(release_gil([&] { return impl->writer->write_raw(lines); }));
        Py_RETURN_NONE;
    });
    if (has_buffer) PyBuffer_Release(&view);
    return result;
}

PyObject *TraceWriter_close(TraceWriterObject *self, PyObject *) {
    if (!self->impl || !self->impl->writer) Py_RETURN_NONE;
    WriterState *impl = open_impl(self);
    if (!impl) return nullptr;
    return guard([&]() -> PyObject * {
        dstd::Result<void> closed = [&] {
            BusyScope scope(*impl);
            return release_gil([&] { return impl->writer->close(); });
        }();
        impl->writer.reset();
        py_unwrap(std::move(closed));
        Py_RETURN_NONE;
    });
}

PyObject *TraceWriter_discard(TraceWriterObject *self, PyObject *) {
    if (self->impl && self->impl->writer) {
        WriterState *impl = open_impl(self);
        if (!impl) return nullptr;
        impl->writer.reset();
    }
    Py_RETURN_NONE;
}

PyObject *TraceWriter_enter(TraceWriterObject *self, PyObject *) {
    Py_INCREF(self);
    return reinterpret_cast<PyObject *>(self);
}

PyObject *TraceWriter_exit(TraceWriterObject *self, PyObject *args) {
    PyObject *type =
        PyTuple_GET_SIZE(args) > 0 ? PyTuple_GET_ITEM(args, 0) : Py_None;
    PyObject *result = type == Py_None ? TraceWriter_close(self, nullptr)
                                       : TraceWriter_discard(self, nullptr);
    if (!result) return nullptr;
    Py_DECREF(result);
    Py_RETURN_FALSE;
}

PyMethodDef TraceWriter_methods[] = {
    {"write_event", DFTU_PYCFUNCTION(TraceWriter_write_event),
     METH_VARARGS | METH_KEYWORDS,
     "write_event(name, cat, ts, dur, pid=0, tid=0, args=None) -> None\n"
     "\n"
     "Write one complete event (ph 1)."},
    {"write_counter", DFTU_PYCFUNCTION(TraceWriter_write_counter),
     METH_VARARGS | METH_KEYWORDS,
     "write_counter(name, cat, ts, args=None, pid=0, tid=0) -> None\n"
     "\n"
     "Write one counter (ph 2)."},
    {"write_aggregated", DFTU_PYCFUNCTION(TraceWriter_write_aggregated),
     METH_VARARGS | METH_KEYWORDS,
     "write_aggregated(name, cat, ts, args=None, pid=0, tid=0) -> None\n"
     "\n"
     "Write one aggregated row (ph 3)."},
    {"write_metadata", DFTU_PYCFUNCTION(TraceWriter_write_metadata),
     METH_VARARGS | METH_KEYWORDS,
     "write_metadata(name, args=None, pid=0, tid=0) -> None\n"
     "\n"
     "Write one metadata record (ph 4, ts 0)."},
    {"write_raw", DFTU_PYCFUNCTION(TraceWriter_write_raw), METH_O,
     "write_raw(data) -> None\n"
     "\n"
     "Append whole lines as they are. `data` is bytes, bytearray,\n"
     "memoryview or str and must end with a newline."},
    {"close", DFTU_PYCFUNCTION(TraceWriter_close), METH_NOARGS,
     "close() -> None\n"
     "\n"
     "Finish the file. Raises the first write error. Safe to call twice."},
    {"discard", DFTU_PYCFUNCTION(TraceWriter_discard), METH_NOARGS,
     "discard() -> None\n"
     "\n"
     "Close the writer and remove its output."},
    {"__enter__", DFTU_PYCFUNCTION(TraceWriter_enter), METH_NOARGS,
     "Enter the context manager."},
    {"__exit__", DFTU_PYCFUNCTION(TraceWriter_exit), METH_VARARGS,
     "Close on success; discard the output when the block raised."},
    {nullptr, nullptr, 0, nullptr}};

PyTypeObject TraceWriterType = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0).tp_name =
        "dftracer_utils_ext.TraceWriter",
    .tp_basicsize = sizeof(TraceWriterObject),
    .tp_dealloc = reinterpret_cast<destructor>(TraceWriter_dealloc),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc =
        "TraceWriter(path, *, level=6, threads=0, member_size=4194304,\n"
        "            compress=True)\n"
        "--\n"
        "\n"
        "Writes DFTracer records, one JSON line each, to a gzip file of\n"
        "members of about member_size bytes, or to a plain file when\n"
        "compress is False. Not thread-safe: a call made while another\n"
        "thread is inside a write raises RuntimeError.\n",
    .tp_methods = TraceWriter_methods,
    .tp_init = reinterpret_cast<initproc>(TraceWriter_init),
    .tp_new = TraceWriter_new,
};

}  // namespace

int dftracer::utils::python::trace::init_trace_writer(PyObject *m) {
    return register_type(m, &TraceWriterType, "TraceWriter");
}
