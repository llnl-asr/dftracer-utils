// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/frame_native.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/python/frame/series_detail.h>

#include <cstdint>
#include <exception>
#include <limits>
#include <utility>

namespace dftracer::utils::python::frame::series_detail {

namespace {

std::size_t buffer_size(const dataframe::BufferView& b) {
    return b.block() ? b.size() : 0;
}

std::size_t nbytes_of(const dftu_series& c) {
    if (c.is_chunked())
        return static_cast<std::size_t>(dftu_series_buffer_bytes(&c));
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
                     dftracer::utils::dstd::StringView zone, int precision,
                     int scale, int width) {
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
    Series* a = as_native<Series>(self);
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
    Series* a = as_native_or_null<Series>(obj);
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

// _series_list_of(values, ends): a List column whose row i holds
// values[ends[i - 1]:ends[i]], the first row starting at 0.
PyObject* series_list_of(PyObject*, PyObject* args) {
    PyObject* values_obj = nullptr;
    PyObject* offsets_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &values_obj, &offsets_obj))
        return nullptr;
    Series* values = as_native_or_null<Series>(values_obj);
    Series* ends = as_native_or_null<Series>(offsets_obj);
    if (!values || !ends) {
        PyErr_SetString(PyExc_TypeError, "list_of: expected two Series");
        return nullptr;
    }
    const Series flat_ends = ends->materialize();
    if (flat_ends.type() != TypeId::Int64 || flat_ends.null_count() != 0) {
        PyErr_SetString(PyExc_ValueError,
                        "list_of: ends must be an Int64 column without nulls");
        return nullptr;
    }
    const std::int64_t n = flat_ends.length();
    const auto* src = flat_ends.data<std::int64_t>();
    dftracer::utils::dstd::Vector<std::int32_t> narrow(
        static_cast<std::size_t>(n) + 1, 0);
    for (std::int64_t i = 0; i < n; ++i) {
        const std::int64_t at = src[i];
        if (at < 0 || at > values->length() || (i > 0 && at < src[i - 1]) ||
            at > std::numeric_limits<std::int32_t>::max()) {
            PyErr_SetString(PyExc_ValueError,
                            "list_of: ends must not decrease and must stay "
                            "within the values");
            return nullptr;
        }
        narrow[static_cast<std::size_t>(i) + 1] = static_cast<std::int32_t>(at);
    }
    return make_series(Series::list(narrow, values->materialize()));
}

PyObject* Series_get_nbytes(PyObject* self, void*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    return PyLong_FromSize_t(nbytes_of(*a->handle()));
}

PyObject* Series_to_bytes(PyObject* self, PyObject*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    return guard_as(PyExc_TypeError, [&]() -> PyObject* {
        dataframe::DataFrame f;
        f.names.emplace_back("v");
        f.columns.push_back(a->share());
        const auto bytes = without_gil(
            a->length(), [&] { return dataframe::frame_to_native(f); });
        if (!bytes) {
            PyErr_SetString(PyExc_TypeError, bytes.error().c_str());
            return nullptr;
        }
        return PyBytes_FromStringAndSize(
            bytes->data(), static_cast<Py_ssize_t>(bytes->size()));
    });
}

// _tz_set_search_path(paths): the directories searched first for time zone
// files, separated by ':'.
PyObject* tz_set_search_path(PyObject*, PyObject* arg) {
    Py_ssize_t size = 0;
    const char* paths = PyUnicode_AsUTF8AndSize(arg, &size);
    if (!paths) return nullptr;
    dftu_tz_set_search_path(paths, static_cast<std::int32_t>(size));
    Py_RETURN_NONE;
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
        dftracer::utils::dstd::StringView(static_cast<const char*>(view.buf),
                                          static_cast<std::size_t>(view.len)));
    PyBuffer_Release(&view);
    if (!f || f->columns.size() != 1) {
        PyErr_SetString(PyExc_ValueError,
                        "not a Series written by this version");
        return nullptr;
    }
    return make_series(std::move(f->columns.front()));
}

}  // namespace dftracer::utils::python::frame::series_detail
