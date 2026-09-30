// clang-format off
#include <dftracer/utils/python/series_detail.h>  // Python.h first
#include <datetime.h>
// clang-format on
#include <dftracer/utils/core/common/hash/fnv1a.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/float16.h>
#include <dftracer/utils/dataframe/series.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::python::series_detail {

namespace {

// A column resolved once for cell reads: flat, with its nested children.
struct Node {
    Series col;
    TypeId type = TypeId::Int64;
    std::vector<Node> kids;
    std::vector<std::string> fields;
    dataframe::TimeUnit unit = dataframe::TimeUnit::Micro;
    std::string zone;
    std::int32_t scale = 0;
    std::int32_t width = 0;
    PyObject* tz = nullptr;  // borrowed: cached ZoneInfo of `zone`
};

constexpr std::int64_t US_PER_DAY = 86'400'000'000LL;

bool datetime_ready() {
    if (!PyDateTimeAPI) PyDateTime_IMPORT;
    return PyDateTimeAPI != nullptr;
}

// Days since 1970-01-01 to a proleptic Gregorian date (H. Hinnant,
// "chrono-Compatible Low-Level Date Algorithms", public domain).
void civil_from_days(std::int64_t z, std::int64_t& y, int& m, int& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y += m <= 2;
}

std::int64_t days_from_civil(std::int64_t y, int m, int d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy =
        (153U * static_cast<unsigned>(m > 2 ? m - 3 : m + 9) + 2) / 5 +
        static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

std::int64_t floor_mod(std::int64_t a, std::int64_t b) {
    return a - floor_div(a, b) * b;
}

// `v` in `unit` as microseconds, floored; false when it overflows.
bool to_micros(std::int64_t v, dataframe::TimeUnit unit, std::int64_t& out) {
    switch (unit) {
        case dataframe::TimeUnit::Second:
            return !__builtin_mul_overflow(v, std::int64_t{1'000'000}, &out);
        case dataframe::TimeUnit::Milli:
            return !__builtin_mul_overflow(v, std::int64_t{1'000}, &out);
        case dataframe::TimeUnit::Micro:
            out = v;
            return true;
        case dataframe::TimeUnit::Nano:
            out = floor_div(v, 1000);
            return true;
    }
    return false;
}

PyObject* range_error(const char* what) {
    PyErr_SetString(PyExc_OverflowError, what);
    return nullptr;
}

// The `zoneinfo.ZoneInfo` (or fixed-offset timezone) of `name`, cached.
PyObject* zone_of(const std::string& name) {
    static PyObject* cache = nullptr;
    if (!cache && !(cache = PyDict_New())) return nullptr;
    PyObject* key = PyUnicode_FromString(name.c_str());
    if (!key) return nullptr;
    if (PyObject* hit = PyDict_GetItemWithError(cache, key)) {
        Py_DECREF(key);
        return hit;
    }
    if (PyErr_Occurred()) {
        Py_DECREF(key);
        return nullptr;
    }
    PyObject* zone = nullptr;
    if (!name.empty() && (name[0] == '+' || name[0] == '-')) {
        int hh = 0, mm = 0;
        if (std::sscanf(name.c_str() + 1, "%d:%d", &hh, &mm) < 1) {
            Py_DECREF(key);
            PyErr_Format(PyExc_ValueError, "bad timezone offset '%s'",
                         name.c_str());
            return nullptr;
        }
        const int sign = name[0] == '-' ? -1 : 1;
        PyObject* delta = PyDelta_FromDSU(0, sign * (hh * 3600 + mm * 60), 0);
        if (delta) {
            zone = PyTimeZone_FromOffset(delta);
            Py_DECREF(delta);
        }
    } else if (name == "UTC") {
        zone = PyDateTime_TimeZone_UTC;
        Py_XINCREF(zone);
    } else {
        PyObject* mod = PyImport_ImportModule("zoneinfo");
        if (!mod) {
            PyErr_Clear();
            PyErr_SetString(PyExc_ImportError,
                            "timezone-aware timestamps need zoneinfo "
                            "(Python 3.9 or newer)");
        } else {
            PyObject* cls = PyObject_GetAttrString(mod, "ZoneInfo");
            Py_DECREF(mod);
            if (cls) {
                zone = PyObject_CallFunction(cls, "s", name.c_str());
                Py_DECREF(cls);
            }
        }
    }
    if (zone && PyDict_SetItem(cache, key, zone) < 0) {
        Py_DECREF(zone);
        zone = nullptr;
    }
    Py_DECREF(key);
    if (zone) Py_DECREF(zone);  // the cache keeps it
    return zone;
}

PyObject* decimal_class() {
    static PyObject* cls = nullptr;
    if (!cls) {
        PyObject* mod = PyImport_ImportModule("decimal");
        if (!mod) return nullptr;
        cls = PyObject_GetAttrString(mod, "Decimal");
        Py_DECREF(mod);
    }
    return cls;
}

Series flat_of(const Series& s) {
    return s.encoding() == dataframe::Encoding::Flat ? s.share()
                                                     : s.materialize();
}

Node build_node(const Series& s) {
    Node n;
    n.col = flat_of(s);
    n.type = n.col.type();
    const dftu_series& h = *n.col.handle();
    n.unit = h.time_unit();
    n.zone = h.timezone();
    n.scale = h.decimal_scale();
    n.width = h.fixed_size();
    switch (n.type) {
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
        case TypeId::Float16:
        case TypeId::Float32:
        case TypeId::Float64:
        case TypeId::String:
        case TypeId::LargeString:
        case TypeId::Binary:
        case TypeId::LargeBinary:
        case TypeId::FixedSizeBinary:
        case TypeId::Date32:
        case TypeId::Date64:
        case TypeId::Time32:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
        case TypeId::Decimal128:
        case TypeId::Decimal256:
            break;
        case TypeId::List:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Map:
            n.kids.push_back(build_node(n.col.child(0)));
            break;
        case TypeId::Struct:
            for (std::int64_t k = 0; k < n.col.num_children(); ++k) {
                n.fields.push_back(n.col.field_name(k));
                n.kids.push_back(build_node(n.col.child(k)));
            }
            break;
        default:
            throw std::invalid_argument(
                std::string("no native conversion for column type '") +
                dataframe::type_name(n.type) + "'");
    }
    return n;
}

// A little-endian two's complement integer of `bytes` bytes as a Python int.
PyObject* wide_int(const std::uint8_t* p, std::size_t bytes) {
    PyObject* raw = PyBytes_FromStringAndSize(reinterpret_cast<const char*>(p),
                                              static_cast<Py_ssize_t>(bytes));
    if (!raw) return nullptr;
    PyObject* args = Py_BuildValue("(Os)", raw, "little");
    PyObject* kwargs = Py_BuildValue("{s:O}", "signed", Py_True);
    Py_DECREF(raw);
    PyObject* out = nullptr;
    PyObject* from_bytes = PyObject_GetAttrString(
        reinterpret_cast<PyObject*>(&PyLong_Type), "from_bytes");
    if (args && kwargs && from_bytes)
        out = PyObject_Call(from_bytes, args, kwargs);
    Py_XDECREF(from_bytes);
    Py_XDECREF(args);
    Py_XDECREF(kwargs);
    return out;
}

PyObject* decimal_cell(const Node& n, std::int64_t i) {
    const std::size_t bytes = n.type == TypeId::Decimal128 ? 16 : 32;
    const std::uint8_t* p =
        n.col.data<std::uint8_t>() + static_cast<std::size_t>(i) * bytes;
    PyObject* value = wide_int(p, bytes);
    if (!value) return nullptr;
    PyObject* text = PyObject_Str(value);
    Py_DECREF(value);
    if (!text) return nullptr;
    PyObject* full =
        PyUnicode_FromFormat("%UE%s%d", text, n.scale > 0 ? "-" : "+",
                             n.scale > 0 ? n.scale : -n.scale);
    Py_DECREF(text);
    if (!full) return nullptr;
    PyObject* cls = decimal_class();
    PyObject* out =
        cls ? PyObject_CallFunctionObjArgs(cls, full, nullptr) : nullptr;
    Py_DECREF(full);
    return out;
}

PyObject* date_cell(std::int64_t days) {
    std::int64_t y = 0;
    int m = 0, d = 0;
    civil_from_days(days, y, m, d);
    if (y < 1 || y > 9999) return range_error("date out of range");
    return PyDate_FromDate(static_cast<int>(y), m, d);
}

PyObject* timestamp_cell(const Node& n, std::int64_t v) {
    std::int64_t us = 0;
    if (!to_micros(v, n.unit, us)) return range_error("timestamp out of range");
    const std::int64_t days = floor_div(us, US_PER_DAY);
    const std::int64_t rem = floor_mod(us, US_PER_DAY);
    std::int64_t y = 0;
    int m = 0, d = 0;
    civil_from_days(days, y, m, d);
    if (y < 1 || y > 9999) return range_error("timestamp out of range");
    const int hh = static_cast<int>(rem / 3'600'000'000LL);
    const int mi = static_cast<int>(rem / 60'000'000LL % 60);
    const int ss = static_cast<int>(rem / 1'000'000LL % 60);
    const int uu = static_cast<int>(rem % 1'000'000LL);
    if (n.zone.empty())
        return PyDateTime_FromDateAndTime(static_cast<int>(y), m, d, hh, mi, ss,
                                          uu);
    PyObject* utc = PyDateTimeAPI->DateTime_FromDateAndTime(
        static_cast<int>(y), m, d, hh, mi, ss, uu, PyDateTime_TimeZone_UTC,
        PyDateTimeAPI->DateTimeType);
    if (!utc || n.zone == "UTC") return utc;
    PyObject* zone = zone_of(n.zone);
    PyObject* out =
        zone ? PyObject_CallMethod(utc, "astimezone", "O", zone) : nullptr;
    Py_DECREF(utc);
    return out;
}

PyObject* cell(const Node& n, std::int64_t i);

// One map entry (a struct of key and value) as a (key, value) tuple.
PyObject* entry_cell(const Node& n, std::int64_t i) {
    if (n.col.is_null(i)) Py_RETURN_NONE;
    PyObject* out = PyTuple_New(static_cast<Py_ssize_t>(n.kids.size()));
    if (!out) return nullptr;
    for (std::size_t f = 0; f < n.kids.size(); ++f) {
        PyObject* v = cell(n.kids[f], i);
        if (!v) {
            Py_DECREF(out);
            return nullptr;
        }
        PyTuple_SET_ITEM(out, static_cast<Py_ssize_t>(f), v);
    }
    return out;
}

PyObject* cell(const Node& n, std::int64_t i) {
    if (n.col.is_null(i)) Py_RETURN_NONE;
    const auto at = static_cast<std::size_t>(i);
    switch (n.type) {
        case TypeId::Bool:
            return PyBool_FromLong(
                (n.col.data<std::uint8_t>()[at >> 3] >> (at & 7)) & 1);
        case TypeId::Int8:
            return PyLong_FromLong(n.col.data<std::int8_t>()[at]);
        case TypeId::Int16:
            return PyLong_FromLong(n.col.data<std::int16_t>()[at]);
        case TypeId::Int32:
            return PyLong_FromLong(n.col.data<std::int32_t>()[at]);
        case TypeId::Int64:
            return PyLong_FromLongLong(n.col.data<std::int64_t>()[at]);
        case TypeId::Uint8:
            return PyLong_FromUnsignedLong(n.col.data<std::uint8_t>()[at]);
        case TypeId::Uint16:
            return PyLong_FromUnsignedLong(n.col.data<std::uint16_t>()[at]);
        case TypeId::Uint32:
            return PyLong_FromUnsignedLong(n.col.data<std::uint32_t>()[at]);
        case TypeId::Uint64:
            return PyLong_FromUnsignedLongLong(n.col.data<std::uint64_t>()[at]);
        case TypeId::Float16:
            return PyFloat_FromDouble(static_cast<double>(
                dataframe::half_to_float(n.col.data<std::uint16_t>()[at])));
        case TypeId::Date32:
            return date_cell(n.col.data<std::int32_t>()[at]);
        case TypeId::Date64:
            return date_cell(
                floor_div(n.col.data<std::int64_t>()[at], 86'400'000LL));
        case TypeId::Time32:
        case TypeId::Time64: {
            const std::int64_t v = n.type == TypeId::Time32
                                       ? n.col.data<std::int32_t>()[at]
                                       : n.col.data<std::int64_t>()[at];
            std::int64_t us = 0;
            if (!to_micros(v, n.unit, us) || us < 0 || us >= US_PER_DAY)
                return range_error("time out of range");
            return PyTime_FromTime(static_cast<int>(us / 3'600'000'000LL),
                                   static_cast<int>(us / 60'000'000LL % 60),
                                   static_cast<int>(us / 1'000'000LL % 60),
                                   static_cast<int>(us % 1'000'000LL));
        }
        case TypeId::Timestamp:
            return timestamp_cell(n, n.col.data<std::int64_t>()[at]);
        case TypeId::Duration: {
            std::int64_t us = 0;
            if (!to_micros(n.col.data<std::int64_t>()[at], n.unit, us))
                return range_error("duration out of range");
            const std::int64_t days = floor_div(us, US_PER_DAY);
            const std::int64_t rem = floor_mod(us, US_PER_DAY);
            if (days < -999'999'999 || days > 999'999'999)
                return range_error("duration out of range");
            return PyDelta_FromDSU(static_cast<int>(days),
                                   static_cast<int>(rem / 1'000'000LL),
                                   static_cast<int>(rem % 1'000'000LL));
        }
        case TypeId::Decimal128:
        case TypeId::Decimal256:
            return decimal_cell(n, i);
        case TypeId::FixedSizeBinary:
            return PyBytes_FromStringAndSize(
                reinterpret_cast<const char*>(n.col.data<std::uint8_t>()) +
                    at * static_cast<std::size_t>(n.width),
                static_cast<Py_ssize_t>(n.width));
        case TypeId::Float32:
            return PyFloat_FromDouble(
                static_cast<double>(n.col.data<float>()[at]));
        case TypeId::Float64:
            return PyFloat_FromDouble(n.col.data<double>()[at]);
        case TypeId::String:
        case TypeId::LargeString: {
            const std::string_view v = n.col.string_at(i);
            return PyUnicode_DecodeUTF8(
                v.data(), static_cast<Py_ssize_t>(v.size()), "strict");
        }
        case TypeId::Binary:
        case TypeId::LargeBinary: {
            const std::string_view v = n.col.string_at(i);
            return PyBytes_FromStringAndSize(v.data(),
                                             static_cast<Py_ssize_t>(v.size()));
        }
        case TypeId::List:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Map: {
            std::int64_t lo, hi;
            if (n.type == TypeId::FixedSizeList) {
                lo = i * n.width;
                hi = lo + n.width;
            } else if (const std::int64_t* o64 = n.col.offsets64()) {
                lo = o64[at];
                hi = o64[at + 1];
            } else {
                lo = n.col.offsets()[at];
                hi = n.col.offsets()[at + 1];
            }
            PyObject* out = PyList_New(static_cast<Py_ssize_t>(hi - lo));
            if (!out) return nullptr;
            for (std::int64_t k = lo; k < hi; ++k) {
                PyObject* v = n.type == TypeId::Map ? entry_cell(n.kids[0], k)
                                                    : cell(n.kids[0], k);
                if (!v) {
                    Py_DECREF(out);
                    return nullptr;
                }
                PyList_SET_ITEM(out, static_cast<Py_ssize_t>(k - lo), v);
            }
            return out;
        }
        default: {  // Struct
            PyObject* out = PyDict_New();
            if (!out) return nullptr;
            for (std::size_t f = 0; f < n.kids.size(); ++f) {
                PyObject* v = cell(n.kids[f], i);
                if (!v ||
                    PyDict_SetItemString(out, n.fields[f].c_str(), v) < 0) {
                    Py_XDECREF(v);
                    Py_DECREF(out);
                    return nullptr;
                }
                Py_DECREF(v);
            }
            return out;
        }
    }
}

using Bits = std::vector<std::uint8_t>;

std::vector<std::uint8_t> valid_bitmap(std::size_t n) {
    return std::vector<std::uint8_t>((n + 7) / 8, 0xFF);
}

void clear_bit(std::vector<std::uint8_t>& bits, std::size_t i) {
    bits[i >> 3] &= static_cast<std::uint8_t>(~(1U << (i & 7)));
}

PyObject* type_error(const char* what) {
    PyErr_SetString(PyExc_TypeError, what);
    return nullptr;
}

}  // namespace

namespace {

PyObject* strings_to_list(const Series& col, const std::uint8_t* valid,
                          PyObject* na);  // below

// A flat integer, float, bool or string column as a list, one typed loop with
// the null test only when the column has nulls. Null when the type has no fast
// loop (the caller then uses the general per-cell reader); an exception is set
// only when the result is null because a build failed.
template <class T, class Make>
PyObject* typed_list(const T* d, std::int64_t n, const std::uint8_t* valid,
                     Make make) {
    PyObject* out = PyList_New(static_cast<Py_ssize_t>(n));
    if (!out) return nullptr;
    PyObject** items = &PyList_GET_ITEM(out, 0);
    for (std::int64_t i = 0; i < n; ++i) {
        PyObject* v;
        if (valid && !((valid[i >> 3] >> (i & 7)) & 1U)) {
            v = Py_None;
            Py_INCREF(v);
        } else {
            v = make(d[i]);
            if (!v) {
                Py_DECREF(out);
                return nullptr;
            }
        }
        items[i] = v;
    }
    return out;
}

bool fast_pylist(const Series& col, PyObject** result) {
    const std::int64_t n = col.length();
    const dftu_series* h = col.handle();
    const std::uint8_t* valid =
        col.null_count() > 0 && h->validity ? h->validity->data() : nullptr;
    if (col.null_count() > 0 && !valid) return false;
    const void* d = dftu_series_data(const_cast<dftu_series*>(h));
    if (!d && n > 0) return false;
    switch (col.type()) {
        case TypeId::Int8:
            *result =
                typed_list(static_cast<const std::int8_t*>(d), n, valid,
                           [](std::int8_t x) { return PyLong_FromLong(x); });
            return true;
        case TypeId::Int16:
            *result =
                typed_list(static_cast<const std::int16_t*>(d), n, valid,
                           [](std::int16_t x) { return PyLong_FromLong(x); });
            return true;
        case TypeId::Int32:
            *result =
                typed_list(static_cast<const std::int32_t*>(d), n, valid,
                           [](std::int32_t x) { return PyLong_FromLong(x); });
            return true;
        case TypeId::Int64:
            *result = typed_list(
                static_cast<const std::int64_t*>(d), n, valid,
                [](std::int64_t x) { return PyLong_FromLongLong(x); });
            return true;
        case TypeId::Uint8:
            *result = typed_list(
                static_cast<const std::uint8_t*>(d), n, valid,
                [](std::uint8_t x) { return PyLong_FromUnsignedLong(x); });
            return true;
        case TypeId::Uint16:
            *result = typed_list(
                static_cast<const std::uint16_t*>(d), n, valid,
                [](std::uint16_t x) { return PyLong_FromUnsignedLong(x); });
            return true;
        case TypeId::Uint32:
            *result = typed_list(
                static_cast<const std::uint32_t*>(d), n, valid,
                [](std::uint32_t x) { return PyLong_FromUnsignedLong(x); });
            return true;
        case TypeId::Uint64:
            *result = typed_list(
                static_cast<const std::uint64_t*>(d), n, valid,
                [](std::uint64_t x) { return PyLong_FromUnsignedLongLong(x); });
            return true;
        case TypeId::Float32:
            *result =
                typed_list(static_cast<const float*>(d), n, valid, [](float x) {
                    return PyFloat_FromDouble(static_cast<double>(x));
                });
            return true;
        case TypeId::Float64:
            *result =
                typed_list(static_cast<const double*>(d), n, valid,
                           [](double x) { return PyFloat_FromDouble(x); });
            return true;
        case TypeId::Bool: {
            const auto* bits = static_cast<const std::uint8_t*>(d);
            PyObject* out = PyList_New(static_cast<Py_ssize_t>(n));
            if (!out) {
                *result = nullptr;
                return true;
            }
            PyObject** items = &PyList_GET_ITEM(out, 0);
            for (std::int64_t i = 0; i < n; ++i) {
                PyObject* v = (valid && !((valid[i >> 3] >> (i & 7)) & 1U))
                                  ? Py_None
                              : ((bits[i >> 3] >> (i & 7)) & 1U) ? Py_True
                                                                 : Py_False;
                Py_INCREF(v);
                items[i] = v;
            }
            *result = out;
            return true;
        }
        case TypeId::String:
        case TypeId::LargeString:
            *result = strings_to_list(col, valid, Py_None);
            return true;
        default:
            return false;
    }
}

}  // namespace

PyObject* Series_to_pylist(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a || !datetime_ready()) return nullptr;
    {
        const Series col = a->encoding() == dataframe::Encoding::Flat
                               ? a->share()
                               : a->materialize();
        PyObject* fast = nullptr;
        if (fast_pylist(col, &fast)) return fast;
    }
    try {
        const Node node = build_node(*a);
        const std::int64_t n = node.col.length();
        PyObject* out = PyList_New(static_cast<Py_ssize_t>(n));
        if (!out) return nullptr;
        for (std::int64_t i = 0; i < n; ++i) {
            PyObject* v = cell(node, i);
            if (!v) {
                Py_DECREF(out);
                return nullptr;
            }
            PyList_SET_ITEM(out, static_cast<Py_ssize_t>(i), v);
        }
        return out;
    } catch (const std::invalid_argument& e) {
        PyErr_SetString(PyExc_TypeError, e.what());
        return nullptr;
    }
}

namespace {

// 8 mask bytes (1 = null) for each validity byte: one store expands a byte of
// the bitmap.
const std::uint64_t* null_expand_table() {
    static const auto* table = [] {
        static std::uint64_t t[256];
        for (unsigned b = 0; b < 256; ++b) {
            std::uint64_t w = 0;
            for (unsigned j = 0; j < 8; ++j)
                if (!((b >> j) & 1U)) w |= std::uint64_t{1} << (8 * j);
            t[b] = w;  // little endian: byte j of the word is row 8k + j
        }
        return t;
    }();
    return table;
}

// n bytes, 1 where the validity bit is 0.
PyObject* null_mask_bytes(const std::uint8_t* valid, std::int64_t n) {
    PyObject* out =
        PyByteArray_FromStringAndSize(nullptr, static_cast<Py_ssize_t>(n));
    if (!out) return nullptr;
    auto* dst = reinterpret_cast<std::uint8_t*>(PyByteArray_AS_STRING(out));
    const std::uint64_t* table = null_expand_table();
    const std::int64_t full = n / 8;
    for (std::int64_t k = 0; k < full; ++k) {
        const std::uint64_t w = table[valid[k]];
        std::memcpy(dst + 8 * k, &w, 8);
    }
    for (std::int64_t i = 8 * full; i < n; ++i)
        dst[i] = ((valid[i >> 3] >> (i & 7)) & 1U) ? 0 : 1;
    return out;
}

// Calls `set(i)` for every row whose validity bit is 0, skipping a byte of
// eight present rows and jumping from zero bit to zero bit inside a byte.
template <class Fn>
void for_each_null(const std::uint8_t* valid, std::int64_t n, Fn set) {
    const std::int64_t bytes = (n + 7) / 8;
    for (std::int64_t k = 0; k < bytes; ++k) {
        auto m = static_cast<unsigned>(~valid[k] & 0xFFU);
        while (m) {
            const std::int64_t i = 8 * k + __builtin_ctz(m);
            if (i < n) set(i);
            m &= m - 1;
        }
    }
}

template <class In>
PyObject* widen_to_double(const In* in, const std::uint8_t* valid,
                          std::int64_t n) {
    PyObject* out =
        PyByteArray_FromStringAndSize(nullptr, static_cast<Py_ssize_t>(n * 8));
    if (!out) return nullptr;
    auto* dst = reinterpret_cast<double*>(PyByteArray_AS_STRING(out));
    for (std::int64_t i = 0; i < n; ++i)
        dst[i] = static_cast<double>(in[i]);  // vectorizes
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for_each_null(valid, n, [&](std::int64_t i) { dst[i] = nan; });
    return out;
}

// The values as bytes; `nan_fill` writes NaN into the null slots of a float
// column.
PyObject* copy_values(const void* data, std::int64_t n, std::size_t width,
                      const std::uint8_t* valid, bool nan_fill) {
    PyObject* out = PyByteArray_FromStringAndSize(
        nullptr, static_cast<Py_ssize_t>(n) * static_cast<Py_ssize_t>(width));
    if (!out) return nullptr;
    auto* dst = reinterpret_cast<std::uint8_t*>(PyByteArray_AS_STRING(out));
    if (n > 0) std::memcpy(dst, data, static_cast<std::size_t>(n) * width);
    if (nan_fill && valid) {
        const double nan64 = std::numeric_limits<double>::quiet_NaN();
        const float nan32 = std::numeric_limits<float>::quiet_NaN();
        for_each_null(valid, n, [&](std::int64_t i) {
            if (width == 8)
                std::memcpy(dst + 8 * i, &nan64, 8);
            else
                std::memcpy(dst + 4 * i, &nan32, 4);
        });
    }
    return out;
}

// A bit-packed Bool column as n bytes of 0 or 1.
PyObject* bool_bytes(const std::uint8_t* bits, std::int64_t n) {
    PyObject* out =
        PyByteArray_FromStringAndSize(nullptr, static_cast<Py_ssize_t>(n));
    if (!out) return nullptr;
    auto* dst = reinterpret_cast<std::uint8_t*>(PyByteArray_AS_STRING(out));
    for (std::int64_t i = 0; i < n; ++i)
        dst[i] = static_cast<std::uint8_t>((bits[i >> 3] >> (i & 7)) & 1U);
    return out;
}

PyObject* tuple3(PyObject* a, PyObject* b, const char* dtype) {
    if (!a) {
        Py_XDECREF(b);
        return nullptr;
    }
    PyObject* d = PyUnicode_FromString(dtype);
    PyObject* t = d ? PyTuple_New(3) : nullptr;
    if (!t) {
        Py_DECREF(a);
        Py_XDECREF(b);
        Py_XDECREF(d);
        return nullptr;
    }
    if (!b) {
        Py_INCREF(Py_None);
        b = Py_None;
    }
    PyTuple_SET_ITEM(t, 0, a);
    PyTuple_SET_ITEM(t, 1, b);
    PyTuple_SET_ITEM(t, 2, d);
    return t;
}

// A cheap hash of a short string's first and last bytes. A collision only
// costs a cache miss (the cached text is compared in full).
inline std::uint64_t quick_hash(const char* p, std::size_t n) {
    std::uint64_t a = 0, b = 0;
    if (n >= 8) {
        std::memcpy(&a, p, 8);
        std::memcpy(&b, p + n - 8, 8);
    } else if (n >= 4) {
        std::uint32_t x, y;
        std::memcpy(&x, p, 4);
        std::memcpy(&y, p + n - 4, 4);
        a = x;
        b = y;
    } else if (n > 0) {
        a = static_cast<std::uint8_t>(p[0]) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[n >> 1]))
             << 8) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[n - 1]))
             << 16);
    }
    std::uint64_t h =
        (a ^ (b * 0x9E3779B97F4A7C15ULL) ^ n) * 0xFF51AFD7ED558CCDULL;
    return h ^ (h >> 32);
}

