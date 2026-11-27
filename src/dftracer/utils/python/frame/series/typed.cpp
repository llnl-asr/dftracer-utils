// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/errors.h>
#include <dftracer/utils/dataframe/internal/frame_native.h>
#include <dftracer/utils/dataframe/internal/reshape.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/python/core/errors.h>
#include <dftracer/utils/python/frame/series_detail.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
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

// _dictionary_parts() -> (codes, values) of a DICTIONARY column, both sharing
// its buffers.
PyObject* Series_dictionary_parts(PyObject* self, PyObject*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    if (a->encoding() != dataframe::Encoding::Dictionary) {
        PyErr_SetString(PyExc_TypeError,
                        "_dictionary_parts(): not a dictionary-encoded column");
        return nullptr;
    }
    Series codes{dftu_series_dictionary_codes(a->handle())};
    Series values{dftu_series_dictionary_values(a->handle())};
    if (!codes.valid() || !values.valid()) {
        PyErr_SetString(PyExc_ValueError,
                        "_dictionary_parts(): the column has no codes or "
                        "no values");
        return nullptr;
    }
    PyObject* codes_obj = make_series(std::move(codes));
    if (!codes_obj) return nullptr;
    PyObject* values_obj = make_series(std::move(values));
    if (!values_obj) {
        Py_DECREF(codes_obj);
        return nullptr;
    }
    PyObject* pair = PyTuple_Pack(2, codes_obj, values_obj);
    Py_DECREF(codes_obj);
    Py_DECREF(values_obj);
    return pair;
}

// _series_from_dictionary(codes, values): see dataframe::dictionary_of.
PyObject* series_from_dictionary(PyObject*, PyObject* args) {
    PyObject* codes_obj = nullptr;
    PyObject* values_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &codes_obj, &values_obj)) return nullptr;
    Series* codes = as_native_or_null<Series>(codes_obj);
    Series* values = as_native_or_null<Series>(values_obj);
    if (!codes || !values) {
        PyErr_SetString(PyExc_TypeError,
                        "_series_from_dictionary: expected two Series");
        return nullptr;
    }
    if (!codes->is_flat() || codes->type() != TypeId::Int32) {
        PyErr_SetString(PyExc_TypeError,
                        "_series_from_dictionary: codes must be a flat Int32 "
                        "Series");
        return nullptr;
    }
    auto out = dataframe::dictionary_of(*codes, *values);
    if (!out) {
        core::set_py_error(out.error());
        return nullptr;
    }
    return make_series(std::move(*out));
}

namespace {

// Whether `c` or a column under it (a chunk, a dictionary or selection base,
// a list's values, a struct field) holds view strings.
bool has_view_text(const dftu_series& c) {
    if (c.encoding == dataframe::Encoding::View) return true;
    return std::any_of(c.nested.begin(), c.nested.end(),
                       [](const dftu_nested& n) {
                           return n.series && has_view_text(*n.series);
                       });
}

// `s` with no view strings anywhere in it. A view is made flat (a copy of its
// text); the rest is shared.
dstd::Result<Series> plain_text(const Series& s) {
    const dftu_series* h = s.handle();
    if (!has_view_text(*h)) return s.share();
    if (h->is_chunked()) {
        dstd::Vector<dstd::SharedPtr<dftu_series>> chunks;
        chunks.reserve(h->nested.size());
        for (const dftu_nested& c : h->nested) {
            Series one{new dftu_series(*c.series)};
            auto flat = plain_text(one);
            if (!flat) return flat;
            chunks.push_back(adopt_series(flat->release()));
        }
        Series out{dataframe::make_chunked(std::move(chunks))};
        out.handle()->name = h->name;
        out.handle()->labels = h->labels;
        return out;
    }
    if (h->encoding == dataframe::Encoding::Dictionary && h->child() &&
        h->child()->encoding == dataframe::Encoding::View) {
        Series codes{dftu_series_dictionary_codes(h)};
        Series values{dftu_series_dictionary_values(h)};
        auto out = dataframe::dictionary_of(codes, values);
        if (out) {
            out->handle()->name = h->name;
            out->handle()->labels = h->labels;
        }
        return out;
    }
    if (h->encoding != dataframe::Encoding::Flat) {
        // A view, or a selection or dictionary over views or over nested
        // columns that hold them: gathered flat, then cleaned below.
        Series flat = s.materialize();
        if (!flat.valid())
            return dataframe::invalid_argument<
                "a view column cannot be made flat">();
        if (!has_view_text(*flat.handle())) return flat;
        return plain_text(flat);
    }
    // A FLAT nested column: the same buffers over children with no views.
    auto* out = new dftu_series(*h);
    Series owned{out};
    for (dftu_nested& n : out->nested) {
        auto clean = plain_text(Series{new dftu_series(*n.series)});
        if (!clean) return clean;
        n.series = adopt_series(clean->release());
    }
    return owned;
}

}  // namespace

