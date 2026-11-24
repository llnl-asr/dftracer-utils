// SPDX-License-Identifier: MIT
#include <dftracer/utils/python/formats/bind.h>
#include <dftracer/utils/python/formats/format.h>
#include <dftracer/utils/python/formats/mutable_value.h>
#include <dftracer/utils/python/formats/records.h>
#include <dftracer/utils/python/formats/value.h>
#include <dftracer/utils/python/formats/writer.h>

namespace dftracer::utils::python::formats {

namespace {

template <class F>
int init_format(PyObject *m) {
    if (init_value<F>(m) < 0) return -1;
    if (init_mutable_value<F>(m) < 0) return -1;
    if (init_records<F>(m) < 0) return -1;
    return init_writer<F>(m);
}

}  // namespace

int init_formats(PyObject *m) {
    if (init_format<JsonFormat>(m) < 0) return -1;
    return init_format<YamlFormat>(m);
}

}  // namespace dftracer::utils::python::formats
