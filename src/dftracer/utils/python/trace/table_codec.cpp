// SPDX-License-Identifier: MIT
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/python/core/call.h>
#include <dftracer/utils/python/core/error_exception.h>
#include <dftracer/utils/python/core/seq_helpers.h>
#include <dftracer/utils/python/frame/dataframe.h>
#include <dftracer/utils/python/trace/table_codec.h>
#include <dftracer/utils/trace/table/table_codec.h>

using dftracer::utils::python::core::guard;
using dftracer::utils::python::core::py_unwrap;
using dftracer::utils::python::core::release_gil;
using namespace dftracer::utils;

namespace {

// _write_table(frame, path, header_json, raw_columns, level, threads) -> None
PyObject *write_table_py(PyObject *, PyObject *args) {
    PyObject *frame_obj = nullptr;
    PyObject *raw_obj = nullptr;
    const char *path = nullptr;
    const char *header = nullptr;
    Py_ssize_t path_size = 0;
    Py_ssize_t header_size = 0;
    int level = 6;
    Py_ssize_t threads = 0;
    if (!PyArg_ParseTuple(args, "Os#s#Oin", &frame_obj, &path, &path_size,
                          &header, &header_size, &raw_obj, &level, &threads))
        return nullptr;
    if (threads < 0) {
        PyErr_SetString(PyExc_ValueError, "threads must be >= 0");
        return nullptr;
    }
    dataframe::DataFrame frame;
    if (!python::frame::unwrap_dataframe(frame_obj, frame)) return nullptr;
    dstd::Vector<dstd::String> raw_names;
    if (!python::core::parse_string_seq(
            raw_obj, "raw_columns must be a sequence of str", raw_names))
        return nullptr;
    const dstd::String path_text(path, static_cast<std::size_t>(path_size));
    const dstd::String header_text(header,
                                   static_cast<std::size_t>(header_size));
    dstd::Vector<dstd::StringView> raw_views;
    for (const dstd::String &n : raw_names) raw_views.push_back(n);

    trace::table::WriteOptions opts;
    opts.raw_columns = raw_views;
    opts.level = level;
    opts.workers = static_cast<std::size_t>(threads);
    opts.compress = path_text.size() >= 3 &&
                    path_text.compare(path_text.size() - 3, 3, ".gz") == 0;
    return guard([&]() -> PyObject * {
        py_unwrap(release_gil([&] {
            return trace::table::write_table(frame, path_text, header_text,
                                             opts);
        }));
        Py_RETURN_NONE;
    });
}

// _read_table(path, threads) -> (DataFrame, header_json)
PyObject *read_table_py(PyObject *, PyObject *args) {
    const char *path = nullptr;
    Py_ssize_t path_size = 0;
    Py_ssize_t threads = 0;
    if (!PyArg_ParseTuple(args, "s#n", &path, &path_size, &threads))
        return nullptr;
    if (threads < 0) {
        PyErr_SetString(PyExc_ValueError, "threads must be >= 0");
        return nullptr;
    }
    const dstd::String path_text(path, static_cast<std::size_t>(path_size));
    trace::table::ReadOptions opts;
    opts.parts = static_cast<std::size_t>(threads);
    return guard([&]() -> PyObject * {
        auto table = py_unwrap(release_gil(
            [&] { return trace::table::read_table(path_text, opts); }));
        PyObject *header = PyUnicode_FromStringAndSize(
            table.header_json.data(),
            static_cast<Py_ssize_t>(table.header_json.size()));
        if (!header) return nullptr;
        PyObject *frame = python::frame::wrap_dataframe(std::move(table.frame));
        if (!frame) {
            Py_DECREF(header);
            return nullptr;
        }
        PyObject *pair = PyTuple_Pack(2, frame, header);
        Py_DECREF(frame);
        Py_DECREF(header);
        return pair;
    });
}

PyMethodDef WRITE_DEF = {
    "_write_table", write_table_py, METH_VARARGS,
    "_write_table(frame, path, header_json, raw_columns, level, threads) -> "
    "None\n\nWrite a native DataFrame as a dftu.table file."};
PyMethodDef READ_DEF = {
    "_read_table", read_table_py, METH_VARARGS,
    "_read_table(path, threads) -> (DataFrame, str)\n\nRead a dftu.table "
    "file into a native DataFrame and the args of its header."};

}  // namespace

int dftracer::utils::python::trace::init_table_codec(PyObject *m) {
    PyMethodDef *defs[] = {&WRITE_DEF, &READ_DEF};
    for (PyMethodDef *def : defs) {
        PyObject *fn = PyCFunction_NewEx(def, nullptr, nullptr);
        if (!fn) return -1;
        if (PyModule_AddObject(m, def->ml_name, fn) < 0) {
            Py_DECREF(fn);
            return -1;
        }
    }
    return 0;
}