// Fill `dst` (n PyObject* slots) with a str per row, null rows holding `na`.
// Rows with the same text share one str object (a small cache keyed by
// quick_hash), so a column of repeated names allocates once per distinct name.
// `drop_old` releases what a slot held (a numpy object array starts as None);
// a fresh list's slots are empty. False with an exception set on failure.
bool fill_strings(const Series& col, const std::uint8_t* valid, PyObject* na,
                  PyObject** dst, bool drop_old) {
    const std::int64_t n = col.length();
    struct Slot {
        std::string_view text;
        PyObject* obj =
            nullptr;  // borrowed: the destination holds the reference
    };
    constexpr std::size_t SLOTS = 1U << 16;
    std::vector<Slot> cache(SLOTS);
    for (std::int64_t i = 0; i < n; ++i) {
        PyObject* v;
        if (valid && !((valid[i >> 3] >> (i & 7)) & 1U)) {
            v = na;
            Py_INCREF(v);
        } else {
            const std::string_view t = col.string_at(i);
            Slot& slot = cache[quick_hash(t.data(), t.size()) & (SLOTS - 1)];
            if (slot.obj && slot.text == t) {
                v = slot.obj;
                Py_INCREF(v);
            } else {
                v = PyUnicode_DecodeUTF8(
                    t.data(), static_cast<Py_ssize_t>(t.size()), "strict");
                if (!v) return false;
                slot.text = t;
                slot.obj = v;
            }
        }
        if (drop_old) Py_XDECREF(dst[i]);
        dst[i] = v;
    }
    return true;
}