// _plain_text() -> Series: see plain_text.
PyObject* Series_plain_text(PyObject* self, PyObject*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    auto out = plain_text(*a);
    if (!out) {
        core::set_py_error(out.error());
        return nullptr;
    }
    return make_series(std::move(*out));
}

// _joined_dictionary() -> Series or None: a CHUNKED column whose chunks are all
// dictionaries joined into one column (its codes copied), a dictionary where
// the values allow one, so pandas sees one category; None for any other column.
PyObject* Series_joined_dictionary(PyObject* self, PyObject*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    const dftu_series* h = a->handle();
    const bool dictionaries =
        h->is_chunked() && !h->nested.empty() &&
        std::all_of(
            h->nested.begin(), h->nested.end(), [](const dftu_nested& c) {
                return c.series->encoding == dataframe::Encoding::Dictionary;
            });
    if (!dictionaries) Py_RETURN_NONE;
    auto out = dataframe::join_chunks(*a);
    if (!out) {
        core::set_py_error(out.error());
        return nullptr;
    }
    return make_series(std::move(*out));
}

// _nulls_like(n) -> Series: n null rows of this column's full type, nested
// types included (see dataframe::null_column_like).
PyObject* Series_nulls_like(PyObject* self, PyObject* arg) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    const long long n = PyLong_AsLongLong(arg);
    if (n == -1 && PyErr_Occurred()) return nullptr;
    if (n < 0) {
        PyErr_SetString(PyExc_ValueError, "_nulls_like: n must be >= 0");
        return nullptr;
    }
    Series out = dataframe::null_column_like(*a, n);
    if (!out.valid()) {
        PyErr_Format(PyExc_TypeError,
                     "_nulls_like: a column of type '%s' has no null form",
                     dataframe::type_name(a->type()));
        return nullptr;
    }
    return make_series(std::move(out));
}

namespace {

// The null mask of `c` (a byte per row, 1 where null) into `dst`, chunk by
// chunk, with no copy of the column. A dictionary row is null where its code
// is null or the value it points at is.
void fill_null_mask(const dftu_series& c, std::uint8_t* dst) {
    const std::int64_t n = c.length;
    if (c.is_chunked()) {
        const auto* starts =
            reinterpret_cast<const std::int64_t*>(c.data.data());
        for (std::size_t k = 0; k < c.nested.size(); ++k)
            fill_null_mask(*c.nested[k].series, dst + starts[k]);
        return;
    }
    if (c.encoding == dataframe::Encoding::Flat ||
        c.encoding == dataframe::Encoding::View ||
        c.encoding == dataframe::Encoding::Dictionary) {
        if (c.validity.block() && c.null_count > 0) {
            expand_null_bytes(c.validity.data(), n, dst);
        } else {
            std::memset(dst, 0, static_cast<std::size_t>(n));
        }
        if (c.encoding != dataframe::Encoding::Dictionary || !c.rowwise_nulls())
            return;
        const auto* codes =
            reinterpret_cast<const std::int32_t*>(c.data.data());
        for (std::int64_t i = 0; i < n; ++i)
            if (!dst[i] && dftu_series_is_null(c.child().get(), codes[i]))
                dst[i] = 1;
        return;
    }
    for (std::int64_t i = 0; i < n; ++i)
        dst[i] = dftu_series_is_null(&c, i) ? 1 : 0;
}

}  // namespace

