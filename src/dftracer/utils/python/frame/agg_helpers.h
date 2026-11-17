// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_FRAME_AGG_HELPERS_H
#define DFTRACER_UTILS_PYTHON_FRAME_AGG_HELPERS_H

#include <Python.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/python/core/seq_helpers.h>

#include <stdexcept>
#include <string>

namespace dftracer::utils::python::frame {

// A leading double of `text` as std::stod reads it: optional space and '+',
// then a number. Throws std::invalid_argument when there is none.
inline double parse_double(dftracer::utils::dstd::StringView text) {
    const char* first = text.data();
    const char* last = first + text.size();
    while (first < last &&
           (*first == ' ' || (*first >= '\t' && *first <= '\r')))
        ++first;
    if (first < last && *first == '+') ++first;
    double value = 0.0;
    if (dftracer::utils::dstd::from_chars(
            {first, static_cast<std::size_t>(last - first)}, value)
            .ec != std::errc{})
        throw std::invalid_argument("stod: no conversion");
    return value;
}

// Parse an "op[:column[:out[:by[:param]]]]" aggregate spec (the output name
// defaults to "op" or "op_column"; `by` is the second input column of the
// two-column aggregates, `param` the quantile / k of the parameterized ones);
// throws std::invalid_argument on a bad op or a bad param.
inline dftracer::utils::dataframe::GroupAgg group_agg_from_spec(
    const dftracer::utils::dstd::String& spec) {
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t colon = spec.find(':', start);
        parts.push_back(
            spec.substr(start, colon == dftracer::utils::dstd::String::npos
                                   ? dftracer::utils::dstd::String::npos
                                   : colon - start));
        if (colon == dftracer::utils::dstd::String::npos) break;
        start = colon + 1;
    }
    dataframe::GroupAgg a;
    const auto op = dataframe::agg_from_string(parts[0]);
    if (!op) throw std::invalid_argument(op.error().c_str());
    a.op = *op;
    if (parts.size() == 1) {
        a.out = parts[0];
        return a;
    }
    a.column = parts[1];
    a.out = parts.size() > 2
                ? parts[2]
                : dftracer::utils::dstd::fmt<"{}_{}">(parts[0], a.column);
    if (parts.size() > 3) a.by = parts[3];
    if (parts.size() > 4 && !parts[4].empty()) a.param = parse_double(parts[4]);
    return a;
}

// The "op:column:out" spec strings of a broadcast, for the Python GroupBy
// to hand back to the string group_by.
inline dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>
reduce_spec_strings(
    const dftracer::utils::dstd::Vector<dftracer::utils::dataframe::GroupAgg>&
        aggs) {
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> out;
    out.reserve(aggs.size());
    for (const auto& a : aggs)
        out.push_back(dftracer::utils::dstd::fmt<"{}:{}:{}">(
            dftracer::utils::dataframe::to_string(a.op), a.column, a.out));
    return out;
}

// Parse a UDAF spec (name, out, [column, ...]) into a GroupAgg: `column` is the
// first argument and `args` the others.
inline bool udaf_agg_from_tuple(PyObject* item,
                                dftracer::utils::dataframe::GroupAgg& out) {
    if (PyTuple_GET_SIZE(item) != 3 ||
        !PyList_Check(PyTuple_GET_ITEM(item, 2))) {
        PyErr_SetString(PyExc_TypeError,
                        "a udaf spec is (name, out, [column, ...])");
        return false;
    }
    PyObject* columns = PyTuple_GET_ITEM(item, 2);
    const char* fn = PyUnicode_AsUTF8(PyTuple_GET_ITEM(item, 0));
    const char* name =
        fn ? PyUnicode_AsUTF8(PyTuple_GET_ITEM(item, 1)) : nullptr;
    if (!name) return false;
    out.op = dftracer::utils::dataframe::Agg::Udaf;
    out.fn = fn;
    out.out = name;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(columns); ++i) {
        const char* c = PyUnicode_AsUTF8(PyList_GET_ITEM(columns, i));
        if (!c) return false;
        if (i == 0)
            out.column = c;
        else
            out.args.emplace_back(c);
    }
    return true;
}

// Parse a sequence of agg specs into GroupAgg records: a string ("count" or
// "<op>:<column>"), mirroring the group_by string-spec form, or a UDAF tuple.
inline bool aggs_from_seq(
    PyObject* obj,
    dftracer::utils::dstd::Vector<dftracer::utils::dataframe::GroupAgg>& out) {
    return core::parse_seq<dftracer::utils::dataframe::GroupAgg>(
        obj, "aggs must be a sequence of str or udaf tuples", out,
        [](PyObject* item,
           dftracer::utils::dstd::Vector<dftracer::utils::dataframe::GroupAgg>&
               o) {
            if (PyTuple_Check(item)) {
                dftracer::utils::dataframe::GroupAgg a;
                if (!udaf_agg_from_tuple(item, a)) return false;
                o.push_back(std::move(a));
                return true;
            }
            const char* s = PyUnicode_AsUTF8(item);
            if (!s) return false;
            return core::guard([&] {
                o.push_back(group_agg_from_spec(s));
                return true;
            });
        });
}

// Parse a group_by key argument that is either one column name or a sequence
// of names (native multi-key entry point). Appends to `out`.
inline bool strings_from_str_or_seq(
    PyObject* obj,
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& out) {
    if (PyUnicode_Check(obj)) {
        const char* s = PyUnicode_AsUTF8(obj);
        if (!s) return false;
        out.emplace_back(s);
        return true;
    }
    return core::parse_string_seq(obj, "key must be a str or a sequence of str",
                                  out);
}

}  // namespace dftracer::utils::python::frame

#endif  // DFTRACER_UTILS_PYTHON_FRAME_AGG_HELPERS_H