PyObject* strings_to_list(const Series& col, const std::uint8_t* valid,
                          PyObject* na) {
    const std::int64_t n = col.length();
    PyObject* out = PyList_New(static_cast<Py_ssize_t>(n));
    if (!out) return nullptr;
    if (!fill_strings(col, valid, na, &PyList_GET_ITEM(out, 0), false)) {
        Py_DECREF(out);  // slots past the failure are still NULL, which a list
                         // tolerates
        return nullptr;
    }
    return out;
}

}  // namespace

// np_parts(nullable, na) -> (values, mask, dtype): the column in one native
// pass, ready for numpy.frombuffer with no Python object per row (strings
// aside).
//  - integer and float columns: `values` is a bytearray of the column's bytes
//  and
//    `mask` a bytearray of n bytes (1 = null) or None when there is no null;
//    nullable=False turns nulls into NaN instead (integers widen to float64)
//    and returns no mask;
//  - Bool: 0/1 bytes ("u1"), mask as above, nullable only when the column has
//  nulls;
//  - String / LargeString: a list of str, null rows holding `na`; dtype "O";
//  - any other type: TypeError.
PyObject* Series_np_parts(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    int nullable = 0;
    PyObject* na = Py_None;
    if (!PyArg_ParseTuple(args, "|pO", &nullable, &na)) return nullptr;
    const Series col = a->encoding() == dataframe::Encoding::Flat
                           ? a->share()
                           : a->materialize();
    const dftu_series* h = col.handle();
    const std::int64_t n = col.length();
    const bool has_nulls = col.null_count() > 0;
    const std::uint8_t* valid =
        has_nulls && h->validity ? h->validity->data() : nullptr;
    if (has_nulls && !valid) {
        PyErr_SetString(PyExc_TypeError,
                        "Series has nulls but no validity bitmap");
        return nullptr;
    }
    const void* data = dftu_series_data(const_cast<dftu_series*>(h));
    auto mask = [&]() -> PyObject* {
        return (nullable && valid) ? null_mask_bytes(valid, n) : nullptr;
    };
    auto fixed = [&](std::size_t width, const char* dtype) -> PyObject* {
        if (!data && n > 0) {
            PyErr_SetString(PyExc_TypeError, "Series has no contiguous buffer");
            return nullptr;
        }
        PyObject* m = mask();
        if (nullable && valid && !m) return nullptr;
        return tuple3(copy_values(data, n, width, valid, !nullable), m, dtype);
    };
    auto widen = [&](auto tag, const char* from) -> PyObject* {
        using T = decltype(tag);
        (void)from;
        if (!data && n > 0) {
            PyErr_SetString(PyExc_TypeError, "Series has no contiguous buffer");
            return nullptr;
        }
        return tuple3(widen_to_double(static_cast<const T*>(data), valid, n),
                      nullptr, "<f8");
    };
    try {
        switch (col.type()) {
            case TypeId::Int8:
                return valid && !nullable ? widen(std::int8_t{}, "i1")
                                          : fixed(1, "i1");
            case TypeId::Int16:
                return valid && !nullable ? widen(std::int16_t{}, "i2")
                                          : fixed(2, "<i2");
            case TypeId::Int32:
                return valid && !nullable ? widen(std::int32_t{}, "i4")
                                          : fixed(4, "<i4");
            case TypeId::Int64:
                return valid && !nullable ? widen(std::int64_t{}, "i8")
                                          : fixed(8, "<i8");
            case TypeId::Uint8:
                return valid && !nullable ? widen(std::uint8_t{}, "u1")
                                          : fixed(1, "u1");
            case TypeId::Uint16:
                return valid && !nullable ? widen(std::uint16_t{}, "u2")
                                          : fixed(2, "<u2");
            case TypeId::Uint32:
                return valid && !nullable ? widen(std::uint32_t{}, "u4")
                                          : fixed(4, "<u4");
            case TypeId::Uint64:
                return valid && !nullable ? widen(std::uint64_t{}, "u8")
                                          : fixed(8, "<u8");
            case TypeId::Float32:
                return fixed(4, "<f4");
            case TypeId::Float64:
                return fixed(8, "<f8");
            case TypeId::Bool: {
                if (valid && !nullable) {
                    PyErr_SetString(
                        PyExc_TypeError,
                        "a Bool column with nulls needs nullable=True");
                    return nullptr;
                }
                if (!data && n > 0) {
                    PyErr_SetString(PyExc_TypeError,
                                    "Series has no contiguous buffer");
                    return nullptr;
                }
                PyObject* m = mask();
                if (nullable && valid && !m) return nullptr;
                return tuple3(
                    bool_bytes(static_cast<const std::uint8_t*>(data), n), m,
                    "u1");
            }
            case TypeId::String:
            case TypeId::LargeString:
                return tuple3(strings_to_list(col, valid, na), nullptr, "O");
            default:
                PyErr_SetString(PyExc_TypeError,
                                "column type has no native numpy conversion");
                return nullptr;
        }
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_TypeError, e.what());
        return nullptr;
    }
}

