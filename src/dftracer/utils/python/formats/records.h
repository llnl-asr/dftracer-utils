// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_FORMATS_RECORDS_H
#define DFTRACER_UTILS_PYTHON_FORMATS_RECORDS_H

#include <dftracer/utils/python/formats/format.h>

namespace dftracer::utils::python::formats {

/// Registers the RecordBatch and RecordIterator types of format F and the
/// functions parse, loads, read, load and read_records.
template <class F>
int init_records(PyObject *m);

}  // namespace dftracer::utils::python::formats

#endif  // DFTRACER_UTILS_PYTHON_FORMATS_RECORDS_H
