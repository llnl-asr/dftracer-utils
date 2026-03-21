#define PY_SSIZE_T_CLEAN
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/python/arrow_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/trace_reader_iterator.h>
#include <dftracer/utils/python/utilities/view_reader.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <dftracer/utils/utilities/composites/dft/views/view_reader_utility.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <nanoarrow/nanoarrow.h>
#include <yyjson.h>
#endif

#include <string>
#include <vector>

using dftracer::utils::Runtime;
using namespace dftracer::utils::utilities::composites::dft::views;

using dftracer::utils::python::wrap_arrow_result;
using dftracer::utils::python::wrap_arrow_table;

static Runtime *get_runtime(ViewReaderObject *self) {
    if (self->runtime_obj)
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    return get_default_runtime();
}

static void ViewReader_dealloc(ViewReaderObject *self) {
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *ViewReader_new(PyTypeObject *type, PyObject *args,
                                PyObject *kwds) {
    ViewReaderObject *self = (ViewReaderObject *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->runtime_obj = NULL;
    }
    return (PyObject *)self;
}

static int ViewReader_init(ViewReaderObject *self, PyObject *args,
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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int parse_predicates(PyObject *predicates_obj, ViewDefinition &view) {
    if (!predicates_obj || predicates_obj == Py_None) return 0;
    if (!PyDict_Check(predicates_obj)) {
        PyErr_SetString(PyExc_TypeError, "predicates must be a dict or None");
        return -1;
    }
    ViewPredicate pred;
    PyObject *key, *value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(predicates_obj, &pos, &key, &value)) {
        const char *dim_cstr = PyUnicode_AsUTF8(key);
        if (!dim_cstr) return -1;
        if (!PyList_Check(value)) {
            PyErr_SetString(PyExc_TypeError,
                            "predicate values must be lists of str");
            return -1;
        }
        std::vector<std::string> vals;
        Py_ssize_t n = PyList_Size(value);
        for (Py_ssize_t i = 0; i < n; i++) {
            const char *s = PyUnicode_AsUTF8(PyList_GetItem(value, i));
            if (!s) return -1;
            vals.emplace_back(s);
        }
        pred.with_bloom_dim(std::string(dim_cstr), std::move(vals));
    }
    view.with_predicate(std::move(pred));
    return 0;
}

struct CollectedOutput {
    std::vector<std::string> events;
    std::uint64_t events_matched = 0;
    std::uint64_t events_scanned = 0;
};

static CollectedOutput run_view_reader(ViewReaderObject *self,
                                       const char *file_path,
                                       const char *index_dir,
                                       ViewDefinition view,
                                       std::string &error_msg) {
    using dftracer::utils::coro::CoroTask;

    ViewReaderInput input;
    input.file_path = file_path;
    input.idx_path = std::string(file_path) + ".idx";
    input.view = std::move(view);

    CollectedOutput output;
    auto *out_p = &output;
    ViewReaderInput input_copy = input;

    Py_BEGIN_ALLOW_THREADS try {
        Runtime *rt = get_runtime(self);
        auto task = [out_p, input_copy]() -> CoroTask<void> {
            ViewReaderUtility util;
            auto gen = util.process(input_copy);
            while (auto batch = co_await gen.next()) {
                out_p->events_matched += batch->events_matched;
                out_p->events_scanned += batch->events_scanned;
                for (auto &ev : batch->events)
                    out_p->events.push_back(std::move(ev));
            }
        };
        rt->submit(task(), "view-reader").get();
    } catch (const std::exception &e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        return output;
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW

using dftracer::utils::utilities::common::arrow::ArrowExportResult;
using dftracer::utils::utilities::common::arrow::ColumnType;
using dftracer::utils::utilities::common::arrow::RecordBatchBuilder;

// Build a single Arrow batch from a slice of event strings.
// held_docs and held_serialized are populated and must be freed by the caller
// after finish() returns.
static bool build_batch(const std::vector<std::string> &events,
                        std::size_t start, std::size_t end,
                        RecordBatchBuilder &builder,
                        std::vector<yyjson_doc *> &held_docs,
                        std::vector<std::string> &held_serialized) {
    builder.reserve(end - start);
    for (std::size_t i = start; i < end; ++i) {
        const auto &event_str = events[i];
        yyjson_doc *doc = yyjson_read(event_str.data(), event_str.size(), 0);
        if (!doc) continue;

        yyjson_val *root = yyjson_doc_get_root(doc);
        if (!root || !yyjson_is_obj(root)) {
            yyjson_doc_free(doc);
            continue;
        }
        held_docs.push_back(doc);

        yyjson_obj_iter it;
        yyjson_obj_iter_init(root, &it);
        yyjson_val *key;
        while ((key = yyjson_obj_iter_next(&it))) {
            yyjson_val *val = yyjson_obj_iter_get_val(key);
            std::string_view key_sv(yyjson_get_str(key), yyjson_get_len(key));

            if (yyjson_is_int(val)) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::INT64);
                builder.append_int64(ci, yyjson_get_sint(val));
            } else if (yyjson_is_uint(val)) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::UINT64);
                builder.append_uint64(ci, yyjson_get_uint(val));
            } else if (yyjson_is_real(val)) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::DOUBLE);
                builder.append_double(ci, yyjson_get_real(val));
            } else if (yyjson_is_bool(val)) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::BOOL);
                builder.append_bool(ci, yyjson_get_bool(val));
            } else if (yyjson_is_str(val)) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::STRING);
                builder.append_string(
                    ci,
                    std::string_view(yyjson_get_str(val), yyjson_get_len(val)));
            } else if (yyjson_is_null(val)) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::STRING);
                builder.append_null(ci);
            } else {
                // object/array: serialize as JSON string
                auto ci = builder.add_or_get_column(key_sv, ColumnType::STRING);
                std::size_t jlen;
                char *js = yyjson_val_write(val, 0, &jlen);
                if (js) {
                    held_serialized.emplace_back(js, jlen);
                    free(js);
                    builder.append_string(ci, held_serialized.back());
                } else {
                    builder.append_null(ci);
                }
            }
        }
        builder.end_row();
    }
    return builder.num_rows() > 0;
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW

