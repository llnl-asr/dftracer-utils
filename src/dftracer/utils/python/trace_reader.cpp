#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/trace_reader.h>
#include <dftracer/utils/python/trace_reader_iterator.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>

#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using dftracer::utils::utilities::reader::ReadConfig;
using dftracer::utils::utilities::reader::TraceReader;
using dftracer::utils::utilities::reader::TraceReaderConfig;

CoroTask<void> produce_lines(std::shared_ptr<IteratorState> state,
                             TraceReaderConfig cfg, ReadConfig rc) {
    auto *sp = state.get();
    try {
        TraceReader reader(std::move(cfg));
        auto gen = reader.read_lines(rc);
        while (auto opt = co_await gen.next()) {
            if (sp->cancelled.load(std::memory_order_acquire)) break;
            std::string item(opt->content);
            {
                std::unique_lock<std::mutex> lock(sp->mtx);
                sp->cv_producer.wait(lock, [sp] {
                    return sp->queue.size() < sp->max_queue_size ||
                           sp->cancelled.load(std::memory_order_acquire);
                });
                if (sp->cancelled.load(std::memory_order_acquire)) break;
                sp->queue.push(std::move(item));
            }
            sp->cv_consumer.notify_one();
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->error = std::current_exception();
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
        sp->cv_consumer.notify_one();
        co_return;
    }
    {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
    }
    sp->cv_consumer.notify_one();
}

CoroTask<void> produce_raw(std::shared_ptr<IteratorState> state,
                           TraceReaderConfig cfg, ReadConfig rc) {
    auto *sp = state.get();
    try {
        TraceReader reader(std::move(cfg));
        auto gen = reader.read_raw(rc);
        while (auto opt = co_await gen.next()) {
            if (sp->cancelled.load(std::memory_order_acquire)) break;
            std::string item(opt->data(), opt->size());
            {
                std::unique_lock<std::mutex> lock(sp->mtx);
                sp->cv_producer.wait(lock, [sp] {
                    return sp->queue.size() < sp->max_queue_size ||
                           sp->cancelled.load(std::memory_order_acquire);
                });
                if (sp->cancelled.load(std::memory_order_acquire)) break;
                sp->queue.push(std::move(item));
            }
            sp->cv_consumer.notify_one();
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->error = std::current_exception();
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
        sp->cv_consumer.notify_one();
        co_return;
    }
    {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
    }
    sp->cv_consumer.notify_one();
}

TraceReaderConfig build_config(TraceReaderObject *self) {
    TraceReaderConfig cfg;
    cfg.file_path = PyUnicode_AsUTF8(self->file_path);
    const char *idx = PyUnicode_AsUTF8(self->index_dir);
    if (idx) cfg.index_dir = idx;
    cfg.checkpoint_size = self->checkpoint_size;
    cfg.auto_build_index = self->auto_build_index != 0;
    cfg.index_threshold = self->index_threshold;
    return cfg;
}

static Runtime *get_runtime(TraceReaderObject *self) {
    if (self->runtime_obj) {
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    }
    return get_default_runtime();
}

static TraceReaderIteratorObject *make_iterator(
    std::shared_ptr<IteratorState> state, IteratorMode mode) {
    TraceReaderIteratorObject *it =
        (TraceReaderIteratorObject *)TraceReaderIteratorType.tp_alloc(
            &TraceReaderIteratorType, 0);
    if (!it) return NULL;
    new (&it->state) std::shared_ptr<IteratorState>(std::move(state));
    it->mode = mode;
    return it;
}

}  // namespace

static void TraceReader_dealloc(TraceReaderObject *self) {
    Py_XDECREF(self->file_path);
    Py_XDECREF(self->index_dir);
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *TraceReader_new(PyTypeObject *type, PyObject *args,
                                 PyObject *kwds) {
    TraceReaderObject *self = (TraceReaderObject *)type->tp_alloc(type, 0);
    if (self) {
        self->file_path = NULL;
        self->index_dir = NULL;
        self->checkpoint_size = 32 * 1024 * 1024;
        self->auto_build_index = 0;
        self->index_threshold = 8 * 1024 * 1024;
        self->has_index = 0;
        self->runtime_obj = NULL;
    }
    return (PyObject *)self;
}

static int TraceReader_init(TraceReaderObject *self, PyObject *args,
                            PyObject *kwds) {
    static const char *kwlist[] = {"file_path",
                                   "index_dir",
                                   "checkpoint_size",
                                   "auto_build_index",
                                   "index_threshold",
                                   "runtime",
                                   NULL};

    const char *file_path;
    const char *index_dir = "";
    std::size_t checkpoint_size = 32 * 1024 * 1024;
    int auto_build_index = 0;
    std::size_t index_threshold = 8 * 1024 * 1024;
    PyObject *runtime_arg = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|snpnO", (char **)kwlist,
                                     &file_path, &index_dir, &checkpoint_size,
                                     &auto_build_index, &index_threshold,
                                     &runtime_arg)) {
        return -1;
    }

    if (runtime_arg && runtime_arg != Py_None) {
        if (!PyObject_TypeCheck(runtime_arg, &RuntimeType)) {
            PyErr_SetString(PyExc_TypeError,
                            "runtime must be a Runtime instance or None");
            return -1;
        }
        Py_INCREF(runtime_arg);
        self->runtime_obj = runtime_arg;
    }

    self->file_path = PyUnicode_FromString(file_path);
    if (!self->file_path) return -1;

    self->index_dir = PyUnicode_FromString(index_dir);
    if (!self->index_dir) {
        Py_DECREF(self->file_path);
        self->file_path = NULL;
        return -1;
    }

    self->checkpoint_size = checkpoint_size;
    self->auto_build_index = auto_build_index;
    self->index_threshold = index_threshold;

    try {
        TraceReaderConfig cfg;
        cfg.file_path = file_path;
        cfg.index_dir = index_dir;
        cfg.checkpoint_size = checkpoint_size;
        cfg.auto_build_index = auto_build_index != 0;
        cfg.index_threshold = index_threshold;
        TraceReader probe(std::move(cfg));
        self->has_index = probe.has_index() ? 1 : 0;
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        Py_DECREF(self->file_path);
        Py_DECREF(self->index_dir);
        self->file_path = NULL;
        self->index_dir = NULL;
        return -1;
    }

    return 0;
}

