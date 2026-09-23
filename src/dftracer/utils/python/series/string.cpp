#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/python/series_detail.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dftracer::utils::python::series_detail {

namespace {

// Read a Python str as a string_view; the buffer stays owned by `arg`.
bool as_str_view(PyObject* arg, std::string_view* out) {
    Py_ssize_t len = 0;
    const char* data = PyUnicode_AsUTF8AndSize(arg, &len);
    if (!data) return false;
    *out = std::string_view(data, static_cast<std::size_t>(len));
    return true;
}

// A single-character pad string -> its first byte; empty is rejected.
bool as_fill_char(PyObject* arg, char* out) {
    std::string_view s;
    if (!as_str_view(arg, &s)) return false;
    if (s.empty()) {
        PyErr_SetString(PyExc_ValueError, "fill must be a non-empty string");
        return false;
    }
    *out = s[0];
    return true;
}

}  // namespace

PyObject* Series_str_eq(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    std::string_view rhs;
    if (!as_str_view(arg, &rhs)) return nullptr;
    return make_series(a->str_eq(rhs));
}
PyObject* Series_str_contains(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    std::string_view needle;
    if (!as_str_view(arg, &needle)) return nullptr;
    return make_series(a->str_contains(needle));
}
PyObject* Series_str_starts_with(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    std::string_view prefix;
    if (!as_str_view(arg, &prefix)) return nullptr;
    return make_series(a->str_starts_with(prefix));
}
PyObject* Series_str_ends_with(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    std::string_view suffix;
    if (!as_str_view(arg, &suffix)) return nullptr;
    return make_series(a->str_ends_with(suffix));
}
PyObject* Series_str_matches(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    std::string_view pattern;
    if (!as_str_view(arg, &pattern)) return nullptr;
    return make_series(a->str_matches(pattern));
}
PyObject* Series_str_search(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    std::string_view pattern;
    if (!as_str_view(arg, &pattern)) return nullptr;
    return make_series(a->str_search(pattern));
}
PyObject* Series_str_like(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    std::string_view pattern;
    if (!as_str_view(arg, &pattern)) return nullptr;
    return make_series(a->str_like(pattern));
}
PyObject* Series_str_len_bytes(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return make_series(a->str_len_bytes());
}
PyObject* Series_str_len_chars(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return make_series(a->str_len_chars());
}
PyObject* Series_str_find(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    std::string_view needle;
    if (!as_str_view(arg, &needle)) return nullptr;
    return make_series(a->str_find(needle));
}
PyObject* Series_fnv1a(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return make_series(a->fnv1a());
}
PyObject* Series_hex64_parse(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return make_series(a->hex64_parse());
}
PyObject* Series_hex64_format(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return make_series(a->hex64_format());
}
PyObject* Series_to_lowercase(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return make_series(a->to_lowercase());
}
PyObject* Series_to_uppercase(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return make_series(a->to_uppercase());
}
PyObject* Series_str_strip(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return make_series(a->str_strip());
}
PyObject* Series_str_lstrip(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return make_series(a->str_lstrip());
}
PyObject* Series_str_rstrip(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return make_series(a->str_rstrip());
}
PyObject* Series_str_replace(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    PyObject* pat = nullptr;
    PyObject* repl = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &pat, &repl)) return nullptr;
    std::string_view p, r;
    if (!as_str_view(pat, &p) || !as_str_view(repl, &r)) return nullptr;
    return make_series(a->str_replace(p, r));
}
PyObject* Series_str_replace_all(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    PyObject* pat = nullptr;
    PyObject* repl = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &pat, &repl)) return nullptr;
    std::string_view p, r;
    if (!as_str_view(pat, &p) || !as_str_view(repl, &r)) return nullptr;
    return make_series(a->str_replace_all(p, r));
}
PyObject* Series_str_slice(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long start = 0;
    long long length = -1;
    if (!PyArg_ParseTuple(args, "L|L", &start, &length)) return nullptr;
    return make_series(a->str_slice(static_cast<std::int64_t>(start),
                                    static_cast<std::int64_t>(length)));
}
PyObject* Series_str_pad_start(PyObject* self, PyObject* args, PyObject* kwds) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long width = 0;
    PyObject* fill = nullptr;
    static const char* kwlist[] = {"width", "fill", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "L|O",
                                     const_cast<char**>(kwlist), &width, &fill))
        return nullptr;
    char f = ' ';
    if (fill && !as_fill_char(fill, &f)) return nullptr;
    return make_series(a->str_pad_start(static_cast<std::int64_t>(width), f));
}
PyObject* Series_str_pad_end(PyObject* self, PyObject* args, PyObject* kwds) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long width = 0;
    PyObject* fill = nullptr;
    static const char* kwlist[] = {"width", "fill", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "L|O",
                                     const_cast<char**>(kwlist), &width, &fill))
        return nullptr;
    char f = ' ';
    if (fill && !as_fill_char(fill, &f)) return nullptr;
    return make_series(a->str_pad_end(static_cast<std::int64_t>(width), f));
}
PyObject* Series_str_zfill(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long width = PyLong_AsLongLong(arg);
    if (width == -1 && PyErr_Occurred()) return nullptr;
    return make_series(a->str_zfill(static_cast<std::int64_t>(width)));
}
PyObject* Series_str_split(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    std::string_view sep;
    if (!as_str_view(arg, &sep)) return nullptr;
    return make_series(a->str_split(sep));
}
PyObject* Series_str_extract(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    PyObject* pat = nullptr;
    long long group = 1;
    if (!PyArg_ParseTuple(args, "O|L", &pat, &group)) return nullptr;
    std::string_view pattern;
    if (!as_str_view(pat, &pattern)) return nullptr;
    if (group < 0) {
        PyErr_SetString(PyExc_ValueError, "str_extract: group must be >= 0");
        return nullptr;
    }
    Series out = a->str_extract(pattern, static_cast<std::int64_t>(group));
    if (!out.valid()) {
        PyErr_SetString(PyExc_ValueError,
                        "str_extract: the pattern does not compile");
        return nullptr;
    }
    return make_series(std::move(out));
}
PyObject* Series_list_len(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    Series out = a->list_len();
    if (!out.valid()) {
        PyErr_SetString(PyExc_TypeError, "list_len: not a List column");
        return nullptr;
    }
    return make_series(std::move(out));
}
PyObject* Series_list_get(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long index = PyLong_AsLongLong(arg);
    if (index == -1 && PyErr_Occurred()) return nullptr;
    Series out = a->list_get(static_cast<std::int64_t>(index));
    if (!out.valid()) {
        PyErr_SetString(PyExc_TypeError, "list_get: not a List column");
        return nullptr;
    }
    return make_series(std::move(out));
}

