// SPDX-License-Identifier: MIT
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/python/py_dict_helpers.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/runtime.h>

#include <chrono>

dftracer::utils::Runtime *dftracer::utils::python::get_default_runtime() {
    return &dftracer::utils::default_runtime();
}

static void Runtime_dealloc(RuntimeObject *self) {
    self->runtime.reset();
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *Runtime_new(PyTypeObject *type, PyObject *, PyObject *) {
    RuntimeObject *self = (RuntimeObject *)type->tp_alloc(type, 0);
    if (self) {
        // Placement-new the shared_ptr (tp_alloc gives raw memory)
        new (&self->runtime)
            dftracer::utils::dstd::SharedPtr<dftracer::utils::Runtime>(nullptr);
    }
    return (PyObject *)self;
}

static int Runtime_init(RuntimeObject *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"threads", "io_threads", NULL};
    Py_ssize_t threads = 0;
    Py_ssize_t io_threads = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|nn",
                                     const_cast<char **>(kwlist), &threads,
                                     &io_threads)) {
        return -1;
    }

    if (threads < 0) {
        PyErr_SetString(PyExc_ValueError, "threads must be >= 0");
        return -1;
    }
    if (io_threads < 0) {
        PyErr_SetString(PyExc_ValueError, "io_threads must be >= 0");
        return -1;
    }

    try {
        // The process has one runtime, which this configures and holds.
        dftracer::utils::RuntimeSettings settings;
        settings.threads = static_cast<std::size_t>(threads);
        settings.io_threads = static_cast<std::size_t>(io_threads);
        dftracer::utils::configure_default_runtime(settings);
        self->runtime = dftracer::utils::default_runtime_shared();
    } catch (const std::exception &e) {
        dftracer::utils::python::set_typed_py_error(e);
        return -1;
    }

    return 0;
}

static bool set_size(PyObject *d, const char *key, std::size_t val) {
    return dict_set_size(d, key, val) == 0;
}

static bool set_double(PyObject *d, const char *key, double val) {
    return dict_set_f64(d, key, val) == 0;
}

static bool set_str(PyObject *d, const char *key,
                    const dftracer::utils::dstd::String &val) {
    return dict_set_str(d, key, val.c_str()) == 0;
}

static bool set_bool(PyObject *d, const char *key, bool val) {
    return dict_set_bool(d, key, val) == 0;
}

static dftracer::utils::dstd::String task_location_text(
    const dftracer::utils::TaskProgress &tp) {
    switch (tp.location) {
        case dftracer::utils::TaskLocation::SharedQueue:
            return dftracer::utils::dstd::String("shared_queue");
        case dftracer::utils::TaskLocation::LocalQueue:
            return "worker_" + dftracer::utils::dstd::to_string(tp.worker) +
                   "_local";
        case dftracer::utils::TaskLocation::Executing:
            return "executing_on_worker_" +
                   dftracer::utils::dstd::to_string(tp.worker);
        case dftracer::utils::TaskLocation::Done:
            return dftracer::utils::dstd::String("done");
    }
    return dftracer::utils::dstd::String();
}

static PyObject *build_task_progress(const dftracer::utils::TaskProgress &tp) {
    PyObject *td = PyDict_New();
    if (!td) return NULL;

    const dftracer::utils::dstd::String state(
        dftracer::utils::task_state_name(tp.state));
    const dftracer::utils::dstd::String location = task_location_text(tp);
    if (!set_str(td, "name", tp.name) || !set_str(td, "state", state) ||
        !set_double(td, "queued_duration_ms", tp.queued_duration_ms) ||
        !set_double(td, "execution_duration_ms", tp.execution_duration_ms) ||
        !set_size(td, "total_subtasks", tp.total_subtasks) ||
        !set_size(td, "completed_subtasks", tp.completed_subtasks) ||
        !set_double(td, "progress_pct", tp.progress_percentage) ||
        !set_str(td, "location", location)) {
        Py_DECREF(td);
        return NULL;
    }

    PyObject *children =
        PyList_New(static_cast<Py_ssize_t>(tp.children.size()));
    if (!children) {
        Py_DECREF(td);
        return NULL;
    }
    for (std::size_t i = 0; i < tp.children.size(); ++i) {
        PyObject *child = build_task_progress(tp.children[i]);
        if (!child) {
            Py_DECREF(children);
            Py_DECREF(td);
            return NULL;
        }
        PyList_SET_ITEM(children, static_cast<Py_ssize_t>(i), child);
    }
    if (PyDict_SetItemString(td, "children", children) < 0) {
        Py_DECREF(children);
        Py_DECREF(td);
        return NULL;
    }
    Py_DECREF(children);
    return td;
}

