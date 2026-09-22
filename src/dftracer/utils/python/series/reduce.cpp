#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/python/series_detail.h>

#include <cstdint>

namespace dftracer::utils::python::series_detail {

// A tagged scalar as the matching Python number.
PyObject* scalar_to_py(dftu_scalar v) {
    using dataframe::ScalarTag;
    if (v.kind < static_cast<std::int32_t>(ScalarTag::I64) ||
        v.kind > static_cast<std::int32_t>(ScalarTag::F64)) {
        PyErr_SetString(PyExc_RuntimeError, "unknown scalar tag");
        return nullptr;
    }
    switch (static_cast<ScalarTag>(v.kind)) {
        case ScalarTag::I64:
            return PyLong_FromLongLong(v.value.i);
        case ScalarTag::U64:
            return PyLong_FromUnsignedLongLong(v.value.u);
        case ScalarTag::F64:
            return PyFloat_FromDouble(v.value.d);
    }
    PyErr_SetString(PyExc_RuntimeError, "unknown scalar tag");
    return nullptr;
}

PyObject* Series_all(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyBool_FromLong(a->all());
}
PyObject* Series_any(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyBool_FromLong(a->any());
}
PyObject* Series_dot(PyObject* self, PyObject* other) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    Series* b = as_series(other);
    if (!b) return nullptr;
    return scalar_to_py(dftu_series_dot(a->handle(), b->handle()));
}

}  // namespace dftracer::utils::python::series_detail

#endif  // DFTRACER_UTILS_ENABLE_ARROW
