// SPDX-License-Identifier: MIT
#include <dftracer/utils/arrow/stream.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/python/core/arrow_helpers.h>
#include <dftracer/utils/python/frame/series_detail.h>

namespace dftracer::utils::python::frame::series_detail {

PyObject* Series_arrow_c_stream(PyObject* self, PyObject* args) {
    PyObject* requested_schema = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &requested_schema)) return nullptr;
    if (!core::no_requested_schema(requested_schema)) return nullptr;
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    auto stream = dftracer::utils::dstd::make_unique<ArrowArrayStream>();
    auto status = arrow::to_arrow_stream(*a, stream.get());
    if (!status) {
        core::set_py_error(status.error());
        return nullptr;
    }
    return core::stream_capsule(std::move(stream));
}

}  // namespace dftracer::utils::python::frame::series_detail
