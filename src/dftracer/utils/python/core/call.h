// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_CORE_CALL_H
#define DFTRACER_UTILS_PYTHON_CORE_CALL_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/python/core/errors.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <type_traits>

#ifndef DFTRACER_UTILS_PYTHON_GIL_RELEASE_ROWS
#define DFTRACER_UTILS_PYTHON_GIL_RELEASE_ROWS 16384
#endif

namespace dftracer::utils::python::core {

/// Row count from which @ref without_gil releases the GIL. Below it the
/// release and reacquire cost more than the call; benchmarks/python/
/// gil_bench.py sets it. The macro lets that benchmark build the extension
/// with the release always or never on.
inline constexpr std::int64_t GIL_RELEASE_ROWS =
    DFTRACER_UTILS_PYTHON_GIL_RELEASE_ROWS;

namespace detail {

template <std::size_t N>
struct FormatString {
    char text[N]{};
    consteval FormatString(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) text[i] = s[i];
    }
};

/// What a PyArg format consumes: `keywords` Python arguments (one per unit) and
/// `pointers` C arguments (`O!`, `O&`, `s#`, `y#` and `z#` take two for one
/// keyword).
struct FormatCount {
    std::size_t keywords = 0;
    std::size_t pointers = 0;
};

/// Counts a format. Rejects the units that this parser does not count: nested
/// tuples and the `e` encoding units.
template <std::size_t N>
consteval FormatCount count_format(const FormatString<N> &fmt) {
    FormatCount count;
    for (std::size_t i = 0; i + 1 < N; ++i) {
        const char c = fmt.text[i];
        if (c == ':' || c == ';') break;
        if (c == '(' || c == ')' || c == 'e')
            throw "unsupported PyArg format unit";
        if (c == '|' || c == '$' || c == '*') continue;
        if (c == '!' || c == '&' || c == '#') {
            ++count.pointers;
            continue;
        }
        ++count.keywords;
        ++count.pointers;
    }
    return count;
}

static_assert(count_format(FormatString("O!")).keywords == 1);
static_assert(count_format(FormatString("O!")).pointers == 2);
static_assert(count_format(FormatString("O&")).pointers == 2);
static_assert(count_format(FormatString("s#")).keywords == 1);
static_assert(count_format(FormatString("z#")).pointers == 2);
static_assert(count_format(FormatString("y#")).pointers == 2);
static_assert(count_format(FormatString("s*")).pointers == 1);
static_assert(count_format(FormatString("s|O!z#:name")).keywords == 3);
static_assert(count_format(FormatString("s|O!z#:name")).pointers == 5);
static_assert(count_format(FormatString("|$Lp")).keywords == 2);

}  // namespace detail

namespace detail {

/// The function name of a format, the text after its `:`; "function" when it
/// has none.
template <std::size_t N>
constexpr const char *format_name(const FormatString<N> &fmt) {
    for (std::size_t i = 0; i + 1 < N; ++i)
        if (fmt.text[i] == ':') return fmt.text + i + 1;
    return "function";
}

/// Sets the TypeError Python gives for a keyword that @p names does not hold.
/// Returns false with the error set; does nothing when @p kw is null.
inline bool check_keywords(const char *func, PyObject *kw,
                           const char *const *names, std::size_t count) {
    if (!kw) return true;
    PyObject *key = nullptr;
    PyObject *value = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(kw, &pos, &key, &value)) {
        bool known = false;
        if (PyUnicode_Check(key))
            for (std::size_t i = 0; i < count && !known; ++i)
                known = PyUnicode_CompareWithASCIIString(key, names[i]) == 0;
        if (!known) {
            PyErr_Format(PyExc_TypeError,
                         "%s() got an unexpected keyword argument '%U'", func,
                         key);
            return false;
        }
    }
    return true;
}

}  // namespace detail

/// Parses a call's arguments with PyArg_ParseTupleAndKeywords. @p Fmt is the
/// format string, @p names the keyword of each unit in order, @p out one
/// pointer per C argument. The count of names and of pointers is checked
/// against the format at compile time (`O!`, `O&`, `s#`, `y#` and `z#` are one
/// name and two pointers). A keyword that is not in @p names raises "<func>()
/// got an unexpected keyword argument '<name>'", where func is the text after
/// the `:` of @p Fmt, or "function". Returns false with a Python error set.
template <detail::FormatString Fmt, std::size_t N, class... Out>
bool parse_kw_for(const char *func, PyObject *args, PyObject *kw,
                  const char *const (&names)[N], Out *...out) {
    constexpr detail::FormatCount COUNT = detail::count_format(Fmt);
    static_assert(N == COUNT.keywords, "keyword names do not match the format");
    static_assert(sizeof...(Out) == COUNT.pointers,
                  "output pointers do not match the format");
    if (!detail::check_keywords(func, kw, names, N)) return false;
    char *kwlist[N + 1];
    for (std::size_t i = 0; i < N; ++i)
        kwlist[i] = const_cast<char *>(names[i]);
    kwlist[N] = nullptr;
    return PyArg_ParseTupleAndKeywords(args, kw, Fmt.text, kwlist, out...) != 0;
}

