#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/python/py_dict_helpers.h>
#include <dftracer/utils/python/py_list_helpers.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/utilities/reorganization_planner.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>

#include <string>
#include <vector>

using dftracer::utils::CoroScope;
using dftracer::utils::Runtime;
using dftracer::utils::utilities::behaviors::UtilityExecutor;
namespace tags = dftracer::utils::utilities::tags;
using namespace dftracer::utils::utilities::composites::dft::reorganize;

DFTRACER_UTILS_RUNTIME_BACKED_SLOTS(ReorganizationPlanner,
                                    ReorganizationPlannerObject)

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

    std::vector<std::string> files;
    if (!parse_str_list(source_files_obj, "source_files", files)) return NULL;

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
            PyObject *pred = PyDict_GetItemString(item, "query");
            if (name) {
                const char *ns = PyUnicode_AsUTF8(name);
                if (!ns) return NULL;
                g.name = ns;
            }
            if (pred) {
                const char *ps = PyUnicode_AsUTF8(pred);
                if (!ps) return NULL;
                g.query = ps;
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

    if (!run_blocking([&] {
            Runtime *rt = resolve_runtime(self);
            auto task = run_coro_scope(
                rt->executor(),
                [plan_p, input_copy](CoroScope &scope) -> CoroTask<void> {
                    auto planner =
                        std::make_shared<ReorganizationPlannerUtility>();
                    UtilityExecutor<ReorganizationPlannerInput, ExtractionPlan,
                                    tags::NeedsContext>
                        exec(planner);
                    *plan_p = co_await exec.execute(scope, input_copy);
                });
            rt->submit(std::move(task), "reorganization-planner").wait();
        })) {
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
        dict_set_steal(g, "name",
                       PyUnicode_FromString(plan.groups[i].name.c_str()));
        dict_set_steal(g, "query",
                       PyUnicode_FromString(plan.groups[i].query.c_str()));
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
        dict_set_steal(entry, "file_path",
                       PyUnicode_FromString(sf.file_path.c_str()));
        dict_set_steal(entry, "index_path",
                       PyUnicode_FromString(sf.index_path.c_str()));
        dict_set_steal(entry, "num_checkpoints",
                       PyLong_FromSize_t(sf.num_checkpoints));
        dict_set_steal(entry, "uncompressed_size",
                       PyLong_FromUnsignedLongLong(sf.uncompressed_size));
        dict_set_steal(entry, "checkpoint_size",
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
        dict_set_steal(entry, "source_file_idx",
                       PyLong_FromSize_t(t.source_file_idx));
        dict_set_steal(entry, "checkpoint_idx",
                       PyLong_FromUnsignedLongLong(t.checkpoint_idx));
        dict_set_steal(entry, "target_group",
                       PyUnicode_FromString(t.target_group.c_str()));
        dict_set_steal(entry, "start_byte",
                       PyLong_FromUnsignedLongLong(t.start_byte));
        dict_set_steal(entry, "end_byte",
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
    dict_set_steal(result, "total_events",
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
     "    index_dir (str): Directory for .dftindex stores (default '').\n"
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
    if (register_type(m, &ReorganizationPlannerType,
                      "ReorganizationPlannerUtility") < 0)
        return -1;

    return 0;
}
