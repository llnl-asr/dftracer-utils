#ifndef DFTRACER_UTILS_PYTHON_PY_FRAME_OP_HELPERS_H
#define DFTRACER_UTILS_PYTHON_PY_FRAME_OP_HELPERS_H

#include <Python.h>
#include <dftracer/utils/dataframe/abi.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace dftracer::utils::python {

/// The window specs of one window() call in their C ABI shape, with the
/// column names they point at owned here (a deque keeps them stable).
struct WindowSpecs {
    std::deque<std::string> names;
    std::vector<dftu_window_spec> specs;
};

inline bool window_func_from_str(const char* name, dftu_window_func* out) {
    struct Entry {
        const char* name;
        dftu_window_func func;
    };
    static const Entry table[] = {{"row_number", DFTU_WINDOW_ROW_NUMBER},
                                  {"rank", DFTU_WINDOW_RANK},
                                  {"dense_rank", DFTU_WINDOW_DENSE_RANK},
                                  {"lag", DFTU_WINDOW_LAG},
                                  {"lead", DFTU_WINDOW_LEAD},
                                  {"running_sum", DFTU_WINDOW_RUNNING_SUM},
                                  {"running_min", DFTU_WINDOW_RUNNING_MIN},
                                  {"running_max", DFTU_WINDOW_RUNNING_MAX},
                                  {"running_count", DFTU_WINDOW_RUNNING_COUNT},
                                  {"delta", DFTU_WINDOW_DELTA},
                                  {"rate", DFTU_WINDOW_RATE},
                                  {"sessionize", DFTU_WINDOW_SESSIONIZE},
                                  {"frame_sum", DFTU_WINDOW_FRAME_SUM},
                                  {"frame_min", DFTU_WINDOW_FRAME_MIN},
                                  {"frame_max", DFTU_WINDOW_FRAME_MAX},
                                  {"frame_count", DFTU_WINDOW_FRAME_COUNT},
                                  {"frame_mean", DFTU_WINDOW_FRAME_MEAN},
                                  {"ntile", DFTU_WINDOW_NTILE},
                                  {"first_value", DFTU_WINDOW_FIRST_VALUE},
                                  {"last_value", DFTU_WINDOW_LAST_VALUE},
                                  {"nth_value", DFTU_WINDOW_NTH_VALUE},
                                  {"fill_forward", DFTU_WINDOW_FILL_FORWARD},
                                  {"running_prod", DFTU_WINDOW_RUNNING_PROD}};
    for (const auto& e : table)
        if (std::strcmp(e.name, name) == 0) {
            *out = e.func;
            return true;
        }
    PyErr_Format(PyExc_ValueError, "window: unknown function '%s'", name);
    return false;
}

/// Parse the sequence of 9-tuples the Python wrapper normalizes a window()
/// call into: (func, value_col|None, offset, name, time_col|None, threshold,
/// counter, frame_preceding, frame_following). Returns false with a Python
/// error set.
inline bool parse_window_specs(PyObject* seq, WindowSpecs& out) {
    PyObject* fast = PySequence_Fast(seq, "window: specs must be a sequence");
    if (!fast) return false;
    const Py_ssize_t n = PySequence_Fast_GET_SIZE(fast);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* t = PySequence_Fast_GET_ITEM(fast, i);
        if (!PyTuple_Check(t)) {
            Py_DECREF(fast);
            PyErr_SetString(PyExc_TypeError,
                            "window: each spec must be a 9-tuple");
            return false;
        }
        const char* func = nullptr;
        PyObject* value_obj = nullptr;
        Py_ssize_t offset = 0;
        const char* name = nullptr;
        PyObject* time_obj = nullptr;
        double threshold = 0.0;
        int counter = 0;
        Py_ssize_t frame_pre = 0;
        Py_ssize_t frame_post = 0;
        if (!PyArg_ParseTuple(t, "sOnsOdpnn", &func, &value_obj, &offset, &name,
                              &time_obj, &threshold, &counter, &frame_pre,
                              &frame_post)) {
            Py_DECREF(fast);
            return false;
        }
        dftu_window_spec s{};
        if (!window_func_from_str(func, &s.func)) {
            Py_DECREF(fast);
            return false;
        }
        if (value_obj != Py_None) {
            const char* vc = PyUnicode_AsUTF8(value_obj);
            if (!vc) {
                Py_DECREF(fast);
                return false;
            }
            s.value = out.names.emplace_back(vc).c_str();
        }
        if (time_obj != Py_None) {
            const char* tc = PyUnicode_AsUTF8(time_obj);
            if (!tc) {
                Py_DECREF(fast);
                return false;
            }
            s.time = out.names.emplace_back(tc).c_str();
        }
        s.out = out.names.emplace_back(name).c_str();
        s.offset = static_cast<std::int64_t>(offset);
        s.threshold = threshold;
        s.counter = counter;
        s.preceding = static_cast<std::int64_t>(frame_pre);
        s.following = static_cast<std::int64_t>(frame_post);
        out.specs.push_back(s);
    }
    Py_DECREF(fast);
    return true;
}