static PyObject *Runtime_get_progress(RuntimeObject *self,
                                      PyObject *Py_UNUSED(ignored)) {
    if (!self->runtime) {
        PyErr_SetString(PyExc_RuntimeError, "Runtime not initialized");
        return NULL;
    }

    dftracer::utils::ExecutorProgress prog;
    Py_BEGIN_ALLOW_THREADS prog = self->runtime->progress();
    Py_END_ALLOW_THREADS

        PyObject *d = PyDict_New();
    if (!d) return NULL;

    if (!set_size(d, "total", prog.total_tasks_submitted) ||
        !set_size(d, "completed", prog.tasks_completed) ||
        !set_size(d, "running", prog.tasks_running) ||
        !set_size(d, "queued", prog.tasks_queued) ||
        !set_size(d, "failed", prog.tasks_failed)) {
        Py_DECREF(d);
        return NULL;
    }

    PyObject *workers =
        PyList_New(static_cast<Py_ssize_t>(prog.workers.size()));
    if (!workers) {
        Py_DECREF(d);
        return NULL;
    }
    for (std::size_t i = 0; i < prog.workers.size(); ++i) {
        const auto &w = prog.workers[i];
        PyObject *wd = PyDict_New();
        if (!wd || !set_size(wd, "id", w.worker_id) ||
            !set_bool(wd, "idle", w.is_idle) ||
            !set_str(wd, "task", w.current_task_name) ||
            !set_size(wd, "queue_depth", w.local_queue_depth)) {
            Py_XDECREF(wd);
            Py_DECREF(workers);
            Py_DECREF(d);
            return NULL;
        }
        PyList_SET_ITEM(workers, static_cast<Py_ssize_t>(i), wd);
    }
    if (PyDict_SetItemString(d, "workers", workers) < 0) {
        Py_DECREF(workers);
        Py_DECREF(d);
        return NULL;
    }
    Py_DECREF(workers);

    PyObject *tasks =
        PyList_New(static_cast<Py_ssize_t>(prog.root_tasks.size()));
    if (!tasks) {
        Py_DECREF(d);
        return NULL;
    }
    for (std::size_t i = 0; i < prog.root_tasks.size(); ++i) {
        PyObject *tp = build_task_progress(prog.root_tasks[i]);
        if (!tp) {
            Py_DECREF(tasks);
            Py_DECREF(d);
            return NULL;
        }
        PyList_SET_ITEM(tasks, static_cast<Py_ssize_t>(i), tp);
    }
    if (PyDict_SetItemString(d, "tasks", tasks) < 0) {
        Py_DECREF(tasks);
        Py_DECREF(d);
        return NULL;
    }
    Py_DECREF(tasks);

    PyObject *errors =
        PyList_New(static_cast<Py_ssize_t>(prog.recent_errors.size()));
    if (!errors) {
        Py_DECREF(d);
        return NULL;
    }
    for (std::size_t i = 0; i < prog.recent_errors.size(); ++i) {
        const auto &[tid, msg] = prog.recent_errors[i];
        PyObject *ed = PyDict_New();
        if (!ed || !set_size(ed, "task_id", static_cast<std::size_t>(tid)) ||
            !set_str(ed, "message", msg)) {
            Py_XDECREF(ed);
            Py_DECREF(errors);
            Py_DECREF(d);
            return NULL;
        }
        PyList_SET_ITEM(errors, static_cast<Py_ssize_t>(i), ed);
    }
    if (PyDict_SetItemString(d, "errors", errors) < 0) {
        Py_DECREF(errors);
        Py_DECREF(d);
        return NULL;
    }
    Py_DECREF(errors);

    return d;
}

static PyObject *Runtime_is_responsive(RuntimeObject *self,
                                       PyObject *Py_UNUSED(ignored)) {
    if (!self->runtime) {
        PyErr_SetString(PyExc_RuntimeError, "Runtime not initialized");
        return NULL;
    }
    bool resp;
    Py_BEGIN_ALLOW_THREADS resp = self->runtime->is_responsive();
    Py_END_ALLOW_THREADS return PyBool_FromLong(resp ? 1 : 0);
}

