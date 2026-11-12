// SPDX-License-Identifier: MIT
#include <dftracer/utils/python/py_scalar_helpers.h>
#include <dftracer/utils/python/series_detail.h>

namespace dftracer::utils::python::series_detail {

PyObject* Series_is_between(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    PyObject* lo = nullptr;
    PyObject* hi = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &lo, &hi)) return nullptr;
    dftu_scalar slo{}, shi{};
    if (!py_to_scalar(lo, &slo) || !py_to_scalar(hi, &shi)) return nullptr;
    return make_series(
        named_like(Series{dftu_series_is_between(a->handle(), slo, shi)}, *a));
}
PyObject* Series_fillna(PyObject* self, PyObject* value) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    dftu_scalar s{};
    if (!py_to_scalar(value, &s)) return nullptr;
    return make_series(
        named_like(Series{dftu_series_fillna(a->handle(), s)}, *a));
}
PyObject* Series_clip(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    PyObject* lo = nullptr;
    PyObject* hi = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &lo, &hi)) return nullptr;
    dftu_scalar slo{}, shi{};
    if (!py_to_scalar(lo, &slo) || !py_to_scalar(hi, &shi)) return nullptr;
    return make_series(
        named_like(Series{dftu_series_clip(a->handle(), slo, shi)}, *a));
}

}  // namespace dftracer::utils::python::series_detail
