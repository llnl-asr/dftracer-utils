#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/utilities/reorganization_planner.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>

#include <string>
#include <vector>

using dftracer::utils::Runtime;
using namespace dftracer::utils::utilities::composites::dft::reorganize;

static Runtime *get_runtime(ReorganizationPlannerObject *self) {
    if (self->runtime_obj)
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    return get_default_runtime();
}

static void ReorganizationPlanner_dealloc(ReorganizationPlannerObject *self) {
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *ReorganizationPlanner_new(PyTypeObject *type, PyObject *args,
                                           PyObject *kwds) {
    ReorganizationPlannerObject *self;
    self = (ReorganizationPlannerObject *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->runtime_obj = NULL;
    }
    return (PyObject *)self;
}

static int ReorganizationPlanner_init(ReorganizationPlannerObject *self,
                                      PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"runtime", NULL};
    PyObject *runtime_arg = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", (char **)kwlist,
                                     &runtime_arg)) {
        return -1;
    }

    if (runtime_arg && runtime_arg != Py_None) {
        if (PyObject_TypeCheck(runtime_arg, &RuntimeType)) {
            Py_INCREF(runtime_arg);
            self->runtime_obj = runtime_arg;
        } else {
            PyObject *native = PyObject_GetAttrString(runtime_arg, "_native");
            if (native && PyObject_TypeCheck(native, &RuntimeType)) {
                self->runtime_obj = native;
            } else {
                Py_XDECREF(native);
                PyErr_SetString(PyExc_TypeError,
                                "runtime must be a Runtime instance or None");
                return -1;
            }
        }
    }

    return 0;
}

static PyObject *ReorganizationPlanner_plan(ReorganizationPlannerObject *self,
                                            PyObject *args, PyObject *kwds) {
    using dftracer::utils::coro::CoroTask;

    static const char *kwlist[] = {"source_files", "groups", "index_dir", NULL};
    PyObject *source_files_obj;
    PyObject *groups_obj = Py_None;
    const char *index_dir = "";

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|Os", (char **)kwlist,
                                     &source_files_obj, &groups_obj,
                                     &index_dir))
        return NULL;

    if (!PyList_Check(source_files_obj)) {
        PyErr_SetString(PyExc_TypeError, "source_files must be a list");
        return NULL;
    }

    Py_ssize_t nfiles = PyList_Size(source_files_obj);
    std::vector<std::string> files;
    files.reserve(static_cast<std::size_t>(nfiles));
    for (Py_ssize_t i = 0; i < nfiles; i++) {
        PyObject *item = PyList_GetItem(source_files_obj, i);
        const char *s = PyUnicode_AsUTF8(item);
        if (!s) return NULL;
        files.emplace_back(s);
    }

    std::vector<PredicateGroup> groups;
    if (groups_obj && groups_obj != Py_None) {
        if (!PyList_Check(groups_obj)) {
            PyErr_SetString(PyExc_TypeError,
                            "groups must be a list of dicts or None");
            return NULL;
        }
        Py_ssize_t n = PyList_Size(groups_obj);
        groups.reserve(static_cast<std::size_t>(n));
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *item = PyList_GetItem(groups_obj, i);
            PredicateGroup g;
            PyObject *name = PyDict_GetItemString(item, "name");
            PyObject *pred = PyDict_GetItemString(item, "predicate");
            if (name) {
                const char *ns = PyUnicode_AsUTF8(name);
                if (!ns) return NULL;
                g.name = ns;
            }
            if (pred) {
                const char *ps = PyUnicode_AsUTF8(pred);
                if (!ps) return NULL;
                g.predicate = ps;
            }
            groups.push_back(std::move(g));
        }
    }

    ReorganizationPlannerInput input;
    input.source_files = std::move(files);
    input.groups = std::move(groups);
    input.index_dir = index_dir;

    ExtractionPlan plan;
    auto *plan_p = &plan;
    ReorganizationPlannerInput input_copy = input;
    std::string error_msg;

    Py_BEGIN_ALLOW_THREADS try {
        Runtime *rt = get_runtime(self);
        auto task = [plan_p, input_copy]() -> CoroTask<void> {
            ReorganizationPlannerUtility util;
            *plan_p = co_await util.process(input_copy);
        };
        rt->submit(task(), "reorganization-planner").get();
    } catch (const std::exception &e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

    // groups list
    PyObject *py_groups =
        PyList_New(static_cast<Py_ssize_t>(plan.groups.size()));
    if (!py_groups) return NULL;
    for (std::size_t i = 0; i < plan.groups.size(); i++) {
        PyObject *g = PyDict_New();
        if (!g) {
            Py_DECREF(py_groups);
            return NULL;
        }
        PyDict_SetItemString(g, "name",
                             PyUnicode_FromString(plan.groups[i].name.c_str()));
        PyDict_SetItemString(
            g, "predicate",
            PyUnicode_FromString(plan.groups[i].predicate.c_str()));
        PyList_SetItem(py_groups, static_cast<Py_ssize_t>(i), g);
    }

    // source_files list
    PyObject *py_sources =
        PyList_New(static_cast<Py_ssize_t>(plan.source_files.size()));
    if (!py_sources) {
        Py_DECREF(py_groups);
        return NULL;
    }
    for (std::size_t i = 0; i < plan.source_files.size(); i++) {
        const auto &sf = plan.source_files[i];
        PyObject *entry = PyDict_New();
        if (!entry) {
            Py_DECREF(py_groups);
            Py_DECREF(py_sources);
            return NULL;
        }
        PyDict_SetItemString(entry, "file_path",
                             PyUnicode_FromString(sf.file_path.c_str()));
        PyDict_SetItemString(entry, "idx_path",
                             PyUnicode_FromString(sf.idx_path.c_str()));
        PyDict_SetItemString(entry, "num_checkpoints",
                             PyLong_FromSize_t(sf.num_checkpoints));
        PyDict_SetItemString(entry, "uncompressed_size",
                             PyLong_FromUnsignedLongLong(sf.uncompressed_size));
        PyDict_SetItemString(entry, "checkpoint_size",
                             PyLong_FromUnsignedLongLong(sf.checkpoint_size));
        PyList_SetItem(py_sources, static_cast<Py_ssize_t>(i), entry);
    }

    // tasks list
    PyObject *py_tasks = PyList_New(static_cast<Py_ssize_t>(plan.tasks.size()));
    if (!py_tasks) {
        Py_DECREF(py_groups);
        Py_DECREF(py_sources);
        return NULL;
    }
    for (std::size_t i = 0; i < plan.tasks.size(); i++) {
        const auto &t = plan.tasks[i];
        PyObject *entry = PyDict_New();
        if (!entry) {
            Py_DECREF(py_groups);
            Py_DECREF(py_sources);
            Py_DECREF(py_tasks);
            return NULL;
        }
        PyDict_SetItemString(entry, "source_file_idx",
                             PyLong_FromSize_t(t.source_file_idx));
        PyDict_SetItemString(entry, "checkpoint_idx",
                             PyLong_FromUnsignedLongLong(t.checkpoint_idx));
        PyDict_SetItemString(entry, "target_group",
                             PyUnicode_FromString(t.target_group.c_str()));
        PyDict_SetItemString(entry, "start_byte",
                             PyLong_FromUnsignedLongLong(t.start_byte));
        PyDict_SetItemString(entry, "end_byte",
                             PyLong_FromUnsignedLongLong(t.end_byte));
        PyList_SetItem(py_tasks, static_cast<Py_ssize_t>(i), entry);
    }

    PyObject *result = PyDict_New();
    if (!result) {
        Py_DECREF(py_groups);
        Py_DECREF(py_sources);
        Py_DECREF(py_tasks);
        return NULL;
    }
    PyDict_SetItemString(result, "groups", py_groups);
    Py_DECREF(py_groups);
    PyDict_SetItemString(result, "source_files", py_sources);
    Py_DECREF(py_sources);
    PyDict_SetItemString(result, "tasks", py_tasks);
    Py_DECREF(py_tasks);
    PyDict_SetItemString(result, "total_events",
                         PyLong_FromSize_t(plan.total_events));
    return result;
}