// _null_bytes() -> None without nulls, else a bytearray of a byte per row, 1
// where the row is null. One pass over the validity, no copy of the column.
PyObject* Series_null_bytes(PyObject* self, PyObject*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    const dftu_series* h = a->handle();
    const bool entry_nulls =
        h->encoding == dataframe::Encoding::Dictionary && h->rowwise_nulls();
    if (!entry_nulls && dftu_series_null_count(h) == 0 && !h->is_chunked())
        Py_RETURN_NONE;
    PyObject* out = PyByteArray_FromStringAndSize(
        nullptr, static_cast<Py_ssize_t>(h->length));
    if (!out) return nullptr;
    auto* dst = reinterpret_cast<std::uint8_t*>(PyByteArray_AS_STRING(out));
    fill_null_mask(*h, dst);
    if ((h->is_chunked() || entry_nulls) &&
        std::find(dst, dst + h->length, std::uint8_t{1}) == dst + h->length) {
        Py_DECREF(out);
        Py_RETURN_NONE;
    }
    return out;
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

// _number_rows(keys, sorted) -> (ids, first): see dataframe::number_rows.
PyObject* number_rows(PyObject*, PyObject* args) {
    PyObject* keys_obj = nullptr;
    int sorted = 0;
    if (!PyArg_ParseTuple(args, "Op", &keys_obj, &sorted)) return nullptr;
    PyObject* seq =
        PySequence_Fast(keys_obj, "_number_rows: keys must be a list");
    if (!seq) return nullptr;
    dftracer::utils::dstd::Vector<Series> keys;
    for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(seq); ++i) {
        Series* k = as_native_or_null<Series>(PySequence_Fast_GET_ITEM(seq, i));
        if (!k) {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_TypeError, "_number_rows: expected Series");
            return nullptr;
        }
        keys.push_back(k->share());
    }
    Py_DECREF(seq);
    auto out = without_gil(keys.empty() ? 0 : keys.front().length(), [&] {
        return dataframe::number_rows(keys, sorted != 0);
    });
    if (!out) {
        core::set_py_error(out.error());
        return nullptr;
    }
    PyObject* ids = make_series(std::move(out->ids));
    PyObject* first = make_series(std::move(out->first));
    if (!ids || !first) {
        Py_XDECREF(ids);
        Py_XDECREF(first);
        return nullptr;
    }
    PyObject* pair = PyTuple_Pack(2, ids, first);
    Py_DECREF(ids);
    Py_DECREF(first);
    return pair;
}

// _unstack_positions(rows, cols, row_count, col_count) -> Series: see
// dataframe::unstack_positions.
PyObject* unstack_positions(PyObject*, PyObject* args) {
    PyObject* rows_obj = nullptr;
    PyObject* cols_obj = nullptr;
    long long row_count = 0;
    long long col_count = 0;
    if (!PyArg_ParseTuple(args, "OOLL", &rows_obj, &cols_obj, &row_count,
                          &col_count))
        return nullptr;
    Series* rows = as_native_or_null<Series>(rows_obj);
    Series* cols = as_native_or_null<Series>(cols_obj);
    if (!rows || !cols) {
        PyErr_SetString(PyExc_TypeError, "_unstack_positions: expected Series");
        return nullptr;
    }
    const Series r = rows->share();
    const Series c = cols->share();
    auto out = without_gil(r.length(), [&] {
        return dataframe::unstack_positions(r, c, row_count, col_count);
    });
    if (!out) {
        core::set_py_error(out.error());
        return nullptr;
    }
    return make_series(std::move(*out));
}

// _unstack_columns(values, positions, row_count) -> list of Series: see
// dataframe::unstack_columns.
PyObject* unstack_columns(PyObject*, PyObject* args) {
    PyObject* values_obj = nullptr;
    PyObject* positions_obj = nullptr;
    long long row_count = 0;
    if (!PyArg_ParseTuple(args, "OOL", &values_obj, &positions_obj, &row_count))
        return nullptr;
    Series* values = as_native_or_null<Series>(values_obj);
    Series* positions = as_native_or_null<Series>(positions_obj);
    if (!values || !positions) {
        PyErr_SetString(PyExc_TypeError, "_unstack_columns: expected Series");
        return nullptr;
    }
    const Series v = values->share();
    const Series p = positions->share();
    auto out = without_gil(p.length(), [&] {
        return dataframe::unstack_columns(v, p, row_count);
    });
    if (!out) {
        core::set_py_error(out.error());
        return nullptr;
    }
    PyObject* list = PyList_New(static_cast<Py_ssize_t>(out->size()));
    if (!list) return nullptr;
    for (std::size_t i = 0; i < out->size(); ++i) {
        PyObject* item = make_series(std::move((*out)[i]));
        if (!item) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), item);
    }
    return list;
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