// ---------------------------------------------------------------------------
// process() — returns ArrowTable (materialized single batch)
// ---------------------------------------------------------------------------

static PyObject *ViewReader_read_events(ViewReaderObject *self, PyObject *args,
                                        PyObject *kwds) {
    static const char *kwlist[] = {"file_path", "predicates", "index_dir",
                                   NULL};
    const char *file_path;
    PyObject *predicates_obj = Py_None;
    const char *index_dir = "";

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|Os", (char **)kwlist,
                                     &file_path, &predicates_obj, &index_dir))
        return NULL;

    ViewDefinition view;
    if (parse_predicates(predicates_obj, view) < 0) return NULL;

    std::string error_msg;
    CollectedOutput output =
        run_view_reader(self, file_path, index_dir, std::move(view), error_msg);
    if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    PyObject *batch_list = PyList_New(0);
    if (!batch_list) return NULL;

    if (!output.events.empty()) {
        RecordBatchBuilder builder;
        std::vector<yyjson_doc *> held_docs;
        std::vector<std::string> held_serialized;

        build_batch(output.events, 0, output.events.size(), builder, held_docs,
                    held_serialized);

        if (builder.num_rows() > 0) {
            auto arrow_result = builder.finish();
            for (auto *d : held_docs) yyjson_doc_free(d);

            PyObject *cap = wrap_arrow_result(std::move(arrow_result));
            if (!cap) {
                Py_DECREF(batch_list);
                return NULL;
            }
            int rc = PyList_Append(batch_list, cap);
            Py_DECREF(cap);
            if (rc < 0) {
                Py_DECREF(batch_list);
                return NULL;
            }
        } else {
            for (auto *d : held_docs) yyjson_doc_free(d);
        }
    }

    return wrap_arrow_table(batch_list);

#else
    PyErr_SetString(PyExc_RuntimeError,
                    "dftracer-utils was built without Arrow support");
    return NULL;
#endif
}

// ---------------------------------------------------------------------------
// iter_arrow() — returns list iterator yielding ArrowBatch capsules
// ---------------------------------------------------------------------------