static PyObject *ReorganizationPlanner_call(PyObject *self, PyObject *args,
                                            PyObject *kwds) {
    return ReorganizationPlanner_plan((ReorganizationPlannerObject *)self, args,
                                      kwds);
}

static PyMethodDef ReorganizationPlanner_methods[] = {
    {"process", (PyCFunction)ReorganizationPlanner_plan,
     METH_VARARGS | METH_KEYWORDS,
     "process(source_files, groups=None, index_dir='')\n"
     "--\n"
     "\n"
     "Build a reorganization plan for trace files.\n"
     "\n"
     "Args:\n"
     "    source_files (list[str]): Paths to source trace files.\n"
     "    groups (list[dict] or None): Predicate group definitions\n"
     "        (default None).\n"
     "    index_dir (str): Directory for index sidecars (default '').\n"
     "\n"
     "Returns:\n"
     "    dict: Extraction plan.\n"},
    {NULL} /* Sentinel */
};

PyTypeObject ReorganizationPlannerType = {
    PyVarObject_HEAD_INIT(
        NULL,
        0) "dftracer_utils_ext.ReorganizationPlannerUtility", /* tp_name */
    sizeof(ReorganizationPlannerObject),                      /* tp_basicsize */
    0,                                                        /* tp_itemsize */
    (destructor)ReorganizationPlanner_dealloc,                /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    ReorganizationPlanner_call,               /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "ReorganizationPlannerUtility(runtime: Runtime | None = None)\n"
    "--\n"
    "\n"
    "Plan semantic reorganization of trace files into predicate groups.\n"
    "\n"
    "Args:\n"
    "    runtime (Runtime or None): Runtime for thread pool control.\n"
    "        If None, uses the default global Runtime.\n", /* tp_doc */
    0,                                                     /* tp_traverse */
    0,                                                     /* tp_clear */
    0,                                                     /* tp_richcompare */
    0,                                    /* tp_weaklistoffset */
    0,                                    /* tp_iter */
    0,                                    /* tp_iternext */
    ReorganizationPlanner_methods,        /* tp_methods */
    0,                                    /* tp_members */
    0,                                    /* tp_getset */
    0,                                    /* tp_base */
    0,                                    /* tp_dict */
    0,                                    /* tp_descr_get */
    0,                                    /* tp_descr_set */
    0,                                    /* tp_dictoffset */
    (initproc)ReorganizationPlanner_init, /* tp_init */
    0,                                    /* tp_alloc */
    ReorganizationPlanner_new,            /* tp_new */
};

int init_reorganization_planner(PyObject *m) {
    if (PyType_Ready(&ReorganizationPlannerType) < 0) return -1;

    Py_INCREF(&ReorganizationPlannerType);
    if (PyModule_AddObject(m, "ReorganizationPlannerUtility",
                           (PyObject *)&ReorganizationPlannerType) < 0) {
        Py_DECREF(&ReorganizationPlannerType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}