static PyObject *Runtime_set_timeout(RuntimeObject *self, PyObject *args,
                                     PyObject *kwds) {
    static const char *kwlist[] = {"global_ms", NULL};
    Py_ssize_t ms = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|n",
                                     const_cast<char **>(kwlist), &ms)) {
        return NULL;
    }

    if (!self->runtime) {
        PyErr_SetString(PyExc_RuntimeError, "Runtime not initialized");
        return NULL;
    }

    Py_BEGIN_ALLOW_THREADS self->runtime->set_global_timeout(
        std::chrono::milliseconds(ms));
    Py_END_ALLOW_THREADS Py_RETURN_NONE;
}

static PyObject *Runtime_set_default_task_timeout(RuntimeObject *self,
                                                  PyObject *args,
                                                  PyObject *kwds) {
    static const char *kwlist[] = {"ms", NULL};
    Py_ssize_t ms = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|n",
                                     const_cast<char **>(kwlist), &ms)) {
        return NULL;
    }

    if (!self->runtime) {
        PyErr_SetString(PyExc_RuntimeError, "Runtime not initialized");
        return NULL;
    }

    Py_BEGIN_ALLOW_THREADS self->runtime->set_default_task_timeout(
        std::chrono::milliseconds(ms));
    Py_END_ALLOW_THREADS Py_RETURN_NONE;
}

static PyObject *Runtime_wait_all(RuntimeObject *self,
                                  PyObject *Py_UNUSED(ignored)) {
    if (!self->runtime) {
        PyErr_SetString(PyExc_RuntimeError, "Runtime not initialized");
        return NULL;
    }
    if (!run_blocking([&] { self->runtime->wait_all(); })) return NULL;
    Py_RETURN_NONE;
}

static PyObject *Runtime_enter(RuntimeObject *self,
                               PyObject *Py_UNUSED(ignored)) {
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *Runtime_exit(RuntimeObject *Py_UNUSED(self), PyObject *) {
    Py_RETURN_NONE;
}

static PyObject *Runtime_get_threads(RuntimeObject *self, void *) {
    if (!self->runtime) {
        PyErr_SetString(PyExc_RuntimeError, "Runtime not initialized");
        return NULL;
    }
    return PyLong_FromSize_t(self->runtime->threads());
}

static PyObject *get_default_runtime_py(PyObject *Py_UNUSED(module),
                                        PyObject *Py_UNUSED(ignored)) {
    dftracer::utils::Runtime *rt =
        dftracer::utils::python::get_default_runtime();
    if (!rt) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to create default runtime");
        return NULL;
    }

    RuntimeObject *obj = (RuntimeObject *)RuntimeType.tp_alloc(&RuntimeType, 0);
    if (!obj) return NULL;

    new (&obj->runtime)
        dftracer::utils::dstd::SharedPtr<dftracer::utils::Runtime>(
            dftracer::utils::default_runtime_shared());
    return (PyObject *)obj;
}

// Return the current default runtime without creating one. Unlike
// get_default_runtime, this never materializes a full-machine-sized runtime as
// a side effect, so callers that only want to save/restore the default (e.g. a
// Dask worker plugin) do not each spin up an unused hardware_concurrency-thread
// runtime.
static PyObject *peek_default_runtime_py(PyObject *Py_UNUSED(module),
                                         PyObject *Py_UNUSED(ignored)) {
    auto rt = dftracer::utils::peek_default_runtime();
    if (!rt) Py_RETURN_NONE;

    RuntimeObject *obj = (RuntimeObject *)RuntimeType.tp_alloc(&RuntimeType, 0);
    if (!obj) return NULL;

    new (&obj->runtime)
        dftracer::utils::dstd::SharedPtr<dftracer::utils::Runtime>(rt);
    return (PyObject *)obj;
}

