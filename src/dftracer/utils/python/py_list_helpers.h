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

#endif  // DFTRACER_UTILS_PYTHON_PY_LIST_HELPERS_H
