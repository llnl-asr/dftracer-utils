#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/utilities/view_builder.h>
#include <dftracer/utils/utilities/composites/dft/views/view_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>

#include <string>
#include <vector>

using dftracer::utils::Runtime;
using namespace dftracer::utils::utilities::composites::dft::views;

static Runtime *get_runtime(ViewBuilderObject *self) {
    if (self->runtime_obj)
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    return get_default_runtime();
}

static void ViewBuilder_dealloc(ViewBuilderObject *self) {
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *ViewBuilder_new(PyTypeObject *type, PyObject *args,
                                 PyObject *kwds) {
    ViewBuilderObject *self;
    self = (ViewBuilderObject *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->runtime_obj = NULL;
    }
    return (PyObject *)self;
}

static int ViewBuilder_init(ViewBuilderObject *self, PyObject *args,
                            PyObject *kwds) {
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

static PyObject *ViewBuilder_build(ViewBuilderObject *self, PyObject *args,
                                   PyObject *kwds) {
    using dftracer::utils::coro::CoroTask;

    static const char *kwlist[] = {"file_path", "predicates", "index_dir",
                                   NULL};
    const char *file_path;
    PyObject *predicates_obj = Py_None;
    const char *index_dir = "";

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|Os", (char **)kwlist,
                                     &file_path, &predicates_obj, &index_dir))
        return NULL;

    ViewDefinition view;
    if (predicates_obj && predicates_obj != Py_None) {
        if (!PyDict_Check(predicates_obj)) {
            PyErr_SetString(PyExc_TypeError,
                            "predicates must be a dict or None");
            return NULL;
        }
        ViewPredicate pred;
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(predicates_obj, &pos, &key, &value)) {
            const char *dim_cstr = PyUnicode_AsUTF8(key);
            if (!dim_cstr) return NULL;
            std::string dim(dim_cstr);
            std::vector<std::string> vals;
            if (!PyList_Check(value)) {
                PyErr_SetString(PyExc_TypeError,
                                "predicate values must be lists of str");
                return NULL;
            }
            Py_ssize_t n = PyList_Size(value);
            for (Py_ssize_t i = 0; i < n; i++) {
                PyObject *item = PyList_GetItem(value, i);
                const char *s = PyUnicode_AsUTF8(item);
                if (!s) return NULL;
                vals.emplace_back(s);
            }
            pred.with_bloom_dim(dim, vals);
        }
        view.with_predicate(std::move(pred));
    }

    ViewBuilderInput input;
    input.view = std::move(view);
    input.file_path = file_path;
    input.idx_path = std::string(file_path) + ".idx";

    ViewBuilderOutput output;
    auto *out_p = &output;
    ViewBuilderInput input_copy = input;
    std::string error_msg;

    Py_BEGIN_ALLOW_THREADS try {
        Runtime *rt = get_runtime(self);
        auto task = [out_p, input_copy]() -> CoroTask<void> {
            ViewBuilderUtility util;
            *out_p = co_await util.process(input_copy);
        };
        rt->submit(task(), "view-builder").get();
    } catch (const std::exception &e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

    PyObject *candidates =
        PyList_New(static_cast<Py_ssize_t>(output.candidates.size()));
    if (!candidates) return NULL;

    for (std::size_t i = 0; i < output.candidates.size(); i++) {
        const auto &c = output.candidates[i];
        PyObject *entry = PyDict_New();
        if (!entry) {
            Py_DECREF(candidates);
            return NULL;
        }
        PyDict_SetItemString(entry, "checkpoint_idx",
                             PyLong_FromUnsignedLongLong(c.checkpoint_idx));
        PyDict_SetItemString(entry, "start_byte",
                             PyLong_FromSize_t(c.start_byte));
        PyDict_SetItemString(entry, "end_byte", PyLong_FromSize_t(c.end_byte));
        PyList_SetItem(candidates, static_cast<Py_ssize_t>(i), entry);
    }

    PyObject *result = PyDict_New();
    if (!result) {
        Py_DECREF(candidates);
        return NULL;
    }
    PyDict_SetItemString(result, "file_may_match",
                         PyBool_FromLong(output.file_may_match ? 1 : 0));
    PyDict_SetItemString(result, "candidates", candidates);
    Py_DECREF(candidates);
    PyDict_SetItemString(result, "total_checkpoints",
                         PyLong_FromUnsignedLongLong(output.total_checkpoints));
    PyDict_SetItemString(
        result, "skipped_checkpoints",
        PyLong_FromUnsignedLongLong(output.skipped_checkpoints));
    PyDict_SetItemString(result, "success",
                         PyBool_FromLong(output.success ? 1 : 0));
    return result;
}

static PyObject *ViewBuilder_call(PyObject *self, PyObject *args,
                                  PyObject *kwds) {
    return ViewBuilder_build((ViewBuilderObject *)self, args, kwds);
}

static PyMethodDef ViewBuilder_methods[] = {
    {"process", (PyCFunction)ViewBuilder_build, METH_VARARGS | METH_KEYWORDS,
     "Build view candidates from the index"},
    {NULL} /* Sentinel */
};

PyTypeObject ViewBuilderType = {
    PyVarObject_HEAD_INIT(
        NULL, 0) "dftracer_utils_ext.ViewBuilderUtility", /* tp_name */
    sizeof(ViewBuilderObject),                            /* tp_basicsize */
    0,                                                    /* tp_itemsize */
    (destructor)ViewBuilder_dealloc,                      /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    ViewBuilder_call,                         /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "ViewBuilderUtility(runtime: Runtime | None = None)\n"
    "--\n"
    "\n"
    "Query the bloom-filter index to find candidate chunks.\n"
    "\n"
    "Args:\n"
    "    runtime (Runtime or None): Runtime for thread pool control.\n"
    "        If None, uses the default global Runtime.\n"
    "\n"
    "process(file_path, predicates=None, index_dir='') -> dict\n"
    "    file_path (str): Path to the trace file.\n"
    "    predicates (dict or None): Bloom dimension filters.\n"
    "        Keys are dimension names, values are lists of strings.\n"
    "    index_dir (str): Directory containing the index sidecar.\n", /* tp_doc
                                                                       */
    0,                          /* tp_traverse */
    0,                          /* tp_clear */
    0,                          /* tp_richcompare */
    0,                          /* tp_weaklistoffset */
    0,                          /* tp_iter */
    0,                          /* tp_iternext */
    ViewBuilder_methods,        /* tp_methods */
    0,                          /* tp_members */
    0,                          /* tp_getset */
    0,                          /* tp_base */
    0,                          /* tp_dict */
    0,                          /* tp_descr_get */
    0,                          /* tp_descr_set */
    0,                          /* tp_dictoffset */
    (initproc)ViewBuilder_init, /* tp_init */
    0,                          /* tp_alloc */
    ViewBuilder_new,            /* tp_new */
};

int init_view_builder(PyObject *m) {
    if (PyType_Ready(&ViewBuilderType) < 0) return -1;

    Py_INCREF(&ViewBuilderType);
    if (PyModule_AddObject(m, "ViewBuilderUtility",
                           (PyObject *)&ViewBuilderType) < 0) {
        Py_DECREF(&ViewBuilderType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}
