// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_PY_STR_HELPERS_H
#define DFTRACER_UTILS_PYTHON_PY_STR_HELPERS_H

#include <Python.h>

// Single-string plumbing shared across the CPython bindings. List<->vector
// conversion lives in py_list_helpers.h (parse_str_list / str_list_from).

// Borrowed UTF-8 bytes of a str object, valid while `obj` lives. NULL with the
// Python error set if `obj` is null or not a str.
inline const char *as_utf8(PyObject *obj) {
    return obj ? PyUnicode_AsUTF8(obj) : nullptr;
}

#endif  // DFTRACER_UTILS_PYTHON_PY_STR_HELPERS_H