static PyObject *TraceReader_iter_lines(TraceReaderObject *self, PyObject *args,
                                        PyObject *kwds) {
    static const char *kwlist[] = {"start_line", "end_line",    "start_byte",
                                   "end_byte",   "buffer_size", NULL};
    Py_ssize_t start_line = 0, end_line = 0;
    Py_ssize_t start_byte = 0, end_byte = 0;
    Py_ssize_t buffer_size = 4 * 1024 * 1024;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|nnnnn", (char **)kwlist,
                                     &start_line, &end_line, &start_byte,
                                     &end_byte, &buffer_size)) {
        return NULL;
    }

    if (start_line < 0 || end_line < 0 || start_byte < 0 || end_byte < 0 ||
        buffer_size <= 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "range arguments must be >= 0; buffer_size must be > 0");
        return NULL;
    }

    TraceReaderConfig cfg;
    try {
        cfg = build_config(self);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    ReadConfig rc;
    rc.start_line = static_cast<std::size_t>(start_line);
    rc.end_line = static_cast<std::size_t>(end_line);
    rc.start_byte = static_cast<std::size_t>(start_byte);
    rc.end_byte = static_cast<std::size_t>(end_byte);
    rc.buffer_size = static_cast<std::size_t>(buffer_size);

    auto state = std::make_shared<IteratorState>();

    Runtime *rt = get_runtime(self);
    rt->schedule("iter_lines", produce_lines(state, cfg, rc));

    TraceReaderIteratorObject *it = make_iterator(state, IteratorMode::LINES);
    return (PyObject *)it;
}

static PyObject *TraceReader_iter_raw(TraceReaderObject *self, PyObject *args,
                                      PyObject *kwds) {
    static const char *kwlist[] = {"start_line", "end_line",    "start_byte",
                                   "end_byte",   "buffer_size", "line_aligned",
                                   "multi_line", NULL};
    Py_ssize_t start_line = 0, end_line = 0;
    Py_ssize_t start_byte = 0, end_byte = 0;
    Py_ssize_t buffer_size = 4 * 1024 * 1024;
    int line_aligned = 1;
    int multi_line = 1;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|nnnnnpp", (char **)kwlist, &start_line, &end_line,
            &start_byte, &end_byte, &buffer_size, &line_aligned, &multi_line)) {
        return NULL;
    }

    if (start_line < 0 || end_line < 0 || start_byte < 0 || end_byte < 0 ||
        buffer_size <= 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "range arguments must be >= 0; buffer_size must be > 0");
        return NULL;
    }

    TraceReaderConfig cfg;
    try {
        cfg = build_config(self);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    ReadConfig rc;
    rc.start_line = static_cast<std::size_t>(start_line);
    rc.end_line = static_cast<std::size_t>(end_line);
    rc.start_byte = static_cast<std::size_t>(start_byte);
    rc.end_byte = static_cast<std::size_t>(end_byte);
    rc.buffer_size = static_cast<std::size_t>(buffer_size);
    rc.line_aligned = line_aligned != 0;
    rc.multi_line = multi_line != 0;

    auto state = std::make_shared<IteratorState>();

    Runtime *rt = get_runtime(self);
    rt->schedule("iter_raw", produce_raw(state, cfg, rc));

    TraceReaderIteratorObject *it = make_iterator(state, IteratorMode::RAW);
    return (PyObject *)it;
}

static PyObject *TraceReader_read_lines(TraceReaderObject *self, PyObject *args,
                                        PyObject *kwds) {
    PyObject *iter = TraceReader_iter_lines(self, args, kwds);
    if (!iter) return NULL;
    PyObject *list = PySequence_List(iter);
    Py_DECREF(iter);
    return list;
}

