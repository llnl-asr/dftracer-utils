#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/python/arrow_parallel_reader.h>
#include <dftracer/utils/python/py_dict_helpers.h>
#include <dftracer/utils/python/py_list_helpers.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/trace_reader_iterator.h>
#include <dftracer/utils/utilities/common/arrow/parallel_reader.h>

#include <string>
#include <vector>

namespace dftracer::utils::python {

using utilities::common::arrow::ArrowExportResult;
using utilities::common::arrow::read_arrow_files_parallel;

static PyObject* py_read_arrow_files_parallel(PyObject* /*self*/,
                                              PyObject* args,
                                              PyObject* kwargs) {
    static const char* kwlist[] = {"paths", "runtime", nullptr};
    PyObject* paths_obj = nullptr;
    PyObject* runtime_obj = nullptr;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O",
                                     const_cast<char**>(kwlist), &paths_obj,
                                     &runtime_obj)) {
        return nullptr;
    }

    // Convert paths to vector<string>
    std::vector<std::string> paths;
    if (!parse_str_list(paths_obj, "paths", paths)) return nullptr;

    // Get runtime
    Runtime* runtime = nullptr;
    if (runtime_obj && runtime_obj != Py_None) {
        if (!PyObject_TypeCheck(runtime_obj, &RuntimeType)) {
            PyErr_SetString(PyExc_TypeError,
                            "runtime must be a Runtime object");
            return nullptr;
        }
        runtime = ((RuntimeObject*)runtime_obj)->runtime.get();
    } else {
        runtime = get_default_runtime();
    }

    // Call C++ parallel reader (releases GIL during file I/O)
    utilities::common::arrow::ParallelReadResult result;
    if (!run_blocking_r(
            [&] {
                auto task = read_arrow_files_parallel(std::move(paths));
                return runtime->submit(std::move(task), "read_arrow_files")
                    .get();
            },
            result)) {
        return nullptr;
    }

    // Build Python result dict
    PyObject* file_results_list = PyList_New(result.file_results.size());
    if (!file_results_list) return nullptr;

    for (std::size_t i = 0; i < result.file_results.size(); ++i) {
        const auto& fr = result.file_results[i];
        PyObject* fr_dict = PyDict_New();
        if (!fr_dict) {
            Py_DECREF(file_results_list);
            return nullptr;
        }

        dict_set_str(fr_dict, "path", fr.path.c_str());
        dict_set_bool(fr_dict, "success", fr.success);

        if (!fr.error.empty()) {
            dict_set_str(fr_dict, "error", fr.error.c_str());
        } else {
            Py_INCREF(Py_None);
            PyDict_SetItemString(fr_dict, "error", Py_None);
        }

        dict_set_i64(fr_dict, "total_rows", fr.total_rows);

        // batches - list of ArrowBatchCapsule objects
        PyObject* batches_list = PyList_New(fr.batches->size());
        if (!batches_list) {
            Py_DECREF(fr_dict);
            Py_DECREF(file_results_list);
            return nullptr;
        }

        for (std::size_t j = 0; j < fr.batches->size(); ++j) {
            ArrowBatchCapsuleObject* capsule =
                (ArrowBatchCapsuleObject*)ArrowBatchCapsuleType.tp_alloc(
                    &ArrowBatchCapsuleType, 0);
            if (!capsule) {
                Py_DECREF(batches_list);
                Py_DECREF(fr_dict);
                Py_DECREF(file_results_list);
                return nullptr;
            }
            // Move the batch into the capsule
            capsule->result =
                new ArrowExportResult(std::move((*fr.batches)[j]));
            PyList_SetItem(batches_list, j, (PyObject*)capsule);
        }

        PyDict_SetItemString(fr_dict, "batches", batches_list);
        Py_DECREF(batches_list);

        PyList_SetItem(file_results_list, i, fr_dict);
    }

    // Build final result dict
    PyObject* result_dict = PyDict_New();
    if (!result_dict) {
        Py_DECREF(file_results_list);
        return nullptr;
    }

    PyDict_SetItemString(result_dict, "file_results", file_results_list);
    Py_DECREF(file_results_list);

    dict_set_i64(result_dict, "total_rows", result.total_rows);
    dict_set_i64(result_dict, "total_batches", result.total_batches);
    dict_set_size(result_dict, "files_read", result.files_read);
    dict_set_size(result_dict, "files_failed", result.files_failed);

    return result_dict;
}

static PyMethodDef arrow_parallel_reader_methods[] = {
    {"read_arrow_files_parallel", (PyCFunction)py_read_arrow_files_parallel,
     METH_VARARGS | METH_KEYWORDS,
     "Read multiple Arrow IPC files in parallel using the Runtime.\n\n"
     "Args:\n"
     "    paths: List of file paths to read.\n"
     "    runtime: Optional Runtime object. Uses default if not provided.\n\n"
     "Returns:\n"
     "    dict with:\n"
     "        - file_results: List of per-file results, each with:\n"
     "            - path: File path\n"
     "            - success: True if read succeeded\n"
     "            - error: Error message if failed, else None\n"
     "            - total_rows: Number of rows in file\n"
     "            - batches: List of ArrowBatch objects\n"
     "        - total_rows: Total rows across all files\n"
     "        - total_batches: Total batches across all files\n"
     "        - files_read: Number of files read successfully\n"
     "        - files_failed: Number of files that failed"},
    {nullptr, nullptr, 0, nullptr}};

int init_arrow_parallel_reader(PyObject* m) {
    // Add the function to the module
    if (PyModule_AddFunctions(m, arrow_parallel_reader_methods) < 0) {
        return -1;
    }
    return 0;
}

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
