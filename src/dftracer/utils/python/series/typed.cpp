#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/frame_native.h>
#include <dftracer/utils/python/series_detail.h>

#include <cstdint>
#include <exception>
#include <string>
#include <utility>

namespace dftracer::utils::python::series_detail {

namespace {

std::size_t buffer_size(const std::shared_ptr<dataframe::Buffer>& b) {
    return b ? b->size() : 0;
}

std::size_t nbytes_of(const dftu_series& c) {
    std::size_t total =
        buffer_size(c.data) + buffer_size(c.offsets) + buffer_size(c.validity);
    for (const auto& k : c.nested) total += nbytes_of(*k.series);
    return total;
}

Series flat_copy(const Series& s) {
    return s.encoding() == dataframe::Encoding::Flat ? s.share()
                                                     : s.materialize();
}

}  // namespace

Series retype_series(const Series& s, TypeId type, dataframe::TimeUnit unit,
                     const std::string& zone, int precision, int scale,
                     int width) {
    Series flat = flat_copy(s);
    const auto from_width = dataframe::byte_width(flat.type());
    const auto to_width = dataframe::byte_width(type, width);
    if (!from_width || !to_width || *from_width != *to_width) return Series{};
    auto* h = new dftu_series(*flat.handle());
    h->type = type;
    h->set_time_unit(unit);
    h->set_timezone(zone);
    h->set_decimal(precision, scale);
    h->set_fixed_size(width);
    h->set_json(false);
    return Series{h};
}

// The physical integers of a date, time, timestamp, duration or float16
// column, sharing its buffers and nulls: int32 for date32 and time32, int64
// for date64, time64, timestamp and duration, uint16 for float16.
PyObject* Series_physical(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    Series flat = flat_copy(*a);
    TypeId to = TypeId::Int64;
    switch (flat.type()) {
        case TypeId::Date32:
        case TypeId::Time32:
            to = TypeId::Int32;
            break;
        case TypeId::Date64:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
            to = TypeId::Int64;
            break;
        case TypeId::Float16:
            to = TypeId::Uint16;
            break;
        default:
            PyErr_SetString(PyExc_TypeError,
                            "physical(): not a date, time, timestamp, "
                            "duration or float16 column");
            return nullptr;
    }
    auto* h = new dftu_series(*flat.handle());
    h->type = to;
    h->set_time_unit(dataframe::TimeUnit::Micro);
    h->set_timezone({});
    return make_series(Series{h});
}

// _series_retype(series, type_id, unit, timezone, precision, scale, width)
PyObject* series_retype(PyObject*, PyObject* args) {
    PyObject* obj = nullptr;
    int type = 0, unit = 0, precision = 0, scale = 0, width = 0;
    const char* zone = "";
    if (!PyArg_ParseTuple(args, "Oi|isiii", &obj, &type, &unit, &zone,
                          &precision, &scale, &width))
        return nullptr;
    Series* a = as_series_or_null(obj);
    if (!a) {
        PyErr_SetString(PyExc_TypeError, "retype: expected a Series");
        return nullptr;
    }
    if (unit < 0 || unit > 3) {
        PyErr_SetString(PyExc_ValueError, "retype: bad time unit");
        return nullptr;
    }
    Series out = retype_series(*a, static_cast<TypeId>(type),
                               static_cast<dataframe::TimeUnit>(unit), zone,
                               precision, scale, width);
    if (!out.valid()) {
        PyErr_SetString(PyExc_TypeError,
                        "retype: the target type does not have the width of "
                        "the column");
        return nullptr;
    }
    return make_series(std::move(out));
}

PyObject* Series_get_nbytes(PyObject* self, void*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyLong_FromSize_t(nbytes_of(*a->handle()));
}

PyObject* Series_to_bytes(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    try {
        dataframe::DataFrame f;
        f.names.emplace_back("v");
        f.columns.push_back(a->share());
        const std::string bytes = dataframe::frame_to_native(f);
        return PyBytes_FromStringAndSize(bytes.data(),
                                         static_cast<Py_ssize_t>(bytes.size()));
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_TypeError, e.what());
        return nullptr;
    }
}

// _series_nulls(type_id, n): a column of `n` nulls of the given type.
PyObject* series_nulls(PyObject*, PyObject* args) {
    int type_id = 0;
    Py_ssize_t n = 0;
    if (!PyArg_ParseTuple(args, "in", &type_id, &n)) return nullptr;
    if (n < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "_series_nulls: the length must not be negative");
        return nullptr;
    }
    return make_series(Series::nulls(static_cast<TypeId>(type_id),
                                     static_cast<std::int64_t>(n)));
}

PyObject* series_from_bytes(PyObject*, PyObject* obj) {
    Py_buffer view;
    if (PyObject_GetBuffer(obj, &view, PyBUF_SIMPLE) < 0) return nullptr;
    auto f = dataframe::frame_from_native(
        std::string_view(static_cast<const char*>(view.buf),
                         static_cast<std::size_t>(view.len)));
    PyBuffer_Release(&view);
    if (!f || f->columns.size() != 1) {
        PyErr_SetString(PyExc_ValueError,
                        "not a Series written by this version");
        return nullptr;
    }
    return make_series(std::move(f->columns.front()));
}

}  // namespace dftracer::utils::python::series_detail