namespace {

// A kernel that takes one str argument, refusing a non-String column.
template <Series (Series::*Method)(std::string_view) const>
PyObject* str_arg_op(PyObject* self, PyObject* arg, const char* name) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    std::string_view s;
    if (!as_str_view(arg, &s)) return nullptr;
    Series out = (a->*Method)(s);
    if (!out.valid()) {
        PyErr_Format(PyExc_TypeError, "%s: not a String column", name);
        return nullptr;
    }
    return make_series(std::move(out));
}

}  // namespace

PyObject* Series_str_case(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    const long code = PyLong_AsLong(arg);
    if (code == -1 && PyErr_Occurred()) return nullptr;
    if (code < DFTU_STR_CAPITALIZE || code > DFTU_STR_SWAPCASE) {
        PyErr_SetString(PyExc_ValueError,
                        "str_case: code must be 0 (capitalize), 1 (title) "
                        "or 2 (swapcase)");
        return nullptr;
    }
    Series out = a->str_case(static_cast<std::int32_t>(code));
    if (!out.valid()) {
        PyErr_SetString(PyExc_TypeError, "str_case: not a String column");
        return nullptr;
    }
    return make_series(std::move(out));
}
PyObject* Series_str_is(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    const long code = PyLong_AsLong(arg);
    if (code == -1 && PyErr_Occurred()) return nullptr;
    if (code < DFTU_STR_ALNUM || code > DFTU_STR_TITLE) {
        PyErr_SetString(PyExc_ValueError,
                        "str_is: code must be 0..8 (alnum, alpha, digit, "
                        "decimal, numeric, space, lower, upper, title)");
        return nullptr;
    }
    Series out = a->str_is(static_cast<std::int32_t>(code));
    if (!out.valid()) {
        PyErr_SetString(PyExc_TypeError, "str_is: not a String column");
        return nullptr;
    }
    return make_series(std::move(out));
}
PyObject* Series_str_count(PyObject* self, PyObject* arg) {
    return str_arg_op<&Series::str_count>(self, arg, "str_count");
}
PyObject* Series_str_rfind(PyObject* self, PyObject* arg) {
    return str_arg_op<&Series::str_rfind>(self, arg, "str_rfind");
}
PyObject* Series_str_remove_prefix(PyObject* self, PyObject* arg) {
    return str_arg_op<&Series::str_remove_prefix>(self, arg,
                                                  "str_remove_prefix");
}
PyObject* Series_str_remove_suffix(PyObject* self, PyObject* arg) {
    return str_arg_op<&Series::str_remove_suffix>(self, arg,
                                                  "str_remove_suffix");
}
PyObject* Series_str_findall(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    std::string_view pattern;
    if (!as_str_view(arg, &pattern)) return nullptr;
    Series out = a->str_findall(pattern);
    if (!out.valid()) {
        PyErr_SetString(PyExc_ValueError,
                        "str_findall: not a String column, or the pattern "
                        "does not compile");
        return nullptr;
    }
    return make_series(std::move(out));
}
PyObject* Series_list_join(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    std::string_view sep;
    if (!as_str_view(arg, &sep)) return nullptr;
    Series out = a->list_join(sep);
    if (!out.valid()) {
        PyErr_SetString(PyExc_TypeError,
                        "list_join: not a List column of strings");
        return nullptr;
    }
    return make_series(std::move(out));
}
PyObject* Series_str_repeat(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    const long long n = PyLong_AsLongLong(arg);
    if (n == -1 && PyErr_Occurred()) return nullptr;
    if (n < 0) {
        PyErr_SetString(PyExc_ValueError, "str_repeat: n must be >= 0");
        return nullptr;
    }
    Series out = a->str_repeat(static_cast<std::int64_t>(n));
    if (!out.valid()) {
        PyErr_SetString(PyExc_TypeError, "str_repeat: not a String column");
        return nullptr;
    }
    return make_series(std::move(out));
}
PyObject* Series_str_center(PyObject* self, PyObject* args, PyObject* kwds) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long width = 0;
    PyObject* fill = nullptr;
    static const char* kwlist[] = {"width", "fill", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "L|O",
                                     const_cast<char**>(kwlist), &width, &fill))
        return nullptr;
    char f = ' ';
    if (fill && !as_fill_char(fill, &f)) return nullptr;
    Series out = a->str_center(static_cast<std::int64_t>(width), f);
    if (!out.valid()) {
        PyErr_SetString(PyExc_TypeError, "str_center: not a String column");
        return nullptr;
    }
    return make_series(std::move(out));
}
PyObject* Series_str_cat(PyObject* self, PyObject* other) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    Series* b = as_series(other);
    if (!b) return nullptr;
    Series out = a->str_cat(*b);
    if (!out.valid()) {
        PyErr_SetString(PyExc_TypeError,
                        "str_cat: both must be String columns of one length");
        return nullptr;
    }
    return make_series(std::move(out));
}
PyObject* Series_str_partition(PyObject* self, PyObject* args, PyObject* kwds) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    PyObject* sep_obj = nullptr;
    int from_right = 0;
    static const char* kwlist[] = {"sep", "from_right", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|p",
                                     const_cast<char**>(kwlist), &sep_obj,
                                     &from_right))
        return nullptr;
    std::string_view sep;
    if (!as_str_view(sep_obj, &sep)) return nullptr;
    if (sep.empty()) {
        PyErr_SetString(PyExc_ValueError, "str_partition: empty separator");
        return nullptr;
    }
    Series out = a->str_partition(sep, from_right != 0);
    if (!out.valid()) {
        PyErr_SetString(PyExc_TypeError, "str_partition: not a String column");
        return nullptr;
    }
    return make_series(std::move(out));
}

