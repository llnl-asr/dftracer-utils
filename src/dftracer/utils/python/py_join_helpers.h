#ifndef DFTRACER_UTILS_PYTHON_PY_JOIN_HELPERS_H
#define DFTRACER_UTILS_PYTHON_PY_JOIN_HELPERS_H

#include <Python.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/python/py_seq_helpers.h>

#include <cstring>
#include <string>
#include <vector>

namespace dftracer::utils::python {

/// Parse a join `how` name. "outer" (pandas) and "full" (polars) both mean
/// JoinHow::Outer. Returns false with a Python ValueError set on any other
/// name.
inline bool join_how_from_str(const char* how, dataframe::JoinHow& out) {
    using dataframe::JoinHow;
    if (std::strcmp(how, "inner") == 0)
        out = JoinHow::Inner;
    else if (std::strcmp(how, "left") == 0)
        out = JoinHow::Left;
    else if (std::strcmp(how, "right") == 0)
        out = JoinHow::Right;
    else if (std::strcmp(how, "outer") == 0 || std::strcmp(how, "full") == 0)
        out = JoinHow::Outer;
    else if (std::strcmp(how, "semi") == 0)
        out = JoinHow::Semi;
    else if (std::strcmp(how, "anti") == 0)
        out = JoinHow::Anti;
    else if (std::strcmp(how, "cross") == 0)
        out = JoinHow::Cross;
    else {
        PyErr_Format(PyExc_ValueError,
                     "join() how must be "
                     "inner|left|right|outer|full|semi|anti|cross, got '%s'",
                     how);
        return false;
    }
    return true;
}

/// The join(on=, left_on=, right_on=) keys resolved to one pair of lists: `on`
/// (a str or sequence) names both sides, else `left_on` / `right_on` (each a
/// str or sequence) name each side. A cross join takes none. Returns false
/// with a Python error set when the combination is invalid.
inline bool join_keys_from_objs(PyObject* on, PyObject* left_on,
                                PyObject* right_on, dataframe::JoinHow how,
                                std::vector<std::string>& l,
                                std::vector<std::string>& r) {
    auto is_set = [](PyObject* o) { return o && o != Py_None; };
    if (how == dataframe::JoinHow::Cross) {
        if (is_set(on) || is_set(left_on) || is_set(right_on)) {
            PyErr_SetString(PyExc_ValueError,
                            "join(): a cross join takes no key columns");
            return false;
        }
        return true;
    }
    if (is_set(on)) {
        if (is_set(left_on) || is_set(right_on)) {
            PyErr_SetString(PyExc_ValueError,
                            "join(): pass either on= or left_on=/right_on=, "
                            "not both");
            return false;
        }
        if (PyUnicode_Check(on)) {
            const char* s = PyUnicode_AsUTF8(on);
            if (!s) return false;
            l.emplace_back(s);
        } else if (!parse_string_seq(
                       on, "on must be a str or a sequence of str", l)) {
            return false;
        }
        r = l;
    } else {
        if (!is_set(left_on) || !is_set(right_on)) {
            PyErr_SetString(PyExc_ValueError,
                            "join(): pass on= or both left_on= and right_on=");
            return false;
        }
        if (PyUnicode_Check(left_on)) {
            const char* s = PyUnicode_AsUTF8(left_on);
            if (!s) return false;
            l.emplace_back(s);
        } else if (!parse_string_seq(
                       left_on, "left_on must be a str or a sequence of str",
                       l)) {
            return false;
        }
        if (PyUnicode_Check(right_on)) {
            const char* s = PyUnicode_AsUTF8(right_on);
            if (!s) return false;
            r.emplace_back(s);
        } else if (!parse_string_seq(
                       right_on, "right_on must be a str or a sequence of str",
                       r)) {
            return false;
        }
    }
    if (l.empty()) {
        PyErr_SetString(PyExc_ValueError,
                        "join(): 'on' must name at least one key column");
        return false;
    }
    if (l.size() != r.size()) {
        PyErr_SetString(PyExc_ValueError,
                        "join(): left_on and right_on differ in length");
        return false;
    }
    return true;
}

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_PY_JOIN_HELPERS_H