/// @ref parse_kw_for with the function name of the format.
template <detail::FormatString Fmt, std::size_t N, class... Out>
bool parse_kw(PyObject *args, PyObject *kw, const char *const (&names)[N],
              Out *...out) {
    return parse_kw_for<Fmt>(detail::format_name(Fmt), args, kw, names, out...);
}

/// A method whose body takes its one argument as a METH_O does, called with
/// the argument by position or by the keyword @p Name.
template <detail::FormatString Name, PyObject *(*Impl)(PyObject *, PyObject *)>
PyObject *kw_one(PyObject *self, PyObject *args, PyObject *kwds) {
    PyObject *arg = nullptr;
    const char *const names[1] = {Name.text};
    if (!parse_kw<"O">(args, kwds, names, &arg)) return nullptr;
    return Impl(self, arg);
}

/// A method whose body takes a METH_VARARGS tuple of one required and one
/// optional argument, called by position or by the keywords @p First and
/// @p Second. The body sees the tuple it always saw.
template <PyObject *(*Impl)(PyObject *, PyObject *), detail::FormatString First,
          detail::FormatString Second>
PyObject *kw_pair(PyObject *self, PyObject *args, PyObject *kwds) {
    PyObject *given[2] = {nullptr, nullptr};
    const char *const names[2] = {First.text, Second.text};
    if (!parse_kw<"O|O">(args, kwds, names, &given[0], &given[1]))
        return nullptr;
    PyObject *tuple = given[1] ? PyTuple_Pack(2, given[0], given[1])
                               : PyTuple_Pack(1, given[0]);
    if (!tuple) return nullptr;
    PyObject *out = Impl(self, tuple);
    Py_DECREF(tuple);
    return out;
}

/// Runs @p fn, which returns a pointer, an int status or a bool, and turns a
/// C++ exception into the matching typed Python error. The failure value is
/// nullptr, -1 or false. An @ref PyErrorException keeps its error domain; see
/// @ref set_typed_py_error for the full map.
template <class Fn>
auto guard(Fn &&fn) -> decltype(fn()) {
    using R = decltype(fn());
    static_assert(
        std::is_pointer_v<R> || std::is_same_v<R, int> ||
            std::is_same_v<R, bool>,
        "guard wraps a body that returns a pointer, an int or a bool");
    try {
        return fn();
    } catch (const std::exception &e) {
        set_typed_py_error(e);
    } catch (...) {
        PyErr_SetString(g_dft_error, "unknown C++ exception");
    }
    if constexpr (std::is_pointer_v<R>) {
        return nullptr;
    } else if constexpr (std::is_same_v<R, bool>) {
        return false;
    } else {
        return -1;
    }
}

/// Like @ref guard, but every C++ exception becomes @p type, for a call whose
/// contract names one Python exception type for all its failures.
template <class Fn>
auto guard_as(PyObject *type, Fn &&fn) -> decltype(fn()) {
    using R = decltype(fn());
    static_assert(std::is_pointer_v<R>,
                  "guard_as wraps a body that returns a pointer");
    try {
        return fn();
    } catch (const std::exception &e) {
        PyErr_SetString(type, e.what());
    } catch (...) {
        PyErr_SetString(type, "unknown C++ exception");
    }
    return nullptr;
}

/// Runs @p fn with the GIL released. The body must not touch a Python object
/// or call the Python C API, and it must not leave a borrowed reference to one
/// that another thread can drop: copy what it needs before the call. The GIL
/// is taken back on every path, including an exception. Use it where the body
/// can run Python code on another thread, which needs the GIL.
template <class Fn>
auto release_gil(Fn &&fn) -> decltype(fn()) {
    struct Released {
        PyThreadState *state = PyEval_SaveThread();
        ~Released() { PyEval_RestoreThread(state); }
    } released;
    return fn();
}

/// Like @ref release_gil, but only when @p rows is at least
/// @ref GIL_RELEASE_ROWS; below it the release costs more than the call. The
/// same rules for the body apply.
template <class Fn>
auto without_gil(std::int64_t rows, Fn &&fn) -> decltype(fn()) {
    if (rows < GIL_RELEASE_ROWS) return fn();
    return release_gil(fn);
}

/// How a native type is found behind a PyObject. A specialization gives the
/// type object, the name for the TypeError and the projection from the
/// object to T.
template <class T>
struct NativeTraits;

/// The T behind @p o, or nullptr with a TypeError set when @p o is not that
/// native type.
template <class T>
T *as_native(PyObject *o) {
    if (!PyObject_TypeCheck(o, NativeTraits<T>::type())) {
        PyErr_SetString(PyExc_TypeError, NativeTraits<T>::EXPECTED);
        return nullptr;
    }
    return NativeTraits<T>::get(o);
}

/// Like @ref as_native, but nullptr with no error set.
template <class T>
T *as_native_or_null(PyObject *o) {
    if (!PyObject_TypeCheck(o, NativeTraits<T>::type())) return nullptr;
    return NativeTraits<T>::get(o);
}

}  // namespace dftracer::utils::python::core

#endif  // DFTRACER_UTILS_PYTHON_CORE_CALL_H
