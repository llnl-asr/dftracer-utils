#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/python/runtime.h>

#include <chrono>
#include <memory>

static std::shared_ptr<dftracer::utils::Runtime> g_default_runtime;

dftracer::utils::Runtime *get_default_runtime() {
    if (!g_default_runtime) {
        g_default_runtime = std::make_shared<dftracer::utils::Runtime>(0);
    }
    return g_default_runtime.get();
}

static void Runtime_dealloc(RuntimeObject *self) {
    self->runtime.reset();
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *Runtime_new(PyTypeObject *type, PyObject *args,
                             PyObject *kwds) {
    RuntimeObject *self = (RuntimeObject *)type->tp_alloc(type, 0);
    if (self) {
        // Placement-new the shared_ptr (tp_alloc gives raw memory)
        new (&self->runtime) std::shared_ptr<dftracer::utils::Runtime>(nullptr);
    }
    return (PyObject *)self;
}

static int Runtime_init(RuntimeObject *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"threads", NULL};
    Py_ssize_t threads = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|n", (char **)kwlist,
                                     &threads)) {
        return -1;
    }

    if (threads < 0) {
        PyErr_SetString(PyExc_ValueError, "threads must be >= 0");
        return -1;
    }

    try {
        self->runtime = std::make_shared<dftracer::utils::Runtime>(
            static_cast<std::size_t>(threads));
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return -1;
    }

    return 0;
}