inline bool gap_fill_mode_from_str(const char* mode, dftu_gap_fill_mode* out) {
    if (std::strcmp(mode, "none") == 0)
        *out = DFTU_GAP_FILL_NONE;
    else if (std::strcmp(mode, "locf") == 0)
        *out = DFTU_GAP_FILL_LOCF;
    else if (std::strcmp(mode, "linear") == 0)
        *out = DFTU_GAP_FILL_LINEAR;
    else {
        PyErr_Format(PyExc_ValueError, "gap_fill: unknown mode '%s'", mode);
        return false;
    }
    return true;
}

inline bool asof_direction_from_str(const char* direction,
                                    dftu_asof_direction* out) {
    if (std::strcmp(direction, "backward") == 0)
        *out = DFTU_ASOF_BACKWARD;
    else if (std::strcmp(direction, "forward") == 0)
        *out = DFTU_ASOF_FORWARD;
    else if (std::strcmp(direction, "nearest") == 0)
        *out = DFTU_ASOF_NEAREST;
    else {
        PyErr_Format(PyExc_ValueError, "asof: unknown direction '%s'",
                     direction);
        return false;
    }
    return true;
}

inline bool groupwise_op_from_str(const char* name, dftu_groupwise_op* out) {
    struct Entry {
        const char* name;
        dftu_groupwise_op op;
    };
    static const Entry table[] = {
        {"cumsum", DFTU_GROUPWISE_CUMSUM},
        {"cummax", DFTU_GROUPWISE_CUMMAX},
        {"cummin", DFTU_GROUPWISE_CUMMIN},
        {"cumcount", DFTU_GROUPWISE_CUMCOUNT},
        {"shift", DFTU_GROUPWISE_SHIFT},
        {"diff", DFTU_GROUPWISE_DIFF},
        {"pct_change", DFTU_GROUPWISE_PCT_CHANGE},
        {"rank", DFTU_GROUPWISE_RANK},
        {"ngroup", DFTU_GROUPWISE_NGROUP},
        {"head", DFTU_GROUPWISE_HEAD},
        {"tail", DFTU_GROUPWISE_TAIL},
        {"nth", DFTU_GROUPWISE_NTH},
        {"ffill", DFTU_GROUPWISE_FFILL},
        {"bfill", DFTU_GROUPWISE_BFILL},
        {"rolling_sum", DFTU_GROUPWISE_ROLLING_SUM},
        {"rolling_mean", DFTU_GROUPWISE_ROLLING_MEAN},
        {"rolling_min", DFTU_GROUPWISE_ROLLING_MIN},
        {"rolling_max", DFTU_GROUPWISE_ROLLING_MAX},
        {"cumprod", DFTU_GROUPWISE_CUMPROD},
    };
    for (const Entry& e : table)
        if (std::strcmp(name, e.name) == 0) {
            *out = e.op;
            return true;
        }
    PyErr_Format(PyExc_ValueError, "group_by: unknown transform '%s'", name);
    return false;
}

// The pandas rank methods; `first` is the engine's ordinal rank.
inline bool rank_method_from_str(const char* name, dftu_rank_method* out) {
    if (std::strcmp(name, "average") == 0)
        *out = DFTU_RANK_AVERAGE;
    else if (std::strcmp(name, "min") == 0)
        *out = DFTU_RANK_MIN;
    else if (std::strcmp(name, "dense") == 0)
        *out = DFTU_RANK_DENSE;
    else if (std::strcmp(name, "first") == 0 ||
             std::strcmp(name, "ordinal") == 0)
        *out = DFTU_RANK_ORDINAL;
    else if (std::strcmp(name, "max") == 0)
        *out = DFTU_RANK_MAX;
    else {
        PyErr_SetString(
            PyExc_ValueError,
            "rank: method must be average, min, max, dense or first");
        return false;
    }
    return true;
}

// group_transform(keys, kind, n=0, method="average", ascending=True): the
// shared argument parse of the DataFrame and LazyFrame bindings.
struct GroupwiseArgs {
    std::vector<std::string> keys;
    dftu_groupwise_op kind = DFTU_GROUPWISE_CUMSUM;
    std::int64_t n = 0;
    dftu_rank_method method = DFTU_RANK_AVERAGE;
    int ascending = 1;
};

template <class Names>
bool parse_groupwise_args(PyObject* args, PyObject* kwds, Names names_from_obj,
                          GroupwiseArgs& out) {
    static const char* kw[] = {"keys",   "kind",      "n",
                               "method", "ascending", nullptr};
    PyObject* keys_obj = nullptr;
    const char* kind = nullptr;
    const char* method = "average";
    long long n = 0;
    int ascending = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "Os|Lsp",
                                     const_cast<char**>(kw), &keys_obj, &kind,
                                     &n, &method, &ascending))
        return false;
    if (!names_from_obj(keys_obj, out.keys)) return false;
    if (!groupwise_op_from_str(kind, &out.kind)) return false;
    if (!rank_method_from_str(method, &out.method)) return false;
    out.n = static_cast<std::int64_t>(n);
    out.ascending = ascending;
    return true;
}

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_PY_FRAME_OP_HELPERS_H
