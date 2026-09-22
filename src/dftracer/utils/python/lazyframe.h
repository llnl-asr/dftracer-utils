#ifndef DFTRACER_UTILS_PYTHON_LAZYFRAME_H
#define DFTRACER_UTILS_PYTHON_LAZYFRAME_H

#include <Python.h>

namespace dftracer::utils::dataframe {
class LazyFrame;
}

namespace dftracer::utils::python {

// Register the LazyFrame type on module `m`. Returns 0 on success, -1 with the
// Python error set on failure.
int init_lazyframe(PyObject* m);

// Wrap an owned dataframe::LazyFrame in a new LazyFrame Python object. Requires
// the arrow-enabled build; returns nullptr with a Python error otherwise. Used
// by DataFrame.lazy() to hand a deferred query to Python.
PyObject* wrap_lazyframe(dftracer::utils::dataframe::LazyFrame&& lf);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_LAZYFRAME_H
