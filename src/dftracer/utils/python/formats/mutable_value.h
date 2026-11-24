// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_FORMATS_MUTABLE_VALUE_H
#define DFTRACER_UTILS_PYTHON_FORMATS_MUTABLE_VALUE_H

#include <dftracer/utils/python/formats/emit.h>
#include <dftracer/utils/python/formats/format.h>
#include <dftracer/utils/python/formats/value.h>

namespace dftracer::utils::python::formats {

/// A MutableValue of format F: the changes made to a Value, kept apart from it.
/// `entries` is a dict (an object) or a list (an array) that holds, for each
/// member or element, the Value it still is, a MutableValue that was read or
/// changed below it, or the Python value that was set. It is made on the first
/// read of a child or the first change, and the base Value never changes.
template <class F>
struct MutableObject {
    enum Mode : int { Scalar, Object, Array };

    PyObject_HEAD int mode;
    PyObject *base;     // the Value this overlays, or null
    PyObject *entries;  // dict or list, or null while nothing was read
    PyObject *scalar;   // a Python scalar that was set, or null
    bool dirty;         // set, deleted or appended here, or made with no base
};

template <class F>
PyTypeObject *mutable_type();

/// A new MutableValue over `value`, a Value of format F.
template <class F>
PyObject *make_mutable(PyObject *value);

/// Writes a MutableValue of format S to `e`: the unchanged parts as the Value
/// writes them and the changed parts as `emit_py` writes them.
template <class S, class D>
bool emit_mutable(Emitter<D> &e, PyObject *mutable_value, int depth);

/// The source text of the Value under a MutableValue of format S when nothing
/// was changed in it or below it, else null.
template <class S>
const RawSpan *unchanged_raw(PyObject *mutable_value);

/// A new Python object of the plain data under any value that `emit_py`
/// writes. Null with the Python error set.
PyObject *data_of(PyObject *o, int depth);

template <class F>
int init_mutable_value(PyObject *m);

}  // namespace dftracer::utils::python::formats

#endif  // DFTRACER_UTILS_PYTHON_FORMATS_MUTABLE_VALUE_H
