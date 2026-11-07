// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_PY_AGG_CONFIG_H
#define DFTRACER_UTILS_PYTHON_PY_AGG_CONFIG_H

#include <Python.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/python/py_str_helpers.h>

namespace dftracer::utils::python {

/// The aggregation config a Python AggregationConfig describes; a missing or
/// unreadable attribute keeps the standard value. Builders and the coordinator
/// of a distributed build use it alike, so they agree on the params hash.
inline index::schemas::dft::agg::AggregationConfig agg_config_from_py(
    PyObject *obj) {
    index::schemas::dft::agg::AggregationConfig cfg =
        index::schemas::dft::agg::standard_aggregation_config();
    auto attr = [obj](const char *name) -> PyObject * {
        PyObject *v = PyObject_GetAttrString(obj, name);
        if (!v || v == Py_None) {
            Py_XDECREF(v);
            PyErr_Clear();
            return nullptr;
        }
        return v;
    };
    auto pull_double = [&](const char *name, double fallback) {
        PyObject *v = attr(name);
        if (!v) return fallback;
        double out = PyFloat_AsDouble(v);
        Py_DECREF(v);
        if (out == -1.0 && PyErr_Occurred()) {
            PyErr_Clear();
            return fallback;
        }
        return out;
    };
    auto pull_bool = [&](const char *name, bool fallback) {
        PyObject *v = attr(name);
        if (!v) return fallback;
        int out = PyObject_IsTrue(v);
        Py_DECREF(v);
        if (out < 0) {
            PyErr_Clear();
            return fallback;
        }
        return out > 0;
    };
    auto pull_strings = [&](const char *name) {
        dstd::Vector<dstd::String> out;
        PyObject *v = attr(name);
        if (!v) return out;
        PyObject *seq = PySequence_Fast(v, "expected list of str");
        Py_DECREF(v);
        if (!seq) {
            PyErr_Clear();
            return out;
        }
        Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
        out.reserve(static_cast<std::size_t>(n));
        for (Py_ssize_t i = 0; i < n; ++i)
            if (const char *s = as_utf8(PySequence_Fast_GET_ITEM(seq, i)))
                out.emplace_back(s);
        Py_DECREF(seq);
        return out;
    };
    cfg.time_interval_us = static_cast<std::uint64_t>(
        pull_double("time_interval_ms",
                    static_cast<double>(cfg.time_interval_us) / 1000.0) *
        1000.0);
    cfg.compute_percentiles =
        pull_bool("compute_percentiles", cfg.compute_percentiles);
    cfg.group_by_file = pull_bool("group_by_file", cfg.group_by_file);
    cfg.extra_group_keys = pull_strings("group_keys");
    return cfg;
}

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_PY_AGG_CONFIG_H