// str_into(out, na): the String column's rows into `out`, a writable
// one-dimensional numpy object array of the column's length (made with
// numpy.empty(n, dtype=object)); null rows get `na`. Returns None.
PyObject* Series_str_into(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    PyObject* out = nullptr;
    PyObject* na = Py_None;
    if (!PyArg_ParseTuple(args, "O|O", &out, &na)) return nullptr;
    const Series col = a->encoding() == dataframe::Encoding::Flat
                           ? a->share()
                           : a->materialize();
    if (col.type() != TypeId::String && col.type() != TypeId::LargeString) {
        PyErr_SetString(PyExc_TypeError, "str_into needs a String column");
        return nullptr;
    }
    Py_buffer view;
    if (PyObject_GetBuffer(
            out, &view, PyBUF_WRITABLE | PyBUF_FORMAT | PyBUF_C_CONTIGUOUS) < 0)
        return nullptr;
    const std::int64_t n = col.length();
    if (view.itemsize != static_cast<Py_ssize_t>(sizeof(PyObject*)) ||
        view.format == nullptr || view.format[0] != 'O' ||
        view.len != n * static_cast<Py_ssize_t>(sizeof(PyObject*))) {
        PyBuffer_Release(&view);
        PyErr_SetString(
            PyExc_ValueError,
            "str_into needs an object array of the column's length");
        return nullptr;
    }
    const dftu_series* h = col.handle();
    const std::uint8_t* valid =
        col.null_count() > 0 && h->validity ? h->validity->data() : nullptr;
    const bool ok =
        fill_strings(col, valid, na, static_cast<PyObject**>(view.buf), true);
    PyBuffer_Release(&view);
    if (!ok) return nullptr;
    Py_RETURN_NONE;
}