PyObject* Series_dt_part(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    int part = 0;
    int unit = DFTU_TIME_UNIT_MICRO;
    if (!PyArg_ParseTuple(args, "i|i", &part, &unit)) return nullptr;
    if (part < DFTU_DT_YEAR || part > DFTU_DT_EPOCH_DAYS) {
        PyErr_SetString(PyExc_ValueError, "dt_part: code must be 0..16");
        return nullptr;
    }
    if (unit < DFTU_TIME_UNIT_SECOND || unit > DFTU_TIME_UNIT_NANO) {
        PyErr_SetString(PyExc_ValueError, "dt_part: unit must be 0..3");
        return nullptr;
    }
    Series out = a->dt_part(part, unit);
    if (!out.valid()) {
        PyErr_SetString(PyExc_TypeError,
                        "dt_part: not a Timestamp, Date, Duration or Int64 "
                        "column");
        return nullptr;
    }
    return make_series(std::move(out));
}
PyObject* Series_dt_round(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long every = 0;
    int mode = 0;
    if (!PyArg_ParseTuple(args, "Li", &every, &mode)) return nullptr;
    if (every <= 0) {
        PyErr_SetString(PyExc_ValueError, "dt_round: every must be positive");
        return nullptr;
    }
    if (mode < DFTU_DT_FLOOR || mode > DFTU_DT_ROUND) {
        PyErr_SetString(PyExc_ValueError,
                        "dt_round: mode must be 0 (floor), 1 (ceil) or 2 "
                        "(round)");
        return nullptr;
    }
    Series out = a->dt_round(static_cast<std::int64_t>(every), mode);
    if (!out.valid()) {
        PyErr_SetString(PyExc_TypeError,
                        "dt_round: not a Timestamp, Duration or Int64 column");
        return nullptr;
    }
    return make_series(std::move(out));
}

PyObject* Series_with_timezone(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    std::string_view tz;
    if (!as_str_view(arg, &tz)) return nullptr;
    Series out = a->with_timezone(tz);
    if (!out.valid()) {
        PyErr_SetString(PyExc_TypeError,
                        "with_timezone: not a Timestamp column");
        return nullptr;
    }
    return make_series(std::move(out));
}

}  // namespace dftracer::utils::python::series_detail

#endif  // DFTRACER_UTILS_ENABLE_ARROW