static PyMethodDef Runtime_methods[] = {
    {"get_progress", DFTU_PYCFUNCTION(Runtime_get_progress), METH_NOARGS,
     "Return progress dict with keys: total, completed, running,\n"
     "queued, failed."},
    {"is_responsive", DFTU_PYCFUNCTION(Runtime_is_responsive), METH_NOARGS,
     "Return True if the runtime is making progress."},
    {"set_timeout", DFTU_PYCFUNCTION(Runtime_set_timeout),
     METH_VARARGS | METH_KEYWORDS,
     "Set global timeout in milliseconds.\n"
     "\n"
     "Args:\n"
     "    global_ms (int): Timeout in milliseconds (0 = no timeout).\n"},
    {"set_default_task_timeout",
     DFTU_PYCFUNCTION(Runtime_set_default_task_timeout),
     METH_VARARGS | METH_KEYWORDS,
     "Set default per-task timeout in milliseconds.\n"
     "\n"
     "Args:\n"
     "    ms (int): Timeout in milliseconds (0 = no timeout).\n"},
    {"wait_all", DFTU_PYCFUNCTION(Runtime_wait_all), METH_NOARGS,
     "Wait for all outstanding submitted tasks to complete."},
    {"__enter__", DFTU_PYCFUNCTION(Runtime_enter), METH_NOARGS,
     "Enter context manager."},
    {"__exit__", DFTU_PYCFUNCTION(Runtime_exit), METH_VARARGS,
     "Exit context manager. The process runtime keeps running."},
    {NULL}};

static PyObject *Runtime_get_io_threads(RuntimeObject *self, void *) {
    if (!self->runtime) {
        PyErr_SetString(PyExc_RuntimeError, "Runtime not initialized");
        return NULL;
    }
    return PyLong_FromSize_t(self->runtime->io_threads());
}

static PyGetSetDef Runtime_getsetters[] = {
    {"threads", (getter)Runtime_get_threads, NULL, "Number of worker threads",
     NULL},
    {"io_threads", (getter)Runtime_get_io_threads, NULL,
     "Number of I/O threads", NULL},
    {NULL}};

PyTypeObject RuntimeType = {
    PyVarObject_HEAD_INIT(NULL, 0) "dftracer_utils_ext.Runtime",
    sizeof(RuntimeObject),                    /* tp_basicsize */
    0,                                        /* tp_itemsize */
    (destructor)Runtime_dealloc,              /* tp_dealloc */
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
    "Runtime(threads: int = 0, io_threads: int = 0)\n"
    "--\n"
    "\n"
    "A handle to the process runtime, the one thread pool every part of\n"
    "the library runs on. Making one configures that runtime in place;\n"
    "it runs until the process exits.\n"
    "\n"
    "Args:\n"
    "    threads (int): Worker threads. 0 (default) keeps the current\n"
    "        count (the hardware concurrency for a new runtime).\n"
    "    io_threads (int): I/O threads, which only grow. 0 (default)\n"
    "        keeps the current count.\n", /* tp_doc */
    0,                                    /* tp_traverse */
    0,                                    /* tp_clear */
    0,                                    /* tp_richcompare */
    0,                                    /* tp_weaklistoffset */
    0,                                    /* tp_iter */
    0,                                    /* tp_iternext */
    Runtime_methods,                      /* tp_methods */
    0,                                    /* tp_members */
    Runtime_getsetters,                   /* tp_getset */
    0,                                    /* tp_base */
    0,                                    /* tp_dict */
    0,                                    /* tp_descr_get */
    0,                                    /* tp_descr_set */
    0,                                    /* tp_dictoffset */
    (initproc)Runtime_init,               /* tp_init */
    0,                                    /* tp_alloc */
    Runtime_new,                          /* tp_new */
};

// Module-level function table (registered via PyModule_AddFunctions or
// appended to the module's method table in init_runtime).
static PyMethodDef runtime_module_methods[] = {
    {"get_default_runtime", get_default_runtime_py, METH_NOARGS,
     "Return the process runtime (made on first use)."},
    {"peek_default_runtime", peek_default_runtime_py, METH_NOARGS,
     "Return the process runtime, or None before its first use "
     "(never makes one)."},
    {NULL}};

// Runs during interpreter finalization. Run the shutdown hooks there, while
// the process is still healthy, because the runtime is one of them.
static void dftracer_utils_atexit_cleanup() {
    dftracer::utils::dstd::shutdown();
}

int dftracer::utils::python::init_runtime(PyObject *m) {
    if (register_type(m, &RuntimeType, "Runtime") < 0) return -1;

    Py_AtExit(dftracer_utils_atexit_cleanup);

    for (PyMethodDef *def = runtime_module_methods; def->ml_name; ++def) {
        PyObject *fn = PyCFunction_New(def, NULL);
        if (!fn) return -1;
        if (PyModule_AddObject(m, def->ml_name, fn) < 0) {
            Py_DECREF(fn);
            return -1;
        }
    }

    return 0;
}