PyObject* Series_item(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a || !datetime_ready()) return nullptr;
    const Py_ssize_t i = PyLong_AsSsize_t(arg);
    if (i == -1 && PyErr_Occurred()) return nullptr;
    if (i < 0 || i >= a->length()) {
        PyErr_SetString(PyExc_IndexError, "Series index out of range");
        return nullptr;
    }
    try {
        return cell(build_node(*a), i);
    } catch (const std::invalid_argument& e) {
        PyErr_SetString(PyExc_TypeError, e.what());
        return nullptr;
    }
}

namespace {

PyObject* build_plain(PyObject** items, Py_ssize_t n, Bits& bits, bool any_bool,
                      bool any_int, bool any_float, bool any_str,
                      bool any_bytes, bool any_big) {
    Series out;
    if (any_bool) {
        std::vector<std::uint8_t> packed((static_cast<std::size_t>(n) + 7) / 8,
                                         0);
        for (Py_ssize_t i = 0; i < n; ++i) {
            if (items[i] == Py_None) {
                clear_bit(bits, static_cast<std::size_t>(i));
            } else if (items[i] == Py_True) {
                packed[static_cast<std::size_t>(i) >> 3] |=
                    static_cast<std::uint8_t>(1U << (i & 7));
            }
        }
        out = Series::flat(TypeId::Bool, packed.data(), n, bits.data());
    } else if (any_float) {
        std::vector<double> data(static_cast<std::size_t>(n), 0.0);
        for (Py_ssize_t i = 0; i < n; ++i) {
            if (items[i] == Py_None) {
                clear_bit(bits, static_cast<std::size_t>(i));
                continue;
            }
            const double x = PyFloat_AsDouble(items[i]);
            if (x == -1.0 && PyErr_Occurred()) {
                return nullptr;
            }
            data[static_cast<std::size_t>(i)] = x;
        }
        out = Series::flat(TypeId::Float64, data.data(), n, bits.data());
    } else if (any_int) {
        if (any_big) {
            std::vector<std::uint64_t> data(static_cast<std::size_t>(n), 0);
            for (Py_ssize_t i = 0; i < n; ++i) {
                if (items[i] == Py_None) {
                    clear_bit(bits, static_cast<std::size_t>(i));
                    continue;
                }
                data[static_cast<std::size_t>(i)] =
                    PyLong_AsUnsignedLongLong(items[i]);
                if (PyErr_Occurred()) {
                    return nullptr;
                }
            }
            out = Series::flat(TypeId::Uint64, data.data(), n, bits.data());
        } else {
            std::vector<std::int64_t> data(static_cast<std::size_t>(n), 0);
            for (Py_ssize_t i = 0; i < n; ++i) {
                if (items[i] == Py_None) {
                    clear_bit(bits, static_cast<std::size_t>(i));
                    continue;
                }
                data[static_cast<std::size_t>(i)] = PyLong_AsLongLong(items[i]);
            }
            out = Series::flat(TypeId::Int64, data.data(), n, bits.data());
        }
    } else if (any_str || any_bytes) {
        std::vector<std::int32_t> offsets(static_cast<std::size_t>(n) + 1, 0);
        std::string bytes;
        for (Py_ssize_t i = 0; i < n; ++i) {
            if (items[i] == Py_None) {
                clear_bit(bits, static_cast<std::size_t>(i));
            } else if (any_str) {
                Py_ssize_t len = 0;
                const char* s = PyUnicode_AsUTF8AndSize(items[i], &len);
                if (!s) {
                    return nullptr;
                }
                bytes.append(s, static_cast<std::size_t>(len));
            } else {
                bytes.append(
                    PyBytes_AS_STRING(items[i]),
                    static_cast<std::size_t>(PyBytes_GET_SIZE(items[i])));
            }
            if (bytes.size() > static_cast<std::size_t>(INT32_MAX)) {
                return type_error("string data exceeds 2 GiB");
            }
            offsets[static_cast<std::size_t>(i) + 1] =
                static_cast<std::int32_t>(bytes.size());
        }
        out = Series{dftu_series_new_string(
            static_cast<dftu_dtype>(any_str ? TypeId::String : TypeId::Binary),
            offsets.data(), bytes.data(), n, bits.data())};
    }
    return make_series(std::move(out));
}

}  // namespace

