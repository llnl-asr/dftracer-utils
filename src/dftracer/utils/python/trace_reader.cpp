#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/python/trace_reader.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>

#include <cstddef>
#include <exception>
#include <string>
#include <vector>

namespace {

using dftracer::utils::coro::CoroTask;
using dftracer::utils::utilities::reader::ReadConfig;
using dftracer::utils::utilities::reader::TraceReader;
using dftracer::utils::utilities::reader::TraceReaderConfig;

// Drain the AsyncGenerator into a vector of owned strings.
// Line::content is string_view — must copy before advancing the generator.
CoroTask<std::vector<std::string>> collect_lines(TraceReaderConfig cfg,
                                                 std::size_t start_line,
                                                 std::size_t end_line) {
    std::vector<std::string> lines;
    TraceReader reader(std::move(cfg));
    ReadConfig rc;
    rc.start_line = start_line;
    rc.end_line = end_line;
    auto gen = reader.read_lines(rc);
    while (auto opt = co_await gen.next()) {
        lines.emplace_back(opt->content);
    }
    co_return lines;
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

// Drive a collect_lines task without the GIL, returning lines or setting a
// Python exception on failure.  Returns true on success.
bool run_collect(TraceReaderConfig cfg, std::size_t start_line,
                 std::size_t end_line, std::vector<std::string> &out) {
    std::exception_ptr exc;
    {
        auto task = collect_lines(std::move(cfg), start_line, end_line);
        Py_BEGIN_ALLOW_THREADS try { out = task.get(); } catch (...) {
            exc = std::current_exception();
        }
        Py_END_ALLOW_THREADS
    }
    if (exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::exception &e) {
            PyErr_SetString(PyExc_RuntimeError, e.what());
        } catch (...) {
            PyErr_SetString(PyExc_RuntimeError, "Unknown error in TraceReader");
        }
        return false;
    }
    return true;
}

}  // namespace

// ============================================================================
// Lifecycle
// ============================================================================

static void TraceReader_dealloc(TraceReaderObject *self) {
    Py_XDECREF(self->file_path);
    Py_XDECREF(self->index_dir);
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
    }
    return (PyObject *)self;
}

static int TraceReader_init(TraceReaderObject *self, PyObject *args,
                            PyObject *kwds) {
    static const char *kwlist[] = {"file_path",       "index_dir",
                                   "checkpoint_size", "auto_build_index",
                                   "index_threshold", NULL};

    const char *file_path;
    const char *index_dir = "";
    std::size_t checkpoint_size = 32 * 1024 * 1024;
    int auto_build_index = 0;
    std::size_t index_threshold = 8 * 1024 * 1024;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|snpn", (char **)kwlist,
                                     &file_path, &index_dir, &checkpoint_size,
                                     &auto_build_index, &index_threshold)) {
        return -1;
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

    // Probe for index existence via a throw-away TraceReader.
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

// ============================================================================
// Methods
// ============================================================================

static PyObject *TraceReader_read_lines(TraceReaderObject *self, PyObject *args,
                                        PyObject *kwds) {
    static const char *kwlist[] = {"start_line", "end_line", NULL};
    Py_ssize_t start_line = 0, end_line = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|nn", (char **)kwlist,
                                     &start_line, &end_line)) {
        return NULL;
    }

    if (start_line < 0 || end_line < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "start_line and end_line must be >= 0");
        return NULL;
    }

    TraceReaderConfig cfg;
    try {
        cfg = build_config(self);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    std::vector<std::string> lines;
    if (!run_collect(std::move(cfg), static_cast<std::size_t>(start_line),
                     static_cast<std::size_t>(end_line), lines)) {
        return NULL;
    }

    PyObject *list = PyList_New(static_cast<Py_ssize_t>(lines.size()));
    if (!list) return NULL;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        PyObject *s =
            PyUnicode_FromStringAndSize(lines[i].data(), lines[i].size());
        if (!s) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), s);
    }

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

// ============================================================================
// Getters
// ============================================================================

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
    TraceReaderConfig cfg;
    try {
        cfg = build_config(self);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    std::vector<std::string> lines;
    if (!run_collect(std::move(cfg), 0, 0, lines)) return NULL;

    return PyLong_FromSize_t(lines.size());
}

// ============================================================================
// Type tables
// ============================================================================

static PyMethodDef TraceReader_methods[] = {
    {"read_lines", (PyCFunction)TraceReader_read_lines,
     METH_VARARGS | METH_KEYWORDS,
     "Read lines and return list[str] (start_line=0, end_line=0)"},
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
