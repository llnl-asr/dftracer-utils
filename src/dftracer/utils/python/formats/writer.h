// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_FORMATS_WRITER_H
#define DFTRACER_UTILS_PYTHON_FORMATS_WRITER_H

#include <dftracer/utils/python/formats/format.h>

namespace dftracer::utils::python::formats {

/// Registers the Writer, ObjectBuilder and ArrayBuilder types of format F.
template <class F>
int init_writer(PyObject *m);

}  // namespace dftracer::utils::python::formats

#endif  // DFTRACER_UTILS_PYTHON_FORMATS_WRITER_H