namespace {

enum class Kind {
    LIST,
    DICT,
    BOOL,
    INT,
    FLOAT,
    STR,
    BYTES,
    DATETIME,
    DATE,
    TIME,
    DELTA,
    DECIMAL
};

PyObject* fail(const char* what) {
    PyErr_SetString(PyExc_TypeError, what);
    return nullptr;
}

// A value the type cannot hold, as against a type this import does not know.
PyObject* bad_value(const char* what) {
    PyErr_SetString(PyExc_ValueError, what);
    return nullptr;
}

std::optional<Kind> kind_of(PyObject* v, bool& other) {
    if (PyBool_Check(v)) return Kind::BOOL;
    if (PyLong_Check(v)) return Kind::INT;
    if (PyFloat_Check(v)) return Kind::FLOAT;
    if (PyUnicode_Check(v)) return Kind::STR;
    if (PyBytes_Check(v)) return Kind::BYTES;
    if (PyDateTime_Check(v)) return Kind::DATETIME;
    if (PyDate_Check(v)) return Kind::DATE;
    if (PyTime_Check(v)) return Kind::TIME;
    if (PyDelta_Check(v)) return Kind::DELTA;
    if (PyList_Check(v) || PyTuple_Check(v)) return Kind::LIST;
    if (PyDict_Check(v)) return Kind::DICT;
    PyObject* cls = decimal_class();
    if (!cls) return std::nullopt;
    const int is = PyObject_IsInstance(v, cls);
    if (is < 0) return std::nullopt;
    if (is) return Kind::DECIMAL;
    other = true;
    return std::nullopt;
}

// The offset of an aware datetime as microseconds, or nullopt for a naive
// one; -1 in `error` on a Python error.
std::optional<std::int64_t> utc_offset(PyObject* v, bool& error) {
    PyObject* off = PyObject_CallMethod(v, "utcoffset", nullptr);
    if (!off) {
        error = true;
        return std::nullopt;
    }
    if (off == Py_None) {
        Py_DECREF(off);
        return std::nullopt;
    }
    const std::int64_t us =
        (static_cast<std::int64_t>(PyDateTime_DELTA_GET_DAYS(off)) * 86400 +
         PyDateTime_DELTA_GET_SECONDS(off)) *
            1'000'000 +
        PyDateTime_DELTA_GET_MICROSECONDS(off);
    Py_DECREF(off);
    return us;
}

// The zone name of an aware datetime: a ZoneInfo key, a pytz zone, UTC, or
// its fixed offset as +HH:MM.
std::string zone_name(PyObject* v, std::int64_t offset_us) {
    PyObject* tz = PyObject_GetAttrString(v, "tzinfo");
    std::string name;
    if (tz) {
        for (const char* attr : {"key", "zone"}) {
            PyObject* n = PyObject_GetAttrString(tz, attr);
            if (n && PyUnicode_Check(n)) name = PyUnicode_AsUTF8(n);
            Py_XDECREF(n);
            if (!name.empty()) break;
            PyErr_Clear();
        }
        Py_DECREF(tz);
    } else {
        PyErr_Clear();
    }
    if (!name.empty()) return name;
    if (offset_us == 0) return "UTC";
    char buf[16];
    const std::int64_t abs_min =
        (offset_us < 0 ? -offset_us : offset_us) / 60'000'000;
    std::snprintf(buf, sizeof buf, "%c%02d:%02d", offset_us < 0 ? '-' : '+',
                  static_cast<int>(abs_min / 60),
                  static_cast<int>(abs_min % 60));
    return buf;
}

PyObject* build_datetimes(PyObject** items, Py_ssize_t n, Bits& bits) {
    std::vector<std::int64_t> micros(static_cast<std::size_t>(n), 0);
    std::optional<bool> aware;
    std::string zone;
    bool mixed_zones = false;
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* v = items[i];
        if (v == Py_None) {
            clear_bit(bits, static_cast<std::size_t>(i));
            continue;
        }
        bool error = false;
        const auto off = utc_offset(v, error);
        if (error) return nullptr;
        if (aware && *aware != off.has_value())
            return bad_value("cannot mix naive and timezone-aware datetimes");
        aware = off.has_value();
        std::int64_t us =
            days_from_civil(PyDateTime_GET_YEAR(v), PyDateTime_GET_MONTH(v),
                            PyDateTime_GET_DAY(v)) *
                US_PER_DAY +
            (PyDateTime_DATE_GET_HOUR(v) * 3600LL +
             PyDateTime_DATE_GET_MINUTE(v) * 60LL +
             PyDateTime_DATE_GET_SECOND(v)) *
                1'000'000LL +
            PyDateTime_DATE_GET_MICROSECOND(v);
        if (off) {
            us -= *off;
            const std::string name = zone_name(v, *off);
            if (zone.empty())
                zone = name;
            else if (zone != name)
                mixed_zones = true;
        }
        micros[static_cast<std::size_t>(i)] = us;
    }
    if (mixed_zones) zone = "UTC";
    Series base = Series::flat_i64(micros.data(), n, bits.data());
    return make_series(retype_series(
        base, TypeId::Timestamp, dataframe::TimeUnit::Micro, zone, 0, 0, 0));
}

