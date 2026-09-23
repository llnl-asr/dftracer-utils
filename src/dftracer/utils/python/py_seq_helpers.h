#ifndef DFTRACER_UTILS_PYTHON_PY_SEQ_HELPERS_H
#define DFTRACER_UTILS_PYTHON_PY_SEQ_HELPERS_H

#include <Python.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::python {

// Convert each item of a Python sequence via `convert`, appending to `out`.
// `msg` is the TypeError text set when `obj` is not a sequence. Owns the
// PySequence_Fast handle and releases it on every path, including errors.
// `convert(item, out)` returns false with a Python error already set to abort;
// `out` may be partially filled in that case.
template <class T, class Convert>
inline bool parse_seq(PyObject* obj, const char* msg, std::vector<T>& out,
                      Convert convert) {
    PyObject* seq = PySequence_Fast(obj, msg);
    if (!seq) return false;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    out.reserve(out.size() + static_cast<std::size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        if (!convert(PySequence_Fast_GET_ITEM(seq, i), out)) {
            Py_DECREF(seq);
            return false;
        }
    }
    Py_DECREF(seq);
    return true;
}

// Append the UTF-8 strings of a Python sequence to `out`. A non-str item sets
// the TypeError raised by PyUnicode_AsUTF8. `msg` is the not-a-sequence error.
inline bool parse_string_seq(PyObject* obj, const char* msg,
                             std::vector<std::string>& out) {
    return parse_seq<std::string>(
        obj, msg, out, [](PyObject* item, std::vector<std::string>& o) {
            const char* s = PyUnicode_AsUTF8(item);
            if (!s) return false;
            o.emplace_back(s);
            return true;
        });
}

// Append the raw bytes of each item of a Python sequence to `out`; each item
// must be a bytes object (PyBytes_AsStringAndSize sets the error otherwise).
inline bool parse_bytes_seq(PyObject* obj, const char* msg,
                            std::vector<std::string>& out) {
    return parse_seq<std::string>(
        obj, msg, out, [](PyObject* item, std::vector<std::string>& o) {
            char* buf = nullptr;
            Py_ssize_t len = 0;
            if (PyBytes_AsStringAndSize(item, &buf, &len) < 0) return false;
            o.emplace_back(buf, static_cast<std::size_t>(len));
            return true;
        });
}

// Append the integers of a Python sequence to `out` via PyLong_AsLongLong.
// A non-int item (or overflow) sets the error PyLong_AsLongLong raises.
inline bool parse_int_seq(PyObject* obj, const char* msg,
                          std::vector<std::int64_t>& out) {
    return parse_seq<std::int64_t>(
        obj, msg, out, [](PyObject* item, std::vector<std::int64_t>& o) {
            long long v = PyLong_AsLongLong(item);
            if (v == -1 && PyErr_Occurred()) return false;
            o.push_back(static_cast<std::int64_t>(v));
            return true;
        });
}

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_PY_SEQ_HELPERS_H