static PyObject *ViewReader_iter_arrow(ViewReaderObject *self, PyObject *args,
                                       PyObject *kwds) {
    static const char *kwlist[] = {"file_path", "predicates", "index_dir",
                                   "batch_size", NULL};
    const char *file_path;
    PyObject *predicates_obj = Py_None;
    const char *index_dir = "";
    Py_ssize_t batch_size = 10000;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|Osn", (char **)kwlist,
                                     &file_path, &predicates_obj, &index_dir,
                                     &batch_size))
        return NULL;

    if (batch_size <= 0) {
        PyErr_SetString(PyExc_ValueError, "batch_size must be positive");
        return NULL;
    }

    ViewDefinition view;
    if (parse_predicates(predicates_obj, view) < 0) return NULL;

    std::string error_msg;
    CollectedOutput output =
        run_view_reader(self, file_path, index_dir, std::move(view), error_msg);
    if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    PyObject *batch_list = PyList_New(0);
    if (!batch_list) return NULL;

    const std::size_t n = output.events.size();
    const std::size_t bsz = static_cast<std::size_t>(batch_size);

    for (std::size_t start = 0; start < n; start += bsz) {
        std::size_t end = (start + bsz < n) ? start + bsz : n;

        RecordBatchBuilder builder;
        std::vector<yyjson_doc *> held_docs;
        std::vector<std::string> held_serialized;

        build_batch(output.events, start, end, builder, held_docs,
                    held_serialized);

        if (builder.num_rows() == 0) {
            for (auto *d : held_docs) yyjson_doc_free(d);
            continue;
        }

        auto arrow_result = builder.finish();
        for (auto *d : held_docs) yyjson_doc_free(d);

        PyObject *cap = wrap_arrow_result(std::move(arrow_result));
        if (!cap) {
            Py_DECREF(batch_list);
            return NULL;
        }
        int rc = PyList_Append(batch_list, cap);
        Py_DECREF(cap);
        if (rc < 0) {
            Py_DECREF(batch_list);
            return NULL;
        }
    }

    PyObject *it = PyObject_GetIter(batch_list);
    Py_DECREF(batch_list);
    return it;

#else
    PyErr_SetString(PyExc_RuntimeError,
                    "dftracer-utils was built without Arrow support");
    return NULL;
#endif
}

static PyObject *ViewReader_call(PyObject *self, PyObject *args,
                                 PyObject *kwds) {
    return ViewReader_read_events((ViewReaderObject *)self, args, kwds);
}

static PyMethodDef ViewReader_methods[] = {
    {"process", (PyCFunction)ViewReader_read_events,
     METH_VARARGS | METH_KEYWORDS,
     "process(file_path, predicates=None, index_dir='')\n"
     "--\n"
     "\n"
     "Read matching events as a materialized ArrowTable.\n"
     "\n"
     "Args:\n"
     "    file_path (str): Path to the trace file.\n"
     "    predicates (dict or None): Bloom dimension filters\n"
     "        (default None, no filtering).\n"
     "    index_dir (str): Directory for index sidecars (default '').\n"
     "\n"
     "Returns:\n"
     "    ArrowTable: Materialized table of matching events.\n"},
    {"iter_arrow", (PyCFunction)ViewReader_iter_arrow,
     METH_VARARGS | METH_KEYWORDS,
     "iter_arrow(file_path, predicates=None, index_dir='',\n"
     "           batch_size=10000)\n"
     "--\n"
     "\n"
     "Stream matching events as Arrow batches.\n"
     "\n"
     "Args:\n"
     "    file_path (str): Path to the trace file.\n"
     "    predicates (dict or None): Bloom dimension filters\n"
     "        (default None, no filtering).\n"
     "    index_dir (str): Directory for index sidecars (default '').\n"
     "    batch_size (int): Maximum rows per batch (default 10000).\n"
     "\n"
     "Returns:\n"
     "    Iterator[ArrowBatch]: Arrow record batches.\n"},
    {NULL} /* Sentinel */
};

PyTypeObject ViewReaderType = {
    PyVarObject_HEAD_INIT(
        NULL, 0) "dftracer_utils_ext.ViewReaderUtility", /* tp_name */
    sizeof(ViewReaderObject),                            /* tp_basicsize */
    0,                                                   /* tp_itemsize */
    (destructor)ViewReader_dealloc,                      /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    ViewReader_call,                          /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "ViewReaderUtility(runtime: Runtime | None = None)\n"
    "--\n"
    "\n"
    "Read events from a trace file filtered by view predicates.\n"
    "\n"
    "Args:\n"
    "    runtime (Runtime or None): Runtime for thread pool control.\n"
    "        If None, uses the default global Runtime.\n", /* tp_doc */
    0,                                                     /* tp_traverse */
    0,                                                     /* tp_clear */
    0,                                                     /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    ViewReader_methods,        /* tp_methods */
    0,                         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)ViewReader_init, /* tp_init */
    0,                         /* tp_alloc */
    ViewReader_new,            /* tp_new */
};

int init_view_reader(PyObject *m) {
    if (PyType_Ready(&ViewReaderType) < 0) return -1;

    Py_INCREF(&ViewReaderType);
    if (PyModule_AddObject(m, "ViewReaderUtility",
                           (PyObject *)&ViewReaderType) < 0) {
        Py_DECREF(&ViewReaderType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}