static PyObject *TraceReader_read_raw(TraceReaderObject *self, PyObject *args,
                                      PyObject *kwds) {
    PyObject *iter = TraceReader_iter_raw(self, args, kwds);
    if (!iter) return NULL;
    PyObject *list = PySequence_List(iter);
    Py_DECREF(iter);
    return list;
}

static PyObject *TraceReader_enter(TraceReaderObject *self,
                                   PyObject *Py_UNUSED(ignored)) {
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *TraceReader_exit(TraceReaderObject *self, PyObject *args) {
    Py_RETURN_NONE;
}

static PyObject *TraceReader_get_file_path(TraceReaderObject *self,
                                           void *closure) {
    Py_INCREF(self->file_path);
    return self->file_path;
}

static PyObject *TraceReader_get_index_dir(TraceReaderObject *self,
                                           void *closure) {
    Py_INCREF(self->index_dir);
    return self->index_dir;
}

static PyObject *TraceReader_get_has_index(TraceReaderObject *self,
                                           void *closure) {
    return PyBool_FromLong(self->has_index);
}

static PyObject *TraceReader_get_num_lines(TraceReaderObject *self,
                                           void *closure) {
    PyObject *empty_args = PyTuple_New(0);
    if (!empty_args) return NULL;
    PyObject *list = TraceReader_read_lines(self, empty_args, NULL);
    Py_DECREF(empty_args);
    if (!list) return NULL;
    Py_ssize_t n = PyList_GET_SIZE(list);
    Py_DECREF(list);
    return PyLong_FromSsize_t(n);
}

static PyMethodDef TraceReader_methods[] = {
    {"iter_lines", (PyCFunction)TraceReader_iter_lines,
     METH_VARARGS | METH_KEYWORDS,
     "Return iterator over decoded lines "
     "(start_line=0, end_line=0, start_byte=0, end_byte=0, "
     "buffer_size=4M)"},
    {"iter_raw", (PyCFunction)TraceReader_iter_raw,
     METH_VARARGS | METH_KEYWORDS,
     "Return iterator over raw byte chunks "
     "(start_line=0, end_line=0, start_byte=0, end_byte=0, "
     "buffer_size=4M, line_aligned=True, multi_line=True)"},
    {"read_lines", (PyCFunction)TraceReader_read_lines,
     METH_VARARGS | METH_KEYWORDS,
     "Read lines and return list[str] "
     "(start_line=0, end_line=0, start_byte=0, end_byte=0, "
     "buffer_size=4M)"},
    {"read_raw", (PyCFunction)TraceReader_read_raw,
     METH_VARARGS | METH_KEYWORDS,
     "Read raw chunks and return list[bytes] "
     "(start_line=0, end_line=0, start_byte=0, end_byte=0, "
     "buffer_size=4M, line_aligned=True, multi_line=True)"},
    {"__enter__", (PyCFunction)TraceReader_enter, METH_NOARGS,
     "Enter the runtime context for the with statement"},
    {"__exit__", (PyCFunction)TraceReader_exit, METH_VARARGS,
     "Exit the runtime context for the with statement"},
    {NULL}};

static PyGetSetDef TraceReader_getsetters[] = {
    {"file_path", (getter)TraceReader_get_file_path, NULL,
     "Path to the trace file", NULL},
    {"index_dir", (getter)TraceReader_get_index_dir, NULL,
     "Directory for index files", NULL},
    {"has_index", (getter)TraceReader_get_has_index, NULL,
     "True if a checkpoint index was found", NULL},
    {"num_lines", (getter)TraceReader_get_num_lines, NULL,
     "Total line count (reads all lines if needed)", NULL},
    {NULL}};

PyTypeObject TraceReaderType = {
    PyVarObject_HEAD_INIT(NULL, 0) "dftracer_utils_ext.TraceReader",
    sizeof(TraceReaderObject),                /* tp_basicsize */
    0,                                        /* tp_itemsize */
    (destructor)TraceReader_dealloc,          /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    0,                                        /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "Smart trace file reader (auto-selects sequential vs indexed)", /* tp_doc */
    0,                          /* tp_traverse */
    0,                          /* tp_clear */
    0,                          /* tp_richcompare */
    0,                          /* tp_weaklistoffset */
    0,                          /* tp_iter */
    0,                          /* tp_iternext */
    TraceReader_methods,        /* tp_methods */
    0,                          /* tp_members */
    TraceReader_getsetters,     /* tp_getset */
    0,                          /* tp_base */
    0,                          /* tp_dict */
    0,                          /* tp_descr_get */
    0,                          /* tp_descr_set */
    0,                          /* tp_dictoffset */
    (initproc)TraceReader_init, /* tp_init */
    0,                          /* tp_alloc */
    TraceReader_new,            /* tp_new */
};

int init_trace_reader(PyObject *m) {
    if (PyType_Ready(&TraceReaderType) < 0) return -1;

    Py_INCREF(&TraceReaderType);
    if (PyModule_AddObject(m, "TraceReader", (PyObject *)&TraceReaderType) <
        0) {
        Py_DECREF(&TraceReaderType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}
