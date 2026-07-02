#define PY_SSIZE_T_CLEAN
#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/python/py_dict_helpers.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/utilities/metadata_collector.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>

#include <string>

using dftracer::utils::get_format_name;
using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using namespace dftracer::utils::utilities::composites::dft;

DFTRACER_UTILS_RUNTIME_BACKED_SLOTS(MetadataCollector, MetadataCollectorObject)

static PyObject *MetadataCollector_collect(MetadataCollectorObject *self,
                                           PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"file_path", "index_dir", NULL};
    const char *file_path;
    const char *index_dir = "";
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|s", (char **)kwlist,
                                     &file_path, &index_dir))
        return NULL;

    std::string file_path_str(file_path);
    std::string index_dir_str(index_dir);
    MetadataCollectorUtilityOutput output;

    if (!run_blocking([&] {
            Runtime *rt = resolve_runtime(self);

            MetadataCollectorUtilityInput input;
            input.file_path = file_path_str;
            input.index_path = dftracer::utils::utilities::composites::dft::
                internal::determine_index_path(file_path_str, index_dir_str);

            auto *out_p = &output;
            auto input_copy = input;
            auto task = [out_p, input_copy]() -> CoroTask<void> {
                MetadataCollectorUtility util;
                *out_p = co_await util.process(input_copy);
            };
            rt->submit(task(), "metadata-collector").get();
        })) {
        return NULL;
    }

    PyObject *d = PyDict_New();
    if (!d) return NULL;

    int rc = 0;
    rc |= dict_set_str(d, "file_path", output.file_path.c_str());
    rc |= dict_set_str(d, "index_path", output.index_path.c_str());
    rc |= dict_set_f64(d, "size_mb", output.size_mb);
    rc |= dict_set_size(d, "start_line", output.start_line);
    rc |= dict_set_size(d, "end_line", output.end_line);
    rc |= dict_set_size(d, "valid_events", output.valid_events);
    rc |= dict_set_f64(d, "size_per_line", output.size_per_line);
    rc |= dict_set_bool(d, "success", output.success);
    rc |= dict_set_bool(d, "has_index", output.has_index);
    rc |= dict_set_bool(d, "index_valid", output.index_valid);
    rc |= dict_set_u64(d, "compressed_size", output.compressed_size);
    rc |= dict_set_u64(d, "uncompressed_size", output.uncompressed_size);
    rc |= dict_set_u64(d, "num_lines", output.num_lines);
    rc |= dict_set_u64(d, "checkpoint_size", output.checkpoint_size);
    rc |= dict_set_size(d, "num_checkpoints", output.num_checkpoints);
    rc |= dict_set_str(d, "format", get_format_name(output.format));
    rc |= dict_set_str(d, "error_message", output.error_message.c_str());

    if (rc != 0) {
        Py_DECREF(d);
        return NULL;
    }

    return d;
}

static PyObject *MetadataCollector_call(PyObject *self, PyObject *args,
                                        PyObject *kwds) {
    return MetadataCollector_collect((MetadataCollectorObject *)self, args,
                                     kwds);
}

static PyMethodDef MetadataCollector_methods[] = {
    {"process", (PyCFunction)MetadataCollector_collect,
     METH_VARARGS | METH_KEYWORDS,
     "Collect metadata from a trace file.\n"
     "\n"
     "Args:\n"
     "    file_path (str): Path to the trace file.\n"
     "    index_dir (str): Directory for .dftindex stores.\n"},
    {NULL}};

PyTypeObject MetadataCollectorType = {
    PyVarObject_HEAD_INIT(
        NULL, 0) "dftracer_utils_ext.MetadataCollectorUtility", /* tp_name */
    sizeof(MetadataCollectorObject),          /* tp_basicsize */
    0,                                        /* tp_itemsize */
    (destructor)MetadataCollector_dealloc,    /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    MetadataCollector_call,                   /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "MetadataCollectorUtility(runtime: Runtime | None = None)\n"
    "--\n\n"
    "Collect metadata from a DFTracer trace file.\n\n"
    "Args:\n"
    "    runtime (Runtime or None): Runtime for thread pool control.\n"
    "\n"
    "process(file_path, index_dir='') -> dict\n"
    "    file_path (str): Path to the trace file.\n"
    "    index_dir (str): Directory for .dftindex stores.\n",
    0,                                /* tp_traverse */
    0,                                /* tp_clear */
    0,                                /* tp_richcompare */
    0,                                /* tp_weaklistoffset */
    0,                                /* tp_iter */
    0,                                /* tp_iternext */
    MetadataCollector_methods,        /* tp_methods */
    0,                                /* tp_members */
    0,                                /* tp_getset */
    0,                                /* tp_base */
    0,                                /* tp_dict */
    0,                                /* tp_descr_get */
    0,                                /* tp_descr_set */
    0,                                /* tp_dictoffset */
    (initproc)MetadataCollector_init, /* tp_init */
    0,                                /* tp_alloc */
    MetadataCollector_new,            /* tp_new */
};

int init_metadata_collector(PyObject *m) {
    if (register_type(m, &MetadataCollectorType, "MetadataCollectorUtility") <
        0)
        return -1;

    return 0;
}
