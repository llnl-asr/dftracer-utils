#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/utilities/reconstruction_planner.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reconstruction_planner.h>

#include <string>
#include <vector>

using dftracer::utils::Runtime;
using namespace dftracer::utils::utilities::composites::dft::reorganize;

static Runtime *get_runtime(ReconstructionPlannerObject *self) {
    if (self->runtime_obj)
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    return get_default_runtime();
}

static void ReconstructionPlanner_dealloc(ReconstructionPlannerObject *self) {
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *ReconstructionPlanner_new(PyTypeObject *type, PyObject *args,
                                           PyObject *kwds) {
    ReconstructionPlannerObject *self;
    self = (ReconstructionPlannerObject *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->runtime_obj = NULL;
    }
    return (PyObject *)self;
}

static int ReconstructionPlanner_init(ReconstructionPlannerObject *self,
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

static PyObject *ReconstructionPlanner_plan(ReconstructionPlannerObject *self,
                                            PyObject *args, PyObject *kwds) {
    using dftracer::utils::coro::CoroTask;

    static const char *kwlist[] = {"reorganized_files", "index_dir", NULL};
    PyObject *files_obj;
    const char *index_dir = "";

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|s", (char **)kwlist,
                                     &files_obj, &index_dir))
        return NULL;

    if (!PyList_Check(files_obj)) {
        PyErr_SetString(PyExc_TypeError, "reorganized_files must be a list");
        return NULL;
    }

    Py_ssize_t nfiles = PyList_Size(files_obj);
    std::vector<std::string> files;
    files.reserve(static_cast<std::size_t>(nfiles));
    for (Py_ssize_t i = 0; i < nfiles; i++) {
        PyObject *item = PyList_GetItem(files_obj, i);
        const char *s = PyUnicode_AsUTF8(item);
        if (!s) return NULL;
        files.emplace_back(s);
    }

    ReconstructionPlannerInput input;
    input.reorganized_files = std::move(files);
    input.index_dir = index_dir;

    ReconstructionPlan plan;
    auto *plan_p = &plan;
    ReconstructionPlannerInput input_copy = input;
    std::string error_msg;

    Py_BEGIN_ALLOW_THREADS try {
        Runtime *rt = get_runtime(self);
        auto task = [plan_p, input_copy]() -> CoroTask<void> {
            ReconstructionPlannerUtility util;
            *plan_p = co_await util.process(input_copy);
        };
        rt->submit(task(), "reconstruction-planner").get();
    } catch (const std::exception &e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

    // files dict: original_path -> reconstruction info
    PyObject *py_files = PyDict_New();
    if (!py_files) return NULL;

    for (const auto &[orig_path, recon] : plan.files) {
        // checkpoint_segments: int -> list of segment dicts
        PyObject *py_segs = PyDict_New();
        if (!py_segs) {
            Py_DECREF(py_files);
            return NULL;
        }

        for (const auto &[cp_idx, seg_list] : recon.checkpoint_segments) {
            PyObject *py_seg_list =
                PyList_New(static_cast<Py_ssize_t>(seg_list.size()));
            if (!py_seg_list) {
                Py_DECREF(py_segs);
                Py_DECREF(py_files);
                return NULL;
            }
            for (std::size_t si = 0; si < seg_list.size(); si++) {
                const auto &seg = seg_list[si];
                PyObject *sd = PyDict_New();
                if (!sd) {
                    Py_DECREF(py_seg_list);
                    Py_DECREF(py_segs);
                    Py_DECREF(py_files);
                    return NULL;
                }
                PyDict_SetItemString(
                    sd, "reorg_file",
                    PyUnicode_FromString(seg.reorg_file.c_str()));
                PyDict_SetItemString(sd, "output_line_start",
                                     PyLong_FromLong(seg.output_line_start));
                PyDict_SetItemString(sd, "output_line_end",
                                     PyLong_FromLong(seg.output_line_end));
                PyDict_SetItemString(sd, "source_checkpoint",
                                     PyLong_FromLong(seg.source_checkpoint));
                PyDict_SetItemString(sd, "event_count",
                                     PyLong_FromLong(seg.event_count));
                PyList_SetItem(py_seg_list, static_cast<Py_ssize_t>(si), sd);
            }
            PyObject *py_cp_key = PyLong_FromLong(cp_idx);
            PyDict_SetItem(py_segs, py_cp_key, py_seg_list);
            Py_DECREF(py_cp_key);
            Py_DECREF(py_seg_list);
        }

        PyObject *py_recon = PyDict_New();
        if (!py_recon) {
            Py_DECREF(py_segs);
            Py_DECREF(py_files);
            return NULL;
        }
        PyDict_SetItemString(py_recon, "original_path",
                             PyUnicode_FromString(recon.original_path.c_str()));
        PyDict_SetItemString(py_recon, "num_checkpoints",
                             PyLong_FromLong(recon.num_checkpoints));
        PyDict_SetItemString(py_recon, "event_hash",
                             PyUnicode_FromString(recon.event_hash.c_str()));
        PyDict_SetItemString(py_recon, "checkpoint_segments", py_segs);
        Py_DECREF(py_segs);

        PyDict_SetItemString(py_files, orig_path.c_str(), py_recon);
        Py_DECREF(py_recon);
    }

    PyObject *result = PyDict_New();
    if (!result) {
        Py_DECREF(py_files);
        return NULL;
    }
    PyDict_SetItemString(result, "files", py_files);
    Py_DECREF(py_files);
    PyDict_SetItemString(result, "total_segments",
                         PyLong_FromSize_t(plan.total_segments));
    PyDict_SetItemString(result, "total_events",
                         PyLong_FromSize_t(plan.total_events));
    return result;
}

static PyObject *ReconstructionPlanner_call(PyObject *self, PyObject *args,
                                            PyObject *kwds) {
    return ReconstructionPlanner_plan((ReconstructionPlannerObject *)self, args,
                                      kwds);
}

static PyMethodDef ReconstructionPlanner_methods[] = {
    {"process", (PyCFunction)ReconstructionPlanner_plan,
     METH_VARARGS | METH_KEYWORDS,
     "process(reorganized_files, index_dir='')\n"
     "--\n"
     "\n"
     "Build a reconstruction plan from reorganized files.\n"
     "\n"
     "Args:\n"
     "    reorganized_files (list[str]): Paths to reorganized files.\n"
     "    index_dir (str): Directory for index sidecars (default '').\n"
     "\n"
     "Returns:\n"
     "    dict: Reconstruction plan.\n"},
    {NULL} /* Sentinel */
};

PyTypeObject ReconstructionPlannerType = {
    PyVarObject_HEAD_INIT(
        NULL,
        0) "dftracer_utils_ext.ReconstructionPlannerUtility", /* tp_name */
    sizeof(ReconstructionPlannerObject),                      /* tp_basicsize */
    0,                                                        /* tp_itemsize */
    (destructor)ReconstructionPlanner_dealloc,                /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    ReconstructionPlanner_call,               /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "ReconstructionPlannerUtility(runtime: Runtime | None = None)\n"
    "--\n"
    "\n"
    "Plan reconstruction of original files from reorganized trace files.\n"
    "\n"
    "Args:\n"
    "    runtime (Runtime or None): Runtime for thread pool control.\n"
    "        If None, uses the default global Runtime.\n"
    "\n"
    "process(reorganized_files, index_dir='') -> dict\n"
    "    reorganized_files (list[str]): Paths to reorganized trace files.\n"
    "    index_dir (str): Directory containing provenance index sidecars.\n",
    /* tp_doc */
    0,                                    /* tp_traverse */
    0,                                    /* tp_clear */
    0,                                    /* tp_richcompare */
    0,                                    /* tp_weaklistoffset */
    0,                                    /* tp_iter */
    0,                                    /* tp_iternext */
    ReconstructionPlanner_methods,        /* tp_methods */
    0,                                    /* tp_members */
    0,                                    /* tp_getset */
    0,                                    /* tp_base */
    0,                                    /* tp_dict */
    0,                                    /* tp_descr_get */
    0,                                    /* tp_descr_set */
    0,                                    /* tp_dictoffset */
    (initproc)ReconstructionPlanner_init, /* tp_init */
    0,                                    /* tp_alloc */
    ReconstructionPlanner_new,            /* tp_new */
};

int init_reconstruction_planner(PyObject *m) {
    if (PyType_Ready(&ReconstructionPlannerType) < 0) return -1;

    Py_INCREF(&ReconstructionPlannerType);
    if (PyModule_AddObject(m, "ReconstructionPlannerUtility",
                           (PyObject *)&ReconstructionPlannerType) < 0) {
        Py_DECREF(&ReconstructionPlannerType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}