PyObject* build_dates(PyObject** items, Py_ssize_t n, Bits& bits) {
    std::vector<std::int32_t> days(static_cast<std::size_t>(n), 0);
    for (Py_ssize_t i = 0; i < n; ++i) {
        if (items[i] == Py_None) {
            clear_bit(bits, static_cast<std::size_t>(i));
            continue;
        }
        days[static_cast<std::size_t>(i)] =
            static_cast<std::int32_t>(days_from_civil(
                PyDateTime_GET_YEAR(items[i]), PyDateTime_GET_MONTH(items[i]),
                PyDateTime_GET_DAY(items[i])));
    }
    Series base = Series::flat(TypeId::Int32, days.data(), n, bits.data());
    return make_series(retype_series(base, TypeId::Date32,
                                     dataframe::TimeUnit::Micro, "", 0, 0, 0));
}

PyObject* build_times(PyObject** items, Py_ssize_t n, Bits& bits) {
    std::vector<std::int64_t> micros(static_cast<std::size_t>(n), 0);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* v = items[i];
        if (v == Py_None) {
            clear_bit(bits, static_cast<std::size_t>(i));
            continue;
        }
        if (reinterpret_cast<PyDateTime_Time*>(v)->hastzinfo)
            return bad_value("timezone-aware times are not supported");
        micros[static_cast<std::size_t>(i)] =
            (PyDateTime_TIME_GET_HOUR(v) * 3600LL +
             PyDateTime_TIME_GET_MINUTE(v) * 60LL +
             PyDateTime_TIME_GET_SECOND(v)) *
                1'000'000LL +
            PyDateTime_TIME_GET_MICROSECOND(v);
    }
    Series base = Series::flat_i64(micros.data(), n, bits.data());
    return make_series(retype_series(base, TypeId::Time64,
                                     dataframe::TimeUnit::Micro, "", 0, 0, 0));
}

PyObject* build_deltas(PyObject** items, Py_ssize_t n, Bits& bits) {
    std::vector<std::int64_t> micros(static_cast<std::size_t>(n), 0);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* v = items[i];
        if (v == Py_None) {
            clear_bit(bits, static_cast<std::size_t>(i));
            continue;
        }
        micros[static_cast<std::size_t>(i)] =
            (static_cast<std::int64_t>(PyDateTime_DELTA_GET_DAYS(v)) * 86400 +
             PyDateTime_DELTA_GET_SECONDS(v)) *
                1'000'000LL +
            PyDateTime_DELTA_GET_MICROSECONDS(v);
    }
    Series base = Series::flat_i64(micros.data(), n, bits.data());
    return make_series(retype_series(base, TypeId::Duration,
                                     dataframe::TimeUnit::Micro, "", 0, 0, 0));
}

__extension__ typedef unsigned __int128 U128;

constexpr int MAX_DECIMAL_DIGITS = 38;

struct DecimalParts {
    bool negative = false;
    std::vector<int> digits;
    long exponent = 0;
};

// The sign, digits and exponent of a Decimal or int; false on error.
bool decimal_parts(PyObject* v, DecimalParts& out) {
    if (PyLong_Check(v)) {
        PyObject* text = PyObject_Str(v);
        if (!text) return false;
        const char* s = PyUnicode_AsUTF8(text);
        if (!s) {
            Py_DECREF(text);
            return false;
        }
        out.negative = s[0] == '-';
        for (const char* c = s + (out.negative ? 1 : 0); *c; ++c)
            out.digits.push_back(*c - '0');
        Py_DECREF(text);
        out.exponent = 0;
        return true;
    }
    PyObject* tuple = PyObject_CallMethod(v, "as_tuple", nullptr);
    if (!tuple) return false;
    PyObject* sign = PyTuple_GetItem(tuple, 0);
    PyObject* digits = PyTuple_GetItem(tuple, 1);
    PyObject* exponent = PyTuple_GetItem(tuple, 2);
    bool ok = sign && digits && exponent;
    if (ok && !PyLong_Check(exponent)) {
        PyErr_SetString(PyExc_ValueError,
                        "NaN and infinite decimals are not supported");
        ok = false;
    }
    if (ok) {
        out.negative = PyLong_AsLong(sign) != 0;
        out.exponent = PyLong_AsLong(exponent);
        for (Py_ssize_t k = 0; k < PyTuple_GET_SIZE(digits); ++k)
            out.digits.push_back(
                static_cast<int>(PyLong_AsLong(PyTuple_GET_ITEM(digits, k))));
    }
    Py_DECREF(tuple);
    return ok && !PyErr_Occurred();
}

PyObject* build_decimals(PyObject** items, Py_ssize_t n, Bits& bits) {
    std::vector<DecimalParts> parts(static_cast<std::size_t>(n));
    long scale = 0;
    for (Py_ssize_t i = 0; i < n; ++i) {
        if (items[i] == Py_None) {
            clear_bit(bits, static_cast<std::size_t>(i));
            continue;
        }
        DecimalParts& p = parts[static_cast<std::size_t>(i)];
        if (!decimal_parts(items[i], p)) return nullptr;
        scale = std::max(scale, -p.exponent);
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(n) * 16, 0);
    int precision = 1;
    for (Py_ssize_t i = 0; i < n; ++i) {
        if (items[i] == Py_None) continue;
        const DecimalParts& p = parts[static_cast<std::size_t>(i)];
        int digits = static_cast<int>(p.digits.size()) +
                     static_cast<int>(p.exponent + scale);
        if (digits > MAX_DECIMAL_DIGITS)
            return bad_value(
                "decimal values wider than 38 digits are not supported");
        U128 v = 0;
        for (const int d : p.digits) v = v * 10 + static_cast<U128>(d);
        for (long k = 0; k < p.exponent + scale; ++k) v *= 10;
        if (p.negative) v = static_cast<U128>(0) - v;
        std::memcpy(bytes.data() + static_cast<std::size_t>(i) * 16, &v, 16);
        precision = std::max(precision, digits);
    }
    Series col = Series::flat(TypeId::Decimal128, bytes.data(), n, bits.data());
    if (!col.valid()) return fail("cannot build a decimal column");
    col.handle()->set_decimal(precision, static_cast<std::int32_t>(scale));
    return make_series(std::move(col));
}

PyObject* typed_nulls(TypeId type, Py_ssize_t n) {
    switch (type) {
        case TypeId::Date32:
            return make_series(retype_series(Series::nulls(TypeId::Int32, n),
                                             type, dataframe::TimeUnit::Micro,
                                             "", 0, 0, 0));
        case TypeId::Date64:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
            return make_series(retype_series(Series::nulls(TypeId::Int64, n),
                                             type, dataframe::TimeUnit::Micro,
                                             "", 0, 0, 0));
        default: {
            Series out = Series::nulls(type, n);
            if (!out.valid())
                return fail("cannot build a null column of that type");
            return make_series(std::move(out));
        }
    }
}

PyObject* from_items(PyObject** items, Py_ssize_t n, int want, int empty);

