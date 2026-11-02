// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_PY_LIST_HELPERS_H
#define DFTRACER_UTILS_PYTHON_PY_LIST_HELPERS_H

#include <Python.h>

#include <cstddef>
#include <string>
#include <vector>

// Append the strings of a Python list to `out`. NULL or None is a no-op
// success (callers that require the argument reject None before calling). A
// non-list argument or a non-str item sets TypeError (mentioning `argname`)
// and returns false; `out` may be partially filled in that case.
inline bool parse_str_list(PyObject *obj, const char *argname,
                           std::vector<std::string> &out) {
    if (!obj || obj == Py_None) return true;
    if (!PyList_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "%s must be a list of str", argname);
        return false;
    }
    Py_ssize_t n = PyList_Size(obj);
    out.reserve(out.size() + static_cast<std::size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject *item = PyList_GetItem(obj, i);
        if (!PyUnicode_Check(item)) {
            PyErr_Format(PyExc_TypeError, "%s items must be str", argname);
            return false;
        }
        const char *s = PyUnicode_AsUTF8(item);
        if (!s) return false;
        out.emplace_back(s);
    }
    return true;
}

// New Python list from a vector of strings. NULL with the error set on
// failure. Each element reference is stolen into the list, so nothing leaks.
inline PyObject *str_list_from(const std::vector<std::string> &v) {
    PyObject *list = PyList_New(static_cast<Py_ssize_t>(v.size()));
    if (!list) return nullptr;
    for (std::size_t i = 0; i < v.size(); ++i) {
        PyObject *s = PyUnicode_FromStringAndSize(
            v[i].data(), static_cast<Py_ssize_t>(v[i].size()));
        if (!s) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), s);  // steals s
    }
    return list;
}

#endif  // DFTRACER_UTILS_PYTHON_PY_LIST_HELPERS_H
