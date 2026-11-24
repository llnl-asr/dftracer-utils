// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_CORE_SEQ_HELPERS_H
#define DFTRACER_UTILS_PYTHON_CORE_SEQ_HELPERS_H

#include <Python.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::python::core {

/// Holds the PySequence_Fast result of an iterable while its items are used.
/// For a list or tuple that is the object itself; for a generator or a deque it
/// is a temporary list, and only this handle keeps the items alive. Keep it for
/// as long as any borrowed item pointer, or a pointer into an item, is in use.
class FastSeq {
   public:
    FastSeq() = default;
    FastSeq(const FastSeq&) = delete;
    FastSeq& operator=(const FastSeq&) = delete;
    ~FastSeq() { Py_XDECREF(seq_); }

    /// Opens @p obj; false with a TypeError of text @p msg when it is not a
    /// sequence or iterable.
    bool open(PyObject* obj, const char* msg) {
        Py_XDECREF(seq_);
        seq_ = PySequence_Fast(obj, msg);
        return seq_ != nullptr;
    }
    Py_ssize_t size() const { return PySequence_Fast_GET_SIZE(seq_); }
    /// Borrowed; valid while this handle lives.
    PyObject* item(Py_ssize_t i) const {
        return PySequence_Fast_GET_ITEM(seq_, i);
    }

   private:
    PyObject* seq_ = nullptr;
};

// Convert each item of a Python sequence via `convert`, appending to `out`.
// `msg` is the TypeError text set when `obj` is not a sequence. Owns the
// PySequence_Fast handle and releases it on every path, including errors.
// The handle is released when this returns, so `convert` must copy what it
// keeps: for a generator the items die with it. A caller that keeps borrowed
// pointers to the items uses @ref FastSeq instead.
// `convert(item, out)` returns false with a Python error already set to abort;
// `out` may be partially filled in that case.
template <class T, class Convert>
inline bool parse_seq(PyObject* obj, const char* msg,
                      dftracer::utils::dstd::Vector<T>& out, Convert convert) {
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
// the TypeError raised by PyUnicode_AsUTF8AndSize. `msg` is the not-a-sequence
// error.
inline bool parse_string_seq(
    PyObject* obj, const char* msg,
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& out) {
    PyObject* seq = PySequence_Fast(obj, msg);
    if (!seq) return false;
    const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    out.reserve(out.size() + static_cast<std::size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        Py_ssize_t len = 0;
        const char* s =
            PyUnicode_AsUTF8AndSize(PySequence_Fast_GET_ITEM(seq, i), &len);
        if (!s) {
            Py_DECREF(seq);
            return false;
        }
        out.emplace_back(s, static_cast<std::size_t>(len));
    }
    Py_DECREF(seq);
    return true;
}

// Append one path of the `files` argument to `paths` unless it is there
// already; `known` holds the paths added so far. `item` is a str or an
// os.PathLike that gives a str; bytes and any other type set a TypeError.
inline bool add_trace_path(
    PyObject* item,
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& paths,
    dftracer::utils::dstd::StringViewSet& known) {
    if (PyBytes_Check(item)) {
        PyErr_SetString(PyExc_TypeError,
                        "a path must be a str or an os.PathLike of str");
        return false;
    }
    PyObject* fs = PyOS_FSPath(item);
    if (!fs) return false;
    if (!PyUnicode_Check(fs)) {
        Py_DECREF(fs);
        PyErr_SetString(PyExc_TypeError,
                        "a path must be a str or an os.PathLike of str");
        return false;
    }
    Py_ssize_t len = 0;
    const char* s = PyUnicode_AsUTF8AndSize(fs, &len);
    if (!s) {
        Py_DECREF(fs);
        return false;
    }
    const dftracer::utils::dstd::StringView path(s,
                                                 static_cast<std::size_t>(len));
    if (known.find(path) == known.end()) {
        paths.emplace_back(path);
        known.emplace(path);
    }
    Py_DECREF(fs);
    return true;
}

// Read the `files` argument of TraceViewer and Indexer: one path or a sequence
// of paths, each a str or an os.PathLike. A path is a file, a directory or a
// glob; the C++ input expansion tells them apart. A sequence fills `paths` in
// order and keeps the first of equal paths. Any other type sets a TypeError.
inline bool parse_trace_files(
    PyObject* files,
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& paths) {
    dftracer::utils::dstd::StringViewSet known;
    if (PyUnicode_Check(files) || PyObject_HasAttrString(files, "__fspath__"))
        return add_trace_path(files, paths, known);
    // A tuple copy: a user __fspath__ may change the caller's list.
    if (!PySequence_Check(files)) {
        PyErr_SetString(PyExc_TypeError,
                        "files must be a path or a sequence of paths");
        return false;
    }
    PyObject* seq = PySequence_Tuple(files);
    if (!seq) return false;
    const Py_ssize_t n = PyTuple_GET_SIZE(seq);
    for (Py_ssize_t i = 0; i < n; ++i) {
        if (!add_trace_path(PyTuple_GET_ITEM(seq, i), paths, known)) {
            Py_DECREF(seq);
            return false;
        }
    }
    Py_DECREF(seq);
    return true;
}

// Append the raw bytes of each item of a Python sequence to `out`; each item
// must be a bytes object (PyBytes_AsStringAndSize sets the error otherwise).
inline bool parse_bytes_seq(
    PyObject* obj, const char* msg,
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& out) {
    return parse_seq<dftracer::utils::dstd::String>(
        obj, msg, out,
        [](PyObject* item,
           dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& o) {
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
                          dftracer::utils::dstd::Vector<std::int64_t>& out) {
    return parse_seq<std::int64_t>(
        obj, msg, out,
        [](PyObject* item, dftracer::utils::dstd::Vector<std::int64_t>& o) {
            long long v = PyLong_AsLongLong(item);
            if (v == -1 && PyErr_Occurred()) return false;
            o.push_back(static_cast<std::int64_t>(v));
            return true;
        });
}

}  // namespace dftracer::utils::python::core

#endif  // DFTRACER_UTILS_PYTHON_CORE_SEQ_HELPERS_H