// `series` with `bits` as its validity, when a row is null.
PyObject* with_validity(Series series, const Bits& bits, Py_ssize_t n) {
    bool any = false;
    for (Py_ssize_t i = 0; i < n; ++i)
        any = any || !((bits[static_cast<std::size_t>(i >> 3)] >> (i & 7)) & 1);
    if (!any) return make_series(std::move(series));
    auto* h = new dftu_series(*series.handle());
    h->validity = dataframe::Buffer::allocate(bits.size());
    std::memcpy(h->validity->data(), bits.data(), bits.size());
    std::int64_t nulls = 0;
    for (Py_ssize_t i = 0; i < n; ++i)
        nulls += !((bits[static_cast<std::size_t>(i >> 3)] >> (i & 7)) & 1);
    h->null_count = nulls;
    return make_series(Series{h});
}

// The elements of every list (or tuple) as one child column.
PyObject* build_lists(PyObject** items, Py_ssize_t n, Bits& bits) {
    std::vector<PyObject*> flat;
    std::vector<std::int32_t> offsets(static_cast<std::size_t>(n) + 1, 0);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* v = items[i];
        if (v == Py_None) {
            clear_bit(bits, static_cast<std::size_t>(i));
        } else {
            PyObject** first = PyList_Check(v) ? &PyList_GET_ITEM(v, 0)
                                               : &PyTuple_GET_ITEM(v, 0);
            const Py_ssize_t len =
                PyList_Check(v) ? PyList_GET_SIZE(v) : PyTuple_GET_SIZE(v);
            for (Py_ssize_t k = 0; k < len; ++k) flat.push_back(first[k]);
        }
        offsets[static_cast<std::size_t>(i) + 1] =
            static_cast<std::int32_t>(flat.size());
    }
    PyObject* child =
        from_items(flat.data(), static_cast<Py_ssize_t>(flat.size()), 0,
                   static_cast<int>(TypeId::String));
    if (!child) return nullptr;
    Series* c = as_series(child);
    Series list = Series::list(offsets, c->share());
    Py_DECREF(child);
    return with_validity(std::move(list), bits, n);
}

// One struct field per key, in first-seen order; a missing key is null.
PyObject* build_structs(PyObject** items, Py_ssize_t n, Bits& bits) {
    std::vector<std::string> names;
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* d = items[i];
        if (d == Py_None) continue;
        PyObject *key = nullptr, *value = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(d, &pos, &key, &value)) {
            if (!PyUnicode_Check(key))
                return bad_value("dict keys must be str");
            const char* k = PyUnicode_AsUTF8(key);
            if (!k) return nullptr;
            if (std::find(names.begin(), names.end(), k) == names.end())
                names.emplace_back(k);
        }
    }
    std::vector<Series> fields;
    for (const std::string& name : names) {
        std::vector<PyObject*> column;
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject* value = items[i] == Py_None ? nullptr
                                                  : PyDict_GetItemString(
                                                        items[i], name.c_str());
            column.push_back(value ? value : Py_None);
        }
        PyObject* built =
            from_items(column.data(), static_cast<Py_ssize_t>(column.size()), 0,
                       static_cast<int>(TypeId::String));
        if (!built) return nullptr;
        fields.push_back(as_series(built)->share());
        Py_DECREF(built);
    }
    for (Py_ssize_t i = 0; i < n; ++i)
        if (items[i] == Py_None) clear_bit(bits, static_cast<std::size_t>(i));
    if (fields.empty())
        return bad_value("cannot infer the fields of empty dicts");
    return with_validity(Series::structs(std::move(names), std::move(fields)),
                         bits, n);
}

PyObject* from_items(PyObject** items, Py_ssize_t n, int want, int empty) {
    bool seen[12] = {};
    bool any_other = false, any_negative = false, any_big = false,
         any_value = false;
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* v = items[i];
        if (v == Py_None) continue;
        any_value = true;
        bool other = false;
        const auto k = kind_of(v, other);
        if (!k && !other) {
            return nullptr;
        }
        if (!k) {
            any_other = true;
            continue;
        }
        seen[static_cast<int>(*k)] = true;
        if (*k == Kind::INT) {
            int overflow = 0;
            const long long x = PyLong_AsLongLongAndOverflow(v, &overflow);
            if (x == -1 && !overflow && PyErr_Occurred()) {
                return nullptr;
            }
            if (overflow > 0) any_big = true;
            if (overflow < 0 || (!overflow && x < 0)) any_negative = true;
            if (overflow < 0) any_other = true;
        }
    }
    auto has = [&](Kind k) { return seen[static_cast<int>(k)]; };
    const bool numeric = has(Kind::INT) || has(Kind::FLOAT);
    const bool decimal = has(Kind::DECIMAL);
    const int kinds =
        int(has(Kind::LIST)) + int(has(Kind::DICT)) + int(has(Kind::BOOL)) +
        int(numeric && !decimal) + int(decimal) + int(has(Kind::STR)) +
        int(has(Kind::BYTES)) + int(has(Kind::DATETIME)) +
        int(has(Kind::DATE)) + int(has(Kind::TIME)) + int(has(Kind::DELTA));
    if (!any_value) {
        if (empty == 0)
            return fail("cannot infer a type from an empty or all-None list");
        return typed_nulls(static_cast<TypeId>(empty), n);
    }
    if (any_other) {
        return fail(
            "list values must be one of bool, int, float, str, bytes, "
            "datetime, date, time, timedelta, Decimal, list or dict, with "
            "None for null");
    }
    if (kinds != 1 || (decimal && has(Kind::FLOAT)) ||
        (any_big && any_negative) || (any_big && has(Kind::FLOAT))) {
        return bad_value(
            "list values have different types (or an int too large for int64 "
            "beside a negative or float one)");
    }

    auto bits = valid_bitmap(static_cast<std::size_t>(n));
    PyObject* built = nullptr;
    if (has(Kind::LIST)) {
        built = build_lists(items, n, bits);
    } else if (has(Kind::DICT)) {
        built = build_structs(items, n, bits);
    } else if (has(Kind::DATETIME)) {
        built = build_datetimes(items, n, bits);
    } else if (has(Kind::DATE)) {
        built = build_dates(items, n, bits);
    } else if (has(Kind::TIME)) {
        built = build_times(items, n, bits);
    } else if (has(Kind::DELTA)) {
        built = build_deltas(items, n, bits);
    } else if (decimal) {
        built = build_decimals(items, n, bits);
    } else {
        built = build_plain(items, n, bits, has(Kind::BOOL), has(Kind::INT),
                            has(Kind::FLOAT), has(Kind::STR), has(Kind::BYTES),
                            any_big);
    }
    if (!built || want == 0) return built;
    Series* got = as_series(built);
    if (!got || static_cast<int>(got->type()) == want) return built;
    Series cast = got->cast(static_cast<TypeId>(want));
    Py_DECREF(built);
    if (!cast.valid())
        return fail("cannot convert the list to the requested type");
    return make_series(std::move(cast));
}

}  // namespace

PyObject* vec_from_list(PyObject* /*self*/, PyObject* call_args) {
    PyObject* obj = nullptr;
    int want = 0;
    if (!PyArg_ParseTuple(call_args, "O|i", &obj, &want)) return nullptr;
    if (!datetime_ready()) return nullptr;
    PyObject* seq = PySequence_Fast(obj, "expected a sequence");
    if (!seq) return nullptr;
    PyObject* out = from_items(PySequence_Fast_ITEMS(seq),
                               PySequence_Fast_GET_SIZE(seq), want, want);
    Py_DECREF(seq);
    return out;
}

}  // namespace dftracer::utils::python::series_detail