static PyObject *Runtime_shutdown(RuntimeObject *self,
                                  PyObject *Py_UNUSED(ignored)) {
    if (!self->runtime) {
        PyErr_SetString(PyExc_RuntimeError, "Runtime not initialized");
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS self->runtime->shutdown();
    Py_END_ALLOW_THREADS Py_RETURN_NONE;
}

static bool set_size(PyObject *d, const char *key, std::size_t val) {
    PyObject *v = PyLong_FromSize_t(val);
    if (!v) return false;
    int rc = PyDict_SetItemString(d, key, v);
    Py_DECREF(v);
    return rc == 0;
}

static bool set_double(PyObject *d, const char *key, double val) {
    PyObject *v = PyFloat_FromDouble(val);
    if (!v) return false;
    int rc = PyDict_SetItemString(d, key, v);
    Py_DECREF(v);
    return rc == 0;
}

static bool set_str(PyObject *d, const char *key, const std::string &val) {
    PyObject *v = PyUnicode_FromStringAndSize(
        val.data(), static_cast<Py_ssize_t>(val.size()));
    if (!v) return false;
    int rc = PyDict_SetItemString(d, key, v);
    Py_DECREF(v);
    return rc == 0;
}

static bool set_bool(PyObject *d, const char *key, bool val) {
    PyObject *v = val ? Py_True : Py_False;
    Py_INCREF(v);
    int rc = PyDict_SetItemString(d, key, v);
    Py_DECREF(v);
    return rc == 0;
}

static PyObject *build_task_progress(const dftracer::utils::TaskProgress &tp) {
    PyObject *td = PyDict_New();
    if (!td) return NULL;

    if (!set_str(td, "name", tp.name) || !set_str(td, "state", tp.state) ||
        !set_double(td, "queued_duration_ms", tp.queued_duration_ms) ||
        !set_double(td, "execution_duration_ms", tp.execution_duration_ms) ||
        !set_size(td, "total_subtasks", tp.total_subtasks) ||
        !set_size(td, "completed_subtasks", tp.completed_subtasks) ||
        !set_double(td, "progress_pct", tp.progress_percentage) ||
        !set_str(td, "location", tp.location)) {
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
    Py_BEGIN_ALLOW_THREADS prog = self->runtime->get_progress();
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

    // Workers
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

    // Tasks
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

    // Errors
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

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|n", (char **)kwlist, &ms)) {
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

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|n", (char **)kwlist, &ms)) {
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
    try {
        Py_BEGIN_ALLOW_THREADS self->runtime->wait_all();
        Py_END_ALLOW_THREADS
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *Runtime_enter(RuntimeObject *self,
                               PyObject *Py_UNUSED(ignored)) {
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *Runtime_exit(RuntimeObject *self, PyObject *args) {
    if (self->runtime) {
        Py_BEGIN_ALLOW_THREADS self->runtime->shutdown();
        Py_END_ALLOW_THREADS
    }
    Py_RETURN_NONE;
}

static PyObject *Runtime_get_threads(RuntimeObject *self, void *closure) {
    if (!self->runtime) {
        PyErr_SetString(PyExc_RuntimeError, "Runtime not initialized");
        return NULL;
    }
    return PyLong_FromSize_t(self->runtime->threads());
}

static PyObject *get_default_runtime_py(PyObject *Py_UNUSED(module),
                                        PyObject *Py_UNUSED(ignored)) {
    dftracer::utils::Runtime *rt = get_default_runtime();
    if (!rt) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to create default runtime");
        return NULL;
    }

    RuntimeObject *obj = (RuntimeObject *)RuntimeType.tp_alloc(&RuntimeType, 0);
    if (!obj) return NULL;

    new (&obj->runtime)
        std::shared_ptr<dftracer::utils::Runtime>(g_default_runtime);
    return (PyObject *)obj;
}

static PyObject *set_default_runtime_py(PyObject *Py_UNUSED(module),
                                        PyObject *args) {
    PyObject *arg;
    if (!PyArg_ParseTuple(args, "O", &arg)) return NULL;

    if (arg == Py_None) {
        g_default_runtime.reset();
        Py_RETURN_NONE;
    }

    if (!PyObject_TypeCheck(arg, &RuntimeType)) {
        PyErr_SetString(PyExc_TypeError, "Expected Runtime or None");
        return NULL;
    }

    g_default_runtime = ((RuntimeObject *)arg)->runtime;
    Py_RETURN_NONE;
}

static PyMethodDef Runtime_methods[] = {
    {"shutdown", (PyCFunction)Runtime_shutdown, METH_NOARGS,
     "Shut down the runtime"},
    {"get_progress", (PyCFunction)Runtime_get_progress, METH_NOARGS,
     "Return dict with keys: total, completed, running, queued, failed"},
    {"is_responsive", (PyCFunction)Runtime_is_responsive, METH_NOARGS,
     "Return True if the runtime is making progress"},
    {"set_timeout", (PyCFunction)Runtime_set_timeout,
     METH_VARARGS | METH_KEYWORDS,
     "Set global timeout in milliseconds (global_ms=0)"},
    {"set_default_task_timeout", (PyCFunction)Runtime_set_default_task_timeout,
     METH_VARARGS | METH_KEYWORDS,
     "Set default per-task timeout in milliseconds (ms=0)"},
    {"wait_all", (PyCFunction)Runtime_wait_all, METH_NOARGS,
     "Wait for all outstanding submitted tasks to complete"},
    {"__enter__", (PyCFunction)Runtime_enter, METH_NOARGS,
     "Enter context manager"},
    {"__exit__", (PyCFunction)Runtime_exit, METH_VARARGS,
     "Exit context manager (calls shutdown)"},
    {NULL}};

static PyGetSetDef Runtime_getsetters[] = {
    {"threads", (getter)Runtime_get_threads, NULL, "Number of worker threads",
     NULL},
    {NULL}};

PyTypeObject RuntimeType = {
    PyVarObject_HEAD_INIT(NULL, 0) "dftracer_utils_ext.Runtime",
    sizeof(RuntimeObject),                       /* tp_basicsize */
    0,                                           /* tp_itemsize */
    (destructor)Runtime_dealloc,                 /* tp_dealloc */
    0,                                           /* tp_vectorcall_offset */
    0,                                           /* tp_getattr */
    0,                                           /* tp_setattr */
    0,                                           /* tp_as_async */
    0,                                           /* tp_repr */
    0,                                           /* tp_as_number */
    0,                                           /* tp_as_sequence */
    0,                                           /* tp_as_mapping */
    0,                                           /* tp_hash */
    0,                                           /* tp_call */
    0,                                           /* tp_str */
    0,                                           /* tp_getattro */
    0,                                           /* tp_setattro */
    0,                                           /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,    /* tp_flags */
    "Coroutine runtime backed by a thread pool", /* tp_doc */
    0,                                           /* tp_traverse */
    0,                                           /* tp_clear */
    0,                                           /* tp_richcompare */
    0,                                           /* tp_weaklistoffset */
    0,                                           /* tp_iter */
    0,                                           /* tp_iternext */
    Runtime_methods,                             /* tp_methods */
    0,                                           /* tp_members */
    Runtime_getsetters,                          /* tp_getset */
    0,                                           /* tp_base */
    0,                                           /* tp_dict */
    0,                                           /* tp_descr_get */
    0,                                           /* tp_descr_set */
    0,                                           /* tp_dictoffset */
    (initproc)Runtime_init,                      /* tp_init */
    0,                                           /* tp_alloc */
    Runtime_new,                                 /* tp_new */
};

// Module-level function table (registered via PyModule_AddFunctions or
// appended to the module's method table in init_runtime).
static PyMethodDef runtime_module_methods[] = {
    {"get_default_runtime", get_default_runtime_py, METH_NOARGS,
     "Return the module-level default Runtime (lazy-created)"},
    {"set_default_runtime", set_default_runtime_py, METH_VARARGS,
     "Replace the module-level default Runtime (pass None to clear)"},
    {NULL}};

int init_runtime(PyObject *m) {
    if (PyType_Ready(&RuntimeType) < 0) return -1;

    Py_INCREF(&RuntimeType);
    if (PyModule_AddObject(m, "Runtime", (PyObject *)&RuntimeType) < 0) {
        Py_DECREF(&RuntimeType);
        Py_DECREF(m);
        return -1;
    }

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
