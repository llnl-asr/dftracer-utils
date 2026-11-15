// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/python/core/errors.h>

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/arrow/arrow.h>
#include <dftracer/utils/arrow/ipc.h>
#include <dftracer/utils/python/core/dict_helpers.h>
#include <dftracer/utils/python/core/list_helpers.h>
#include <dftracer/utils/python/core/method.h>
#include <dftracer/utils/python/core/runtime_mixin.h>
#include <dftracer/utils/python/runtime/runtime.h>
#include <dftracer/utils/python/trace/arrow_parallel_reader.h>
#include <dftracer/utils/python/trace/trace_reader_iterator.h>

namespace dftracer::utils::python::trace {

using dftracer::utils::arrow::Batch;
using dftracer::utils::arrow::Reader;

namespace {

/// One file: its batches as Arrow C structs, or the error that stopped it.
struct FileRead {
    dftracer::utils::dstd::String path;
    dftracer::utils::dstd::Vector<Batch> batches;
    dftracer::utils::dstd::String error;
    std::int64_t total_rows = 0;

    bool success() const { return error.empty(); }
};

FileRead read_file(const dftracer::utils::dstd::Path& path,
                   const dftracer::utils::dstd::String& name) {
    FileRead out;
    out.path = name;
    auto reader = Reader::open(path);
    if (!reader) {
        out.error = reader.error().message().data();
        return out;
    }
    while (true) {
        auto batch = reader->next();
        if (!batch) {
            out.error = batch.error().message().data();
            return out;
        }
        if (!batch->has_value()) break;
        auto exported = arrow::to_arrow(**batch);
        if (!exported) {
            out.error = exported.error().message().data();
            return out;
        }
        out.total_rows += exported->num_rows();
        out.batches.push_back(std::move(*exported));
    }
    return out;
}

}  // namespace

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

    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> names;
    if (!parse_str_list(paths_obj, "paths", names)) return nullptr;

    Runtime* runtime = nullptr;
    if (runtime_obj && runtime_obj != Py_None) {
        if (!PyObject_TypeCheck(runtime_obj, &RuntimeType)) {
            PyErr_SetString(PyExc_TypeError,
                            "runtime must be a Runtime object");
            return nullptr;
        }
        runtime = ((RuntimeObject*)runtime_obj)->runtime.get();
    } else {
        runtime = runtime::get_default_runtime();
    }

    dftracer::utils::dstd::Vector<dftracer::utils::dstd::Path> paths;
    paths.reserve(names.size());
    for (const auto& name : names)
        paths.emplace_back(
            dftracer::utils::dstd::StringView(name.data(), name.size()));

    // One file per worker; the GIL is released for the whole read.
    dftracer::utils::dstd::Vector<FileRead> files(paths.size());
    dftracer::utils::dstd::Result<void> status{};
    Py_BEGIN_ALLOW_THREADS auto read_all = [&](std::int64_t begin,
                                               std::int64_t end) {
        for (std::int64_t i = begin; i < end; ++i)
            files[static_cast<std::size_t>(i)] =
                read_file(paths[static_cast<std::size_t>(i)],
                          names[static_cast<std::size_t>(i)]);
    };
    status = runtime->parallel_for(static_cast<std::int64_t>(paths.size()), 1,
                                   read_all);
    Py_END_ALLOW_THREADS if (!status) {
        PyErr_SetString(PyExc_RuntimeError, status.error().c_str());
        return nullptr;
    }

    std::int64_t total_rows = 0;
    std::int64_t total_batches = 0;
    std::size_t files_read = 0;
    std::size_t files_failed = 0;

    PyObject* file_results_list = PyList_New(files.size());
    if (!file_results_list) return nullptr;
    for (std::size_t i = 0; i < files.size(); ++i) {
        FileRead& fr = files[i];
        PyObject* fr_dict = PyDict_New();
        if (!fr_dict) {
            Py_DECREF(file_results_list);
            return nullptr;
        }
        dict_set_str(fr_dict, "path", fr.path.c_str());
        dict_set_bool(fr_dict, "success", fr.success());
        if (fr.error.empty()) {
            Py_INCREF(Py_None);
            PyDict_SetItemString(fr_dict, "error", Py_None);
            ++files_read;
        } else {
            dict_set_str(fr_dict, "error", fr.error.c_str());
            ++files_failed;
        }
        dict_set_i64(fr_dict, "total_rows", fr.total_rows);
        total_rows += fr.total_rows;
        total_batches += static_cast<std::int64_t>(fr.batches.size());

        PyObject* batches_list = PyList_New(fr.batches.size());
        if (!batches_list) {
            Py_DECREF(fr_dict);
            Py_DECREF(file_results_list);
            return nullptr;
        }
        for (std::size_t j = 0; j < fr.batches.size(); ++j) {
            auto* capsule =
                (ArrowBatchCapsuleObject*)ArrowBatchCapsuleType.tp_alloc(
                    &ArrowBatchCapsuleType, 0);
            if (!capsule) {
                Py_DECREF(batches_list);
                Py_DECREF(fr_dict);
                Py_DECREF(file_results_list);
                return nullptr;
            }
            capsule->result = new Batch(std::move(fr.batches[j]));
            PyList_SetItem(batches_list, static_cast<Py_ssize_t>(j),
                           (PyObject*)capsule);
        }
        PyDict_SetItemString(fr_dict, "batches", batches_list);
        Py_DECREF(batches_list);
        PyList_SetItem(file_results_list, static_cast<Py_ssize_t>(i), fr_dict);
    }

    PyObject* result_dict = PyDict_New();
    if (!result_dict) {
        Py_DECREF(file_results_list);
        return nullptr;
    }
    PyDict_SetItemString(result_dict, "file_results", file_results_list);
    Py_DECREF(file_results_list);
    dict_set_i64(result_dict, "total_rows", total_rows);
    dict_set_i64(result_dict, "total_batches", total_batches);
    dict_set_size(result_dict, "files_read", files_read);
    dict_set_size(result_dict, "files_failed", files_failed);
    return result_dict;
}

static PyMethodDef arrow_parallel_reader_methods[] = {
    {"read_arrow_files_parallel",
     DFTU_PYCFUNCTION(py_read_arrow_files_parallel),
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
    if (PyModule_AddFunctions(m, arrow_parallel_reader_methods) < 0) {
        return -1;
    }
    return 0;
}

}  // namespace dftracer::utils::python::trace
