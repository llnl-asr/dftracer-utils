// A native vec batch exposed to Python: a named set of dataframe::Columns (our
// SIMD columnar format) held as a single STRUCT column. Series access,
// filtering, and row ops stay in vec; Arrow/pandas/polars are produced only on
// an explicit to_arrow()/to_pandas()/to_polars(), zero-copy through the Arrow C
// interface.

#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/python/dataframe.h>
#include <dftracer/utils/python/py_frame_op_helpers.h>
#include <dftracer/utils/utilities/common/arrow/frame_ops.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/arrow_bridge.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/kernels/kernels.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/plan.h>
#include <dftracer/utils/python/columnar_eval.h>
#include <dftracer/utils/python/lazyframe.h>
#include <dftracer/utils/python/py_agg_helpers.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_join_helpers.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_scalar_helpers.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/series.h>
#include <dftracer/utils/query/errc.h>
#include <dftracer/utils/query/query.h>

#include <cstdint>
#include <cstring>
#include <nanoarrow/nanoarrow.hpp>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dataframe = dftracer::utils::dataframe;

namespace {

using dataframe::DataFrame;
using dataframe::Series;
using dftracer::utils::python::aggs_from_seq;
using dftracer::utils::python::group_agg_from_spec;
using dftracer::utils::python::join_how_from_str;
using dftracer::utils::python::join_keys_from_objs;
using dftracer::utils::python::parse_int_seq;
using dftracer::utils::python::parse_seq;
using dftracer::utils::python::parse_string_seq;
using dftracer::utils::python::strings_from_str_or_seq;

// A DataFrame owns its columns as one STRUCT column: child(i) hands out a
// column sharing the struct's buffers (O(1)), and the struct exports to Arrow
// in one zero-copy step.
struct DataFrameObject {
    PyObject_HEAD Series st;
    std::vector<std::string> names;
};

PyTypeObject DataFrameType;

DataFrameObject* as_dataframe(PyObject* o) {
    if (!PyObject_TypeCheck(o, &DataFrameType)) {
        PyErr_SetString(PyExc_TypeError, "expected a DataFrame");
        return nullptr;
    }
    return reinterpret_cast<DataFrameObject*>(o);
}

PyObject* make_dataframe(DataFrame&& b) {
    Series st = Series::structs(b.names, std::move(b.columns));
    auto* self = reinterpret_cast<DataFrameObject*>(
        DataFrameType.tp_alloc(&DataFrameType, 0));
    if (!self) return nullptr;
    new (&self->st) Series(std::move(st));
    new (&self->names) std::vector<std::string>(std::move(b.names));
    return reinterpret_cast<PyObject*>(self);
}

void DataFrame_dealloc(DataFrameObject* self) {
    self->names.~vector();
    self->st.~Series();
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

std::int64_t index_of(const DataFrameObject* self, const std::string& name) {
    for (std::size_t i = 0; i < self->names.size(); ++i)
        if (self->names[i] == name) return static_cast<std::int64_t>(i);
    return -1;
}

// A dataframe::DataFrame view over the struct's children (shared buffers,
// zero-copy), for forwarding to the batch_ops frame kernels.
DataFrame to_dataframe(const DataFrameObject* self) {
    DataFrame b;
    b.names = self->names;
    b.columns.reserve(self->names.size());
    for (std::size_t i = 0; i < self->names.size(); ++i)
        b.columns.push_back(self->st.child(static_cast<std::int64_t>(i)));
    return b;
}

// Wrap a batch_ops call that may throw, turning C++ exceptions into Python
// errors.
template <class Fn>
PyObject* run_batch_op(Fn&& fn) {
    try {
        return make_dataframe(fn());
    } catch (const std::out_of_range& e) {
        PyErr_SetString(PyExc_KeyError, e.what());
        return nullptr;
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
        return nullptr;
    }
}

PyObject* DataFrame_subscript(PyObject* self, PyObject* key) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    const char* name = PyUnicode_AsUTF8(key);
    if (!name) {
        PyErr_SetString(PyExc_TypeError, "DataFrame key must be a column name");
        return nullptr;
    }
    std::int64_t i = index_of(b, name);
    if (i < 0) {
        PyErr_Format(PyExc_KeyError, "no column named '%s'", name);
        return nullptr;
    }
    return dftracer::utils::python::wrap_vec_column(b->st.child(i));
}

int DataFrame_contains(PyObject* self, PyObject* key) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return -1;
    // Only a name can be a column; anything else is simply not in the
    // frame, with no error left behind.
    if (!PyUnicode_Check(key)) return 0;
    const char* name = PyUnicode_AsUTF8(key);
    if (!name) return -1;
    return index_of(b, name) >= 0 ? 1 : 0;
}

PyObject* DataFrame_keys(PyObject* self, PyObject*) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    PyObject* list = PyList_New(static_cast<Py_ssize_t>(b->names.size()));
    if (!list) return nullptr;
    for (std::size_t i = 0; i < b->names.size(); ++i) {
        PyObject* s = PyUnicode_FromString(b->names[i].c_str());
        if (!s) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), s);
    }
    return list;
}

PyObject* DataFrame_filter(PyObject* self, PyObject* mask) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    const Series* m = dftracer::utils::python::unwrap_vec_column(mask);
    if (!m) {
        PyErr_SetString(PyExc_TypeError,
                        "filter() expects a Series boolean mask");
        return nullptr;
    }
    return run_batch_op([&] { return dataframe::filter(to_dataframe(b), *m); });
}

// query(dsl=None, *, group_by=None, aggs=None, select=None, order_by=None,
// descending=False, limit=None) -> DataFrame: a query plan over this batch -
// filter by the DSL predicate (a SIMD mask, pandas.query-style), group_by/aggs,
// project `select`, sort by `order_by`, cap at `limit`. Raises
// DFTUtilsQueryError on a parse error or a predicate with no columnar lowering
// (regex/like/ordered-string/absent field).
PyObject* DataFrame_query(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    const char* dsl = nullptr;
    const char* group_by = nullptr;
    PyObject* aggs_obj = nullptr;
    PyObject* select_obj = nullptr;
    const char* order_by = nullptr;
    int descending = 0;
    PyObject* limit_obj = nullptr;
    static const char* kw[] = {"dsl",      "group_by",   "aggs",  "select",
                               "order_by", "descending", "limit", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|zzOOzpO", const_cast<char**>(kw), &dsl, &group_by,
            &aggs_obj, &select_obj, &order_by, &descending, &limit_obj))
        return nullptr;

    namespace query = dftracer::utils::query;
    namespace dataframe = dftracer::utils::dataframe;
    try {
        DataFrame bat = to_dataframe(b);
        dataframe::QueryPlan plan;
        std::optional<query::Query> q;  // keep root() alive across execute
        if (dsl && *dsl) {
            auto parsed = query::Query::from_string(dsl);
            if (!parsed)
                throw dftracer::utils::DFTUtilsException(
                    dftracer::utils::make_error(query::QueryErrc::Parse,
                                                parsed.error().format()));
            q.emplace(std::move(*parsed));
            plan.where = &q->root();
        }
        if (group_by && *group_by) plan.group_by = group_by;
        if (aggs_obj && aggs_obj != Py_None) {
            if (!parse_seq<dataframe::GroupAgg>(
                    aggs_obj, "aggs must be a sequence of strings", plan.aggs,
                    [](PyObject* item, std::vector<dataframe::GroupAgg>& o) {
                        const char* s = PyUnicode_AsUTF8(item);
                        if (!s) return false;
                        o.push_back(group_agg_from_spec(s));
                        return true;
                    }))
                return nullptr;
        }
        if (select_obj && select_obj != Py_None) {
            if (!parse_string_seq(select_obj,
                                  "select must be a sequence of column names",
                                  plan.select))
                return nullptr;
        }
        if (order_by && *order_by) plan.order_by = order_by;
        plan.descending = descending != 0;
        if (limit_obj && limit_obj != Py_None) {
            plan.limit = PyLong_AsLongLong(limit_obj);
            if (plan.limit == -1 && PyErr_Occurred()) return nullptr;
        }
        return make_dataframe(dataframe::execute(plan, bat));
    } catch (const std::exception& e) {
        dftracer::utils::python::set_typed_py_error(e);
        return nullptr;
    }
}

PyObject* DataFrame_select(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    std::vector<std::string> names;
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    for (Py_ssize_t i = 0; i < n; ++i) {
        const char* s = PyUnicode_AsUTF8(PyTuple_GET_ITEM(args, i));
        if (!s) {
            PyErr_SetString(PyExc_TypeError, "select() names must be strings");
            return nullptr;
        }
        names.emplace_back(s);
    }
    return run_batch_op(
        [&] { return dataframe::select(to_dataframe(b), names); });
}

PyObject* DataFrame_rename(PyObject* self, PyObject* mapping) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    if (!PyDict_Check(mapping)) {
        PyErr_SetString(PyExc_TypeError, "rename() expects a {old: new} dict");
        return nullptr;
    }
    std::vector<std::string> new_names;
    new_names.reserve(b->names.size());
    for (const std::string& name : b->names) {
        PyObject* v = PyDict_GetItemString(mapping, name.c_str());
        if (v) {
            const char* s = PyUnicode_AsUTF8(v);
            if (!s) {
                PyErr_SetString(PyExc_TypeError, "rename() values must be str");
                return nullptr;
            }
            new_names.emplace_back(s);
        } else {
            new_names.push_back(name);
        }
    }
    return run_batch_op(
        [&] { return dataframe::rename(to_dataframe(b), new_names); });
}

PyObject* DataFrame_with_column(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    const char* name = nullptr;
    PyObject* col = nullptr;
    if (!PyArg_ParseTuple(args, "sO", &name, &col)) return nullptr;
    const Series* c = dftracer::utils::python::unwrap_vec_column(col);
    if (!c) {
        PyErr_SetString(PyExc_TypeError,
                        "with_column() expects (name, Series)");
        return nullptr;
    }
    return run_batch_op(
        [&] { return dataframe::with_column(to_dataframe(b), name, *c); });
}

PyObject* DataFrame_take(PyObject* self, PyObject* seq) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    std::vector<std::int64_t> idx;
    if (!parse_int_seq(seq, "take() expects a sequence of ints", idx))
        return nullptr;
    return run_batch_op([&] { return to_dataframe(b).take(idx); });
}

PyObject* DataFrame_tail(PyObject* self, PyObject* arg) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    long long n = PyLong_AsLongLong(arg);
    if (n == -1 && PyErr_Occurred()) return nullptr;
    return run_batch_op([&] { return dataframe::tail(to_dataframe(b), n); });
}

PyObject* DataFrame_reverse(PyObject* self, PyObject*) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    return run_batch_op([&] { return dataframe::reverse(to_dataframe(b)); });
}

PyObject* DataFrame_drop_nulls(PyObject* self, PyObject*) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    return run_batch_op([&] { return dataframe::drop_nulls(to_dataframe(b)); });
}

PyObject* DataFrame_fill_null(PyObject* self, PyObject* value) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    dftu_scalar s{};
    if (!py_to_scalar(value, &s)) return nullptr;
    return run_batch_op(
        [&] { return dataframe::fill_null(to_dataframe(b), s); });
}

// Read a sequence (or single str) of column names into `out`. Returns false and
// sets a Python error on a non-string element.
bool names_from_obj(PyObject* obj, std::vector<std::string>& out) {
    if (PyUnicode_Check(obj)) {
        out.emplace_back(PyUnicode_AsUTF8(obj));
        return true;
    }
    return parse_string_seq(obj, "expected a str or sequence of str", out);
}

// unique(subset=None): subset is a name or a sequence of names; None or an
// empty sequence keys on every column.
PyObject* DataFrame_unique(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    PyObject* subset_obj = Py_None;
    static const char* kwlist[] = {"subset", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O",
                                     const_cast<char**>(kwlist), &subset_obj))
        return nullptr;
    std::vector<std::string> subset;
    if (subset_obj != Py_None && !names_from_obj(subset_obj, subset))
        return nullptr;
    return run_batch_op(
        [&] { return dataframe::unique(to_dataframe(b), subset); });
}

PyObject* DataFrame_sort_by_multi(PyObject* self, PyObject* args,
                                  PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    PyObject* names_obj = nullptr;
    PyObject* descending_obj = nullptr;
    static const char* kwlist[] = {"names", "descending", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O",
                                     const_cast<char**>(kwlist), &names_obj,
                                     &descending_obj))
        return nullptr;
    std::vector<std::string> names;
    if (!parse_string_seq(names_obj, "names must be a sequence", names))
        return nullptr;

    // `descending` is either a single bool (broadcasts) or a sequence of bool,
    // one per name; a bare Python list is never truthy-coerced (that was the
    // old "any non-empty list sorts descending" bug).
    std::vector<bool> descending;
    if (!descending_obj) {
        descending.push_back(false);
    } else if (PyBool_Check(descending_obj) || PyLong_Check(descending_obj)) {
        descending.push_back(PyObject_IsTrue(descending_obj) != 0);
    } else {
        PyObject* dseq = PySequence_Fast(
            descending_obj, "descending must be a bool or a sequence of bool");
        if (!dseq) return nullptr;
        Py_ssize_t dn = PySequence_Fast_GET_SIZE(dseq);
        for (Py_ssize_t i = 0; i < dn; ++i) {
            int truth = PyObject_IsTrue(PySequence_Fast_GET_ITEM(dseq, i));
            if (truth < 0) {
                Py_DECREF(dseq);
                return nullptr;
            }
            descending.push_back(truth != 0);
        }
        Py_DECREF(dseq);
    }
    return run_batch_op([&] {
        return dataframe::sort_by_multi(to_dataframe(b), names, descending);
    });
}

PyObject* DataFrame_sample(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    Py_ssize_t n = 0;
    unsigned long long seed = 0;
    static const char* kwlist[] = {"n", "seed", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "n|K",
                                     const_cast<char**>(kwlist), &n, &seed))
        return nullptr;
    return run_batch_op([&] {
        return dataframe::sample(to_dataframe(b), static_cast<std::int64_t>(n),
                                 static_cast<std::uint64_t>(seed));
    });
}

PyObject* DataFrame_with_row_index(PyObject* self, PyObject* arg) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    const char* name = PyUnicode_AsUTF8(arg);
    if (!name) {
        PyErr_SetString(PyExc_TypeError, "with_row_index() expects a str");
        return nullptr;
    }
    return run_batch_op(
        [&] { return dataframe::with_row_index(to_dataframe(b), name); });
}

PyObject* DataFrame_describe(PyObject* self, PyObject*) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    return run_batch_op([&] { return dataframe::describe(to_dataframe(b)); });
}

PyObject* DataFrame_null_count(PyObject* self, PyObject*) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    return run_batch_op([&] { return dataframe::null_count(to_dataframe(b)); });
}

// reduce(agg): `agg` is an aggregate name (sum, mean, ...) broadcast over
// every eligible column, one row.
PyObject* DataFrame_reduce(PyObject* self, PyObject* arg) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    const char* agg = PyUnicode_AsUTF8(arg);
    if (!agg) return nullptr;
    return run_batch_op([&] {
        return dataframe::reduce(to_dataframe(b),
                                 dataframe::agg_from_string(agg));
    });
}

PyObject* DataFrame_group_transform(PyObject* self, PyObject* args,
                                    PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    dftracer::utils::python::GroupwiseArgs a;
    if (!dftracer::utils::python::parse_groupwise_args(args, kwds,
                                                       names_from_obj, a))
        return nullptr;
    return run_batch_op([&] {
        return dataframe::GroupBy(to_dataframe(b), a.keys)
            .transform(static_cast<dataframe::GroupwiseOp>(a.kind), a.n,
                       static_cast<dataframe::RankMethod>(a.method),
                       a.ascending != 0);
    });
}

// reduce_specs(agg, keys) -> list[str]: the "op:column:out" specs that
// broadcast `agg` over every eligible non-key column, for group_by.
PyObject* DataFrame_reduce_specs(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    const char* agg = nullptr;
    PyObject* keys_obj = Py_None;
    if (!PyArg_ParseTuple(args, "s|O", &agg, &keys_obj)) return nullptr;
    std::vector<std::string> keys;
    if (keys_obj != Py_None && !names_from_obj(keys_obj, keys)) return nullptr;
    std::vector<std::string> specs;
    try {
        specs = dftracer::utils::python::reduce_spec_strings(
            dataframe::reduce_specs(to_dataframe(b),
                                    dataframe::agg_from_string(agg), keys));
    } catch (const std::out_of_range& e) {
        PyErr_SetString(PyExc_KeyError, e.what());
        return nullptr;
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
        return nullptr;
    }
    PyObject* list = PyList_New(static_cast<Py_ssize_t>(specs.size()));
    if (!list) return nullptr;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        PyObject* s = PyUnicode_FromString(specs[i].c_str());
        if (!s) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), s);
    }
    return list;
}

PyObject* DataFrame_is_duplicated(PyObject* self, PyObject*) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    try {
        return dftracer::utils::python::wrap_vec_column(
            dataframe::is_duplicated(to_dataframe(b)));
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
        return nullptr;
    }
}

PyObject* DataFrame_is_unique(PyObject* self, PyObject*) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    try {
        return dftracer::utils::python::wrap_vec_column(
            dataframe::is_unique(to_dataframe(b)));
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
        return nullptr;
    }
}

PyObject* DataFrame_column_index(PyObject* self, PyObject* arg) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    const char* name = PyUnicode_AsUTF8(arg);
    if (!name) {
        PyErr_SetString(PyExc_TypeError, "column_index() expects a str");
        return nullptr;
    }
    return PyLong_FromLongLong(to_dataframe(b).column_index(name));
}

PyObject* DataFrame_head(PyObject* self, PyObject* arg) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    long long n = PyLong_AsLongLong(arg);
    if (n == -1 && PyErr_Occurred()) return nullptr;
    return run_batch_op([&] { return dataframe::head(to_dataframe(b), n); });
}

PyObject* DataFrame_slice(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    long long offset = 0, length = 0;
    if (!PyArg_ParseTuple(args, "LL", &offset, &length)) return nullptr;
    return run_batch_op(
        [&] { return dataframe::slice(to_dataframe(b), offset, length); });
}

PyObject* DataFrame_sort_by(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    const char* name = nullptr;
    int descending = 0;
    static const char* kwlist[] = {"name", "descending", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "s|p", const_cast<char**>(kwlist), &name, &descending))
        return nullptr;
    return run_batch_op([&] {
        return dataframe::sort_by(to_dataframe(b), name, descending != 0);
    });
}

PyObject* DataFrame_topk(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    const char* name = nullptr;
    Py_ssize_t k = 0;
    int largest = 1;
    static const char* kwlist[] = {"name", "k", "largest", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sn|p",
                                     const_cast<char**>(kwlist), &name, &k,
                                     &largest))
        return nullptr;
    return run_batch_op([&] {
        return dataframe::topk(to_dataframe(b), name,
                               static_cast<std::int64_t>(k), largest != 0);
    });
}

PyObject* DataFrame_partition_id(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    PyObject* keys_obj = nullptr;
    Py_ssize_t n_parts = 0;
    if (!PyArg_ParseTuple(args, "On", &keys_obj, &n_parts)) return nullptr;
    std::vector<std::string> keys;
    if (PyUnicode_Check(keys_obj)) {
        keys.emplace_back(PyUnicode_AsUTF8(keys_obj));
    } else if (!parse_string_seq(keys_obj, "keys must be a str or list",
                                 keys)) {
        return nullptr;
    }
    try {
        return dftracer::utils::python::wrap_vec_column(dataframe::partition_id(
            to_dataframe(b), keys, static_cast<std::int64_t>(n_parts)));
    } catch (const std::out_of_range& e) {
        PyErr_SetString(PyExc_KeyError, e.what());
        return nullptr;
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
        return nullptr;
    }
}

// hash_partition(keys, n_parts) -> list[DataFrame]; keys is a name or a
// sequence of names. The shuffle primitive for distributed group_by/join.
PyObject* DataFrame_hash_partition(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    PyObject* keys_obj = nullptr;
    Py_ssize_t n_parts = 0;
    if (!PyArg_ParseTuple(args, "On", &keys_obj, &n_parts)) return nullptr;

    std::vector<std::string> keys;
    if (PyUnicode_Check(keys_obj)) {
        keys.emplace_back(PyUnicode_AsUTF8(keys_obj));
    } else if (!parse_string_seq(keys_obj, "keys must be a str or list",
                                 keys)) {
        return nullptr;
    }

    std::vector<DataFrame> parts;
    try {
        parts = dataframe::hash_partition(to_dataframe(b), keys,
                                          static_cast<std::int64_t>(n_parts));
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
        return nullptr;
    }
    PyObject* list = PyList_New(static_cast<Py_ssize_t>(parts.size()));
    if (!list) return nullptr;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        PyObject* vb = make_dataframe(std::move(parts[i]));
        if (!vb) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), vb);
    }
    return list;
}

// _group_agg_expr(key, specs): the expression-aggregate workhorse. `key` is a
// column name or a sequence of names (composite key). `specs` is a list of
// (op_int, value_ast_or_None, out_name[, param[, by_ast]]); value ASTs
// reference this batch's columns by index. `by_ast` is ArgMax's maximized
// value. All value expressions compile in one CSE'd, pruned pass
// (dataframe::group_agg_expr). The Python GroupBy serializes to this.
PyObject* DataFrame_group_agg_expr(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    PyObject* key_obj = nullptr;
    PyObject* specs = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &key_obj, &specs)) return nullptr;
    std::vector<std::string> keys;
    if (!strings_from_str_or_seq(key_obj, keys)) return nullptr;
    PyObject* seq = PySequence_Fast(specs, "specs must be a sequence");
    if (!seq) return nullptr;

    DataFrame bat = to_dataframe(b);
    std::vector<dataframe::Expr> key_exprs;
    key_exprs.reserve(keys.size());
    for (const std::string& key : keys) {
        std::int32_t ki = -1;
        for (std::size_t i = 0; i < bat.names.size(); ++i)
            if (bat.names[i] == key) {
                ki = static_cast<std::int32_t>(i);
                break;
            }
        if (ki < 0) {
            Py_DECREF(seq);
            PyErr_Format(PyExc_KeyError, "group_by: no column named %s",
                         key.c_str());
            return nullptr;
        }
        key_exprs.push_back(dataframe::expr_col(ki));
    }

    std::vector<dataframe::AggExprSpec> aggs;
    Py_ssize_t ns = PySequence_Fast_GET_SIZE(seq);
    aggs.reserve(static_cast<std::size_t>(ns));
    for (Py_ssize_t i = 0; i < ns; ++i) {
        PyObject* t = PySequence_Fast_GET_ITEM(seq, i);
        const Py_ssize_t tn = PyTuple_Check(t) ? PyTuple_GET_SIZE(t) : 0;
        if (tn != 3 && tn != 4 && tn != 5) {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_TypeError,
                            "each spec is (op, ast, out[, param[, by_ast]])");
            return nullptr;
        }
        dataframe::AggExprSpec s;
        s.op = static_cast<dataframe::AggOp>(
            PyLong_AsLong(PyTuple_GET_ITEM(t, 0)));
        if (tn >= 4) {
            s.param = PyFloat_AsDouble(PyTuple_GET_ITEM(t, 3));
            if (s.param == -1.0 && PyErr_Occurred()) {
                Py_DECREF(seq);
                return nullptr;
            }
        }
        PyObject* ast = PyTuple_GET_ITEM(t, 1);
        if (ast != Py_None) {
            dataframe::Expr v;
            if (!dftracer::utils::python::build_expr_from_ast(ast, &v)) {
                Py_DECREF(seq);
                return nullptr;
            }
            s.value = std::move(v);
        }
        if (tn == 5) {
            PyObject* by_ast = PyTuple_GET_ITEM(t, 4);
            if (by_ast != Py_None) {
                dataframe::Expr by;
                if (!dftracer::utils::python::build_expr_from_ast(by_ast,
                                                                  &by)) {
                    Py_DECREF(seq);
                    return nullptr;
                }
                s.by = std::move(by);
            }
        }
        const char* out = PyUnicode_AsUTF8(PyTuple_GET_ITEM(t, 2));
        if (!out) {
            Py_DECREF(seq);
            return nullptr;
        }
        s.out = out;
        aggs.push_back(std::move(s));
    }
    Py_DECREF(seq);

    std::vector<const Series*> inputs;
    inputs.reserve(bat.columns.size());
    for (const Series& c : bat.columns) inputs.push_back(&c);
    return run_batch_op([&] {
        return dataframe::group_agg_expr(key_exprs, aggs, inputs, keys);
    });
}

// Two-step / expression forms delegate to the Python GroupBy, which serializes
// each spec and calls _group_agg_expr. `args[0:nk]` are the key names (a tuple,
// even for one key); `args[nk:]` are the specs.
PyObject* delegate_groupby(PyObject* self, PyObject* args, Py_ssize_t nk) {
    PyObject* mod = PyImport_ImportModule("dftracer.utils.columnar");
    if (!mod) return nullptr;
    PyObject* gb_type = PyObject_GetAttrString(mod, "GroupBy");
    Py_DECREF(mod);
    if (!gb_type) return nullptr;
    PyObject* keys = PyTuple_GetSlice(args, 0, nk);
    if (!keys) {
        Py_DECREF(gb_type);
        return nullptr;
    }
    PyObject* gb = PyObject_CallFunctionObjArgs(gb_type, self, keys, nullptr);
    Py_DECREF(keys);
    Py_DECREF(gb_type);
    if (!gb) return nullptr;
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n == nk) return gb;             // two-step: hand back the GroupBy
    PyObject* rest =
        PyTuple_GetSlice(args, nk, n);  // the specs, as an arg tuple
    PyObject* aggm = rest ? PyObject_GetAttrString(gb, "agg") : nullptr;
    Py_DECREF(gb);
    PyObject* res = aggm ? PyObject_Call(aggm, rest, nullptr) : nullptr;
    Py_XDECREF(aggm);
    Py_XDECREF(rest);
    return res;
}

// group_by(*keys_and_specs). The leading run of plain column-name strings
// (not "count" and not containing ':') are the (possibly composite) key; a
// trailing run of legacy "op[:column]" strings runs inline, anything else
// (no specs, or an expression/Agg spec) delegates to the Python GroupBy and
// the expression-aggregate path (CSE + pruner).
PyObject* DataFrame_group_by(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n < 1) {
        PyErr_SetString(PyExc_TypeError, "group_by(*keys, *aggs) needs a key");
        return nullptr;
    }
    Py_ssize_t nk = 0;
    for (; nk < n; ++nk) {
        PyObject* a = PyTuple_GET_ITEM(args, nk);
        if (!PyUnicode_Check(a)) break;
        const char* s = PyUnicode_AsUTF8(a);
        if (!s) return nullptr;
        const std::string sv(s);
        if (sv == "count" || sv.find(':') != std::string::npos) break;
    }
    if (nk == 0) nk = 1;  // args[0] is always at least one key
    bool rest_legacy = n > nk;
    for (Py_ssize_t i = nk; i < n && rest_legacy; ++i)
        rest_legacy = PyUnicode_Check(PyTuple_GET_ITEM(args, i)) != 0;
    if (n == nk || !rest_legacy) return delegate_groupby(self, args, nk);

    std::vector<std::string> keys;
    keys.reserve(static_cast<std::size_t>(nk));
    for (Py_ssize_t i = 0; i < nk; ++i) {
        const char* s = PyUnicode_AsUTF8(PyTuple_GET_ITEM(args, i));
        if (!s) return nullptr;
        keys.emplace_back(s);
    }
    std::vector<std::string> specs;
    specs.reserve(static_cast<std::size_t>(n - nk));
    for (Py_ssize_t i = nk; i < n; ++i) {
        const char* s = PyUnicode_AsUTF8(PyTuple_GET_ITEM(args, i));
        if (!s) return nullptr;
        specs.emplace_back(s);
    }
    return run_batch_op([&] {
        std::vector<dataframe::GroupAgg> aggs;
        aggs.reserve(specs.size());
        for (const std::string& spec : specs)
            aggs.push_back(group_agg_from_spec(spec));
        return dataframe::group_by(to_dataframe(b), keys, aggs);
    });
}

// join(other, how="inner", on=1): equi-join on the first `on` (leading) key
// columns; how is inner|left|right|full|semi|anti.
PyObject* DataFrame_join(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    PyObject* other = nullptr;
    PyObject* on = nullptr;
    const char* how = "inner";
    PyObject* left_on = nullptr;
    PyObject* right_on = nullptr;
    const char* suffix = "_right";
    static const char* kwlist[] = {"other",    "on",     "how",  "left_on",
                                   "right_on", "suffix", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|OsOOs",
                                     const_cast<char**>(kwlist), &other, &on,
                                     &how, &left_on, &right_on, &suffix))
        return nullptr;
    DataFrameObject* o = as_dataframe(other);
    if (!o) return nullptr;
    dataframe::JoinHow jh;
    if (!join_how_from_str(how, jh)) return nullptr;
    std::vector<std::string> l;
    std::vector<std::string> r;
    if (!join_keys_from_objs(on, left_on, right_on, jh, l, r)) return nullptr;
    return run_batch_op([&] {
        return dataframe::join(to_dataframe(b), to_dataframe(o), l, r, jh,
                               suffix);
    });
}

PyObject* DataFrame_compare_agg(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    PyObject* other = nullptr;
    Py_ssize_t n_key = 1;
    if (!PyArg_ParseTuple(args, "On", &other, &n_key)) return nullptr;
    DataFrameObject* o = as_dataframe(other);
    if (!o) return nullptr;
    return run_batch_op([&] {
        return dataframe::compare_agg(to_dataframe(b), to_dataframe(o),
                                      static_cast<std::int64_t>(n_key));
    });
}

// unpivot(id_vars, value_vars) / melt: reshape wide -> long.
PyObject* DataFrame_unpivot(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    PyObject* id_obj = nullptr;
    PyObject* val_obj = nullptr;
    static const char* kwlist[] = {"id_vars", "value_vars", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "OO", const_cast<char**>(kwlist), &id_obj, &val_obj))
        return nullptr;
    std::vector<std::string> id_vars, value_vars;
    if (!names_from_obj(id_obj, id_vars)) return nullptr;
    if (!names_from_obj(val_obj, value_vars)) return nullptr;
    return run_batch_op([&] {
        return dataframe::unpivot(to_dataframe(b), id_vars, value_vars);
    });
}

PyObject* DataFrame_explode(PyObject* self, PyObject* arg) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    const char* name = PyUnicode_AsUTF8(arg);
    if (!name) {
        PyErr_SetString(PyExc_TypeError, "explode() expects a column name");
        return nullptr;
    }
    return run_batch_op(
        [&] { return dataframe::explode(to_dataframe(b), name); });
}

namespace arr = dftracer::utils::utilities::common::arrow;

PyObject* DataFrame_window(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    static const char* kwlist[] = {"partition_by", "order_by", "specs",
                                   nullptr};
    PyObject* part = nullptr;
    PyObject* order = nullptr;
    PyObject* specs = nullptr;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOO",
                                     const_cast<char**>(kwlist), &part, &order,
                                     &specs))
        return nullptr;
    std::vector<std::string> pcols;
    std::vector<std::string> ocols;
    if (!parse_string_seq(part, "window: partition_by must be names", pcols) ||
        !parse_string_seq(order, "window: order_by must be names", ocols))
        return nullptr;
    dftracer::utils::python::WindowSpecs parsed;
    if (!dftracer::utils::python::parse_window_specs(specs, parsed))
        return nullptr;
    std::vector<arr::WindowColumn> specv;
    specv.reserve(parsed.specs.size());
    for (const dftu_window_spec& w : parsed.specs) {
        arr::WindowColumn c;
        c.func = static_cast<arr::WindowFunc>(w.func);
        if (w.value) c.value = std::string(w.value);
        if (w.time) c.time = std::string(w.time);
        c.out = w.out;
        c.offset = w.offset;
        c.threshold = w.threshold;
        c.counter = w.counter != 0;
        c.preceding = w.preceding;
        c.following = w.following;
        specv.push_back(std::move(c));
    }
    return run_batch_op(
        [&] { return arr::window(to_dataframe(b), pcols, ocols, specv); });
}

PyObject* DataFrame_gap_fill(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    static const char* kwlist[] = {"partition_by", "time",  "bucket", "values",
                                   "mode",         "start", "end",    nullptr};
    PyObject* part = nullptr;
    const char* time = nullptr;
    long long bucket = 0;
    PyObject* values = nullptr;
    const char* mode = nullptr;
    PyObject* start_obj = Py_None;
    PyObject* end_obj = Py_None;
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "OsLOs|OO", const_cast<char**>(kwlist), &part, &time,
            &bucket, &values, &mode, &start_obj, &end_obj))
        return nullptr;
    dftu_gap_fill_mode mode_code;
    if (!dftracer::utils::python::gap_fill_mode_from_str(mode, &mode_code))
        return nullptr;
    const auto m = static_cast<arr::GapFillMode>(mode_code);
    std::vector<std::string> pcols;
    std::vector<std::string> vcols;
    if (!parse_string_seq(part, "gap_fill: partition_by must be names",
                          pcols) ||
        !parse_string_seq(values, "gap_fill: values must be names", vcols))
        return nullptr;
    std::optional<std::pair<std::int64_t, std::int64_t>> range;
    if (start_obj != Py_None) {
        const long long s = PyLong_AsLongLong(start_obj);
        const long long e = PyLong_AsLongLong(end_obj);
        if (PyErr_Occurred()) return nullptr;
        range = std::make_pair(static_cast<std::int64_t>(s),
                               static_cast<std::int64_t>(e));
    }
    return run_batch_op([&] {
        return arr::gap_fill(to_dataframe(b), pcols, time,
                             static_cast<std::int64_t>(bucket), vcols, m,
                             range);
    });
}

PyObject* DataFrame_asof(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    static const char* kwlist[] = {"other",     "on",        "by",
                                   "direction", "tolerance", nullptr};
    PyObject* other = nullptr;
    const char* on = nullptr;
    PyObject* by = nullptr;
    const char* direction = nullptr;
    PyObject* tol_obj = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OsOsO",
                                     const_cast<char**>(kwlist), &other, &on,
                                     &by, &direction, &tol_obj))
        return nullptr;
    DataFrameObject* o = as_dataframe(other);
    if (!o) return nullptr;
    dftu_asof_direction dir_code;
    if (!dftracer::utils::python::asof_direction_from_str(direction, &dir_code))
        return nullptr;
    const auto dir = static_cast<arr::AsofDirection>(dir_code);
    std::vector<std::string> equi;
    if (!parse_string_seq(by, "asof: by must be names", equi)) return nullptr;
    std::optional<std::int64_t> tol;
    if (tol_obj != Py_None) {
        const long long t = PyLong_AsLongLong(tol_obj);
        if (PyErr_Occurred()) return nullptr;
        tol = static_cast<std::int64_t>(t);
    }
    return run_batch_op([&] {
        return arr::asof(to_dataframe(b), to_dataframe(o), on, equi, dir, tol);
    });
}

PyObject* DataFrame_interval(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    static const char* kwlist[] = {"other", "point", "lo",   "hi",
                                   "by",    "outer", nullptr};
    PyObject* other = nullptr;
    const char* point = nullptr;
    const char* lo = nullptr;
    const char* hi = nullptr;
    PyObject* by = nullptr;
    int outer = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OsssOp",
                                     const_cast<char**>(kwlist), &other, &point,
                                     &lo, &hi, &by, &outer))
        return nullptr;
    DataFrameObject* o = as_dataframe(other);
    if (!o) return nullptr;
    std::vector<std::string> equi;
    if (!parse_string_seq(by, "interval: by must be names", equi))
        return nullptr;
    return run_batch_op([&] {
        return arr::interval(to_dataframe(b), to_dataframe(o), point, lo, hi,
                             equi, outer != 0);
    });
}

PyObject* DataFrame_unnest(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    const char* name = nullptr;
    int keep_empty = 0;
    static const char* kwlist[] = {"column", "keep_empty", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "s|p", const_cast<char**>(kwlist), &name, &keep_empty))
        return nullptr;
    return run_batch_op([&] {
        return dataframe::unnest(to_dataframe(b), name, keep_empty != 0);
    });
}

PyObject* DataFrame_to_dummies(PyObject* self, PyObject* arg) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    const char* name = PyUnicode_AsUTF8(arg);
    if (!name) {
        PyErr_SetString(PyExc_TypeError, "to_dummies() expects a column name");
        return nullptr;
    }
    return run_batch_op(
        [&] { return dataframe::to_dummies(to_dataframe(b), name); });
}

// pivot(index, columns, values, agg="first"): long -> wide.
PyObject* DataFrame_pivot(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    const char* index = nullptr;
    const char* columns = nullptr;
    const char* values = nullptr;
    const char* agg = "first";
    static const char* kwlist[] = {"index", "columns", "values", "agg",
                                   nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sss|s",
                                     const_cast<char**>(kwlist), &index,
                                     &columns, &values, &agg))
        return nullptr;
    return run_batch_op([&] {
        return dataframe::pivot(to_dataframe(b), index, columns, values, agg);
    });
}

// group_by_dynamic(time_col, every, period=None, aggs=[...]): tumbling/sliding
// time-window aggregation. aggs mirror the group_by string-spec format.
PyObject* DataFrame_group_by_dynamic(PyObject* self, PyObject* args,
                                     PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    const char* time_col = nullptr;
    long long every = 0;
    PyObject* period_obj = Py_None;
    PyObject* aggs_obj = Py_None;
    PyObject* origin_obj = Py_None;  // int (explicit) or "min"
    static const char* kwlist[] = {"time_col", "every",  "period",
                                   "aggs",     "origin", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "sL|OOO", const_cast<char**>(kwlist), &time_col, &every,
            &period_obj, &aggs_obj, &origin_obj))
        return nullptr;
    std::int64_t period = 0;
    if (period_obj && period_obj != Py_None) {
        period = PyLong_AsLongLong(period_obj);
        if (period == -1 && PyErr_Occurred()) return nullptr;
    }
    std::int64_t origin = 0;
    bool origin_min = false;
    if (origin_obj && origin_obj != Py_None) {
        if (PyUnicode_Check(origin_obj)) {
            const char* s = PyUnicode_AsUTF8(origin_obj);
            if (!s) return nullptr;
            if (std::string(s) != "min") {
                PyErr_SetString(PyExc_ValueError,
                                "group_by_dynamic: origin must be an int or "
                                "\"min\"");
                return nullptr;
            }
            origin_min = true;
        } else {
            origin = PyLong_AsLongLong(origin_obj);
            if (origin == -1 && PyErr_Occurred()) return nullptr;
        }
    }
    std::vector<dataframe::GroupAgg> aggs;
    if (aggs_obj && aggs_obj != Py_None && !aggs_from_seq(aggs_obj, aggs))
        return nullptr;
    return run_batch_op([&] {
        return dataframe::group_by_dynamic(to_dataframe(b), time_col,
                                           static_cast<std::int64_t>(every),
                                           period, aggs, origin, origin_min);
    });
}

PyObject* DataFrame_concat(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    // `how` is the only keyword; every positional arg is another DataFrame.
    dataframe::ConcatHow how = dataframe::ConcatHow::Vertical;
    if (kwds) {
        if (PyObject* h = PyDict_GetItemString(kwds, "how")) {
            const char* s = PyUnicode_AsUTF8(h);
            if (!s) return nullptr;
            if (std::strcmp(s, "diagonal") == 0)
                how = dataframe::ConcatHow::Diagonal;
            else if (std::strcmp(s, "vertical") != 0) {
                PyErr_SetString(PyExc_ValueError,
                                "how must be 'vertical' or 'diagonal'");
                return nullptr;
            }
        }
        if (PyDict_Size(kwds) > (PyDict_GetItemString(kwds, "how") ? 1 : 0)) {
            PyErr_SetString(PyExc_TypeError,
                            "concat() got an unexpected keyword argument");
            return nullptr;
        }
    }
    std::vector<DataFrame> owned;
    owned.reserve(1 + static_cast<std::size_t>(PyTuple_GET_SIZE(args)));
    owned.push_back(to_dataframe(b));
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(args); ++i) {
        DataFrameObject* o = as_dataframe(PyTuple_GET_ITEM(args, i));
        if (!o) return nullptr;
        owned.push_back(to_dataframe(o));
    }
    std::vector<const DataFrame*> parts;
    parts.reserve(owned.size());
    for (const DataFrame& x : owned) parts.push_back(&x);
    return run_batch_op([&] { return dataframe::concat(parts, how); });
}

// Arrow/pandas/polars conversion and pickling live in the Python wrapper
// (dftracer.utils.frame); the native handle only exposes the zero-copy
// __arrow_c_array__ / __arrow_c_stream__ capsules.

// The batch is one STRUCT column, so a single struct array carries every field.

void schema_capsule_destructor(PyObject* cap) {
    auto* s =
        static_cast<ArrowSchema*>(PyCapsule_GetPointer(cap, "arrow_schema"));
    if (s) {
        if (s->release) s->release(s);
        delete s;
    }
}
void array_capsule_destructor(PyObject* cap) {
    auto* a =
        static_cast<ArrowArray*>(PyCapsule_GetPointer(cap, "arrow_array"));
    if (a) {
        if (a->release) a->release(a);
        delete a;
    }
}

PyObject* DataFrame_arrow_c_array(PyObject* self, PyObject*) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    auto* schema = new (std::nothrow) ArrowSchema{};
    auto* array = new (std::nothrow) ArrowArray{};
    if (!schema || !array) {
        delete schema;
        delete array;
        return PyErr_NoMemory();
    }
    try {
        dataframe::to_arrow(b->st, schema, array);
    } catch (const std::exception& e) {
        delete schema;
        delete array;
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
    }
    PyObject* scap =
        PyCapsule_New(schema, "arrow_schema", schema_capsule_destructor);
    if (!scap) {
        if (schema->release) schema->release(schema);
        delete schema;
        if (array->release) array->release(array);
        delete array;
        return nullptr;
    }
    PyObject* acap =
        PyCapsule_New(array, "arrow_array", array_capsule_destructor);
    if (!acap) {
        Py_DECREF(scap);
        if (array->release) array->release(array);
        delete array;
        return nullptr;
    }
    PyObject* result = PyTuple_Pack(2, scap, acap);
    Py_DECREF(scap);
    Py_DECREF(acap);
    return result;
}

void array_stream_capsule_destructor(PyObject* cap) {
    auto* s = static_cast<ArrowArrayStream*>(
        PyCapsule_GetPointer(cap, "arrow_array_stream"));
    if (s) {
        if (s->release) s->release(s);
        delete s;
    }
}

// __arrow_c_stream__: a one-batch stream of the struct, so pa.table(batch) /
// pl.from_arrow(batch) import every column zero-copy without an explicit
// to_arrow(). The single struct array carries all fields.
PyObject* DataFrame_arrow_c_stream(PyObject* self, PyObject*) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    ArrowSchema schema{};
    ArrowArray array{};
    try {
        dataframe::to_arrow(b->st, &schema, &array);
    } catch (const std::exception& e) {
        if (schema.release) schema.release(&schema);
        if (array.release) array.release(&array);
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
    }
    auto* stream = new (std::nothrow) ArrowArrayStream{};
    if (!stream) {
        if (schema.release) schema.release(&schema);
        if (array.release) array.release(&array);
        return PyErr_NoMemory();
    }
    if (ArrowBasicArrayStreamInit(stream, &schema, 1) != NANOARROW_OK) {
        if (schema.release) schema.release(&schema);
        if (array.release) array.release(&array);
        delete stream;
        PyErr_SetString(PyExc_RuntimeError,
                        "failed to init arrow array stream");
        return nullptr;
    }
    ArrowBasicArrayStreamSetArray(stream, 0, &array);  // moves array in
    PyObject* cap = PyCapsule_New(stream, "arrow_array_stream",
                                  array_stream_capsule_destructor);
    if (!cap) {
        if (stream->release) stream->release(stream);
        delete stream;
        return nullptr;
    }
    return cap;
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
// to_ipc() -> bytes: the frame serialized as an Arrow IPC stream (schema + one
// record batch + EOS), so a consumer needs no pyarrow to produce .arrow bytes.
PyObject* DataFrame_to_ipc(PyObject* self, PyObject*) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    try {
        std::vector<std::uint8_t> bytes = to_dataframe(b).to_ipc();
        return PyBytes_FromStringAndSize(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<Py_ssize_t>(bytes.size()));
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
    }
}
#endif

// __reduce__: pickle by round-tripping through Arrow (pyarrow Tables pickle via
// IPC), so a DataFrame can ship across processes / be persisted.
PyObject* DataFrame_get_num_rows(PyObject* self, void*) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    return PyLong_FromLongLong(b->st.length());
}
PyObject* DataFrame_get_num_columns(PyObject* self, void*) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    return PyLong_FromSsize_t(static_cast<Py_ssize_t>(b->names.size()));
}
PyObject* DataFrame_get_column_names(PyObject* self, void*) {
    return DataFrame_keys(self, nullptr);
}

// lazy() -> _LazyFrame: start a deferred query over a copy of this batch.
PyObject* DataFrame_lazy(PyObject* self, PyObject*) {
    DataFrameObject* b = as_dataframe(self);
    if (!b) return nullptr;
    try {
        return dftracer::utils::python::wrap_lazyframe(
            dataframe::lazy(to_dataframe(b)));
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
        return nullptr;
    }
}

PyMethodDef DataFrame_methods[] = {
    {"lazy", DataFrame_lazy, METH_NOARGS,
     "lazy() -> _LazyFrame, a deferred query over this batch."},
    {"keys", DataFrame_keys, METH_NOARGS, "Series names, in order."},
    {"filter", DataFrame_filter, METH_O,
     "filter(mask) -> DataFrame keeping rows where the Bool mask is true."},
    {"select", DataFrame_select, METH_VARARGS,
     "select(*names) -> DataFrame projecting the named columns (zero-copy)."},
    {"rename", DataFrame_rename, METH_O,
     "rename({old: new}) -> DataFrame with columns renamed (zero-copy)."},
    {"with_column", DataFrame_with_column, METH_VARARGS,
     "with_column(name, Series) -> DataFrame adding/replacing a column."},
    {"take", DataFrame_take, METH_O,
     "take(indices) -> DataFrame gathered at the given row indices."},
    {"column_index", DataFrame_column_index, METH_O,
     "column_index(name) -> int index of a column, or -1 if absent."},
    {"head", DataFrame_head, METH_O,
     "head(n) -> DataFrame of the first n rows."},
    {"tail", DataFrame_tail, METH_O,
     "tail(n) -> DataFrame of the last n rows."},
    {"reverse", DataFrame_reverse, METH_NOARGS,
     "reverse() -> DataFrame with rows reversed."},
    {"drop_nulls", DataFrame_drop_nulls, METH_NOARGS,
     "drop_nulls() -> DataFrame dropping rows null in any column."},
    {"fill_null", DataFrame_fill_null, METH_O,
     "fill_null(value) -> DataFrame with nulls filled in every column."},
    {"unique", DFTU_PYCFUNCTION(DataFrame_unique), METH_VARARGS | METH_KEYWORDS,
     "unique(subset=None) -> DataFrame with duplicate rows removed (keep "
     "first), keyed on every column or on the subset names."},
    {"drop_duplicates", DFTU_PYCFUNCTION(DataFrame_unique),
     METH_VARARGS | METH_KEYWORDS,
     "drop_duplicates(subset=None) -> DataFrame with duplicate rows removed "
     "(alias of unique)."},
    {"sort_by_multi", DFTU_PYCFUNCTION(DataFrame_sort_by_multi),
     METH_VARARGS | METH_KEYWORDS,
     "sort_by_multi(names, descending=False) -> DataFrame stably sorted "
     "lexicographically by several key columns."},
    {"sample", DFTU_PYCFUNCTION(DataFrame_sample), METH_VARARGS | METH_KEYWORDS,
     "sample(n, seed=0) -> DataFrame deterministic n-row sample."},
    {"with_row_index", DataFrame_with_row_index, METH_O,
     "with_row_index(name) -> DataFrame with a prepended Int64 index column."},
    {"describe", DataFrame_describe, METH_NOARGS,
     "describe() -> DataFrame of per-column summary statistics."},
    {"null_count", DataFrame_null_count, METH_NOARGS,
     "null_count() -> 1-row DataFrame of each column's null count."},
    {"reduce", DataFrame_reduce, METH_O,
     "reduce(agg) -> one-row DataFrame: the aggregate named `agg` over every "
     "eligible column."},
    {"group_transform", DFTU_PYCFUNCTION(DataFrame_group_transform),
     METH_VARARGS | METH_KEYWORDS,
     "group_transform(keys, kind, n=0, method='average', ascending=True) -> "
     "DataFrame: the group-wise transform `kind` (cumsum, cummax, cummin, "
     "cumcount, shift, diff, pct_change, rank, ngroup, head, tail, nth) over "
     "the groups of `keys`, one value per input row in input order."},
    {"reduce_specs", DataFrame_reduce_specs, METH_VARARGS,
     "reduce_specs(agg, keys=None) -> list[str]: the 'op:column:out' specs "
     "that broadcast `agg` over every eligible non-key column, for group_by."},
    {"is_duplicated", DataFrame_is_duplicated, METH_NOARGS,
     "is_duplicated() -> Bool Series, true where the whole row is duplicated."},
    {"is_unique", DataFrame_is_unique, METH_NOARGS,
     "is_unique() -> Bool Series, true where the whole row is unique."},
    {"slice", DataFrame_slice, METH_VARARGS,
     "slice(offset, length) -> DataFrame of a row range."},
    {"sort_by", DFTU_PYCFUNCTION(DataFrame_sort_by),
     METH_VARARGS | METH_KEYWORDS,
     "sort_by(name, descending=False) -> DataFrame ordered by a column."},
    {"topk", DFTU_PYCFUNCTION(DataFrame_topk), METH_VARARGS | METH_KEYWORDS,
     "topk(name, k, largest=True) -> DataFrame of the k best rows by a "
     "column."},
    {"concat", DFTU_PYCFUNCTION(DataFrame_concat), METH_VARARGS | METH_KEYWORDS,
     "concat(*others, how='vertical') -> DataFrame concatenating batches. "
     "how='diagonal' unions columns (null-fill absent, promote numeric)."},
    {"unpivot", DFTU_PYCFUNCTION(DataFrame_unpivot),
     METH_VARARGS | METH_KEYWORDS,
     "unpivot(id_vars, value_vars) -> DataFrame reshaped wide->long, stacking "
     "value_vars into 'variable'/'value' columns."},
    {"melt", DFTU_PYCFUNCTION(DataFrame_unpivot), METH_VARARGS | METH_KEYWORDS,
     "melt(id_vars, value_vars) -> DataFrame (alias of unpivot)."},
    {"explode", DataFrame_explode, METH_O,
     "explode(column) -> DataFrame expanding a List column, one row per "
     "element (empty/null list -> one null row)."},
    {"window", DFTU_PYCFUNCTION(DataFrame_window), METH_VARARGS | METH_KEYWORDS,
     "window(partition_by, order_by, specs) -> DataFrame: SQL window "
     "functions; specs are normalized 9-tuples."},
    {"gap_fill", DFTU_PYCFUNCTION(DataFrame_gap_fill),
     METH_VARARGS | METH_KEYWORDS,
     "gap_fill(partition_by, time, bucket, values, mode, start=None, "
     "end=None) -> DataFrame: a regular time grid with none/locf/linear "
     "fills."},
    {"asof", DFTU_PYCFUNCTION(DataFrame_asof), METH_VARARGS | METH_KEYWORDS,
     "asof(other, on, by, direction, tolerance) -> DataFrame: temporal "
     "nearest-match join."},
    {"interval", DFTU_PYCFUNCTION(DataFrame_interval),
     METH_VARARGS | METH_KEYWORDS,
     "interval(other, point, lo, hi, by, outer) -> DataFrame: point-in-range "
     "join."},
    {"unnest", DFTU_PYCFUNCTION(DataFrame_unnest), METH_VARARGS | METH_KEYWORDS,
     "unnest(column, keep_empty=False) -> DataFrame expanding a List column "
     "one row per element; an empty/null list drops the row unless "
     "keep_empty, and a List<Struct> flattens into one column per field."},
    {"to_dummies", DataFrame_to_dummies, METH_O,
     "to_dummies(column) -> DataFrame one-hot encoding a column into one Int8 "
     "column per distinct value, named <column>_<value>."},
    {"pivot", DFTU_PYCFUNCTION(DataFrame_pivot), METH_VARARGS | METH_KEYWORDS,
     "pivot(index, columns, values, agg='first') -> DataFrame reshaping "
     "long->wide; agg is first|last|sum|min|max|mean."},
    {"group_by_dynamic", DFTU_PYCFUNCTION(DataFrame_group_by_dynamic),
     METH_VARARGS | METH_KEYWORDS,
     "group_by_dynamic(time_col, every, period=None, aggs=[...]) -> DataFrame "
     "tumbling/sliding time-window aggregation; aggs are 'count' / "
     "'<op>:<column>'."},
    {"query", DFTU_PYCFUNCTION(DataFrame_query), METH_VARARGS | METH_KEYWORDS,
     "query(dsl=None, *, group_by=None, aggs=None, select=None, order_by=None, "
     "descending=False, limit=None) -> DataFrame; a query plan "
     "(filter/group-by/select/sort/limit) over the batch. The DSL predicate is "
     "evaluated as a SIMD mask (pandas.query-style); aggs are 'count' or "
     "'<op>:<column>'."},
    {"group_by", DataFrame_group_by, METH_VARARGS,
     "group_by(key, *aggs) -> DataFrame or GroupBy; aggs are 'count', "
     "'<op>:<column>' (sum|min|max|mean|var|std|skew|kurt), or aggregate "
     "expressions (F.x.sum(), ...); no aggs returns a GroupBy for .agg(...)."},
    {"_group_agg_expr", DataFrame_group_agg_expr, METH_VARARGS,
     "_group_agg_expr(key, specs) -> DataFrame; specs is a list of "
     "(op_int, value_ast|None, out_name[, param[, by_ast]]). Internal: the "
     "GroupBy expression path (CSE across value expressions + pruner)."},
    {"join", DFTU_PYCFUNCTION(DataFrame_join), METH_VARARGS | METH_KEYWORDS,
     "join(other, on=None, how='inner', left_on=None, right_on=None, "
     "suffix='_right') -> DataFrame hash-joined on the key columns; "
     "how=inner|left|right|outer|full|semi|anti|cross."},
    {"compare_agg", DataFrame_compare_agg, METH_VARARGS,
     "compare_agg(variant, n_key) -> DataFrame: FULL-join two aggregation "
     "results on the first n_key group-key columns and append delta_/pct_ per "
     "numeric metric (the CompareView result)."},
    {"partition_id", DataFrame_partition_id, METH_VARARGS,
     "partition_id(keys, n_parts) -> Int32 Series: the part in [0, n_parts) "
     "each row lands in under a stable hash of the key columns."},
    {"hash_partition", DataFrame_hash_partition, METH_VARARGS,
     "hash_partition(keys, n_parts) -> list[DataFrame] partitioned by a stable "
     "hash of the key columns (the distributed shuffle primitive)."},
    {"__arrow_c_array__", DataFrame_arrow_c_array, METH_VARARGS,
     "Arrow PyCapsule export of the struct: (schema_capsule, array_capsule)."},
    {"__arrow_c_stream__", DataFrame_arrow_c_stream, METH_VARARGS,
     "Arrow PyCapsule stream export (one struct batch); pa.table(batch) uses "
     "this to import every column zero-copy."},
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    {"to_ipc", DataFrame_to_ipc, METH_NOARGS,
     "to_ipc() -> bytes: the frame as an Arrow IPC stream (schema + one record "
     "batch + EOS); no pyarrow needed to produce .arrow bytes."},
#endif
    {nullptr, nullptr, 0, nullptr}};

PyGetSetDef DataFrame_getset[] = {
    {"num_rows", DataFrame_get_num_rows, nullptr, "Row count.", nullptr},
    {"num_columns", DataFrame_get_num_columns, nullptr, "Series count.",
     nullptr},
    {"column_names", DataFrame_get_column_names, nullptr, "Series names.",
     nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}};

PyMappingMethods DataFrame_as_mapping = {};
PySequenceMethods DataFrame_as_sequence = {};

// vec_batch_from_arrow(table) -> DataFrame: import each column of a pyarrow
// Table (or RecordBatch) into vec. Used to reconstruct a pickled DataFrame and
// to lift Arrow into the native format.
PyObject* vec_batch_from_arrow(PyObject* /*self*/, PyObject* obj) {
    PyObject* names = PyObject_GetAttrString(obj, "column_names");
    if (!names) return nullptr;
    Py_ssize_t n = PySequence_Size(names);
    if (n < 0) {
        Py_DECREF(names);
        return nullptr;
    }
    DataFrame b;
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* nm = PySequence_GetItem(names, i);
        const char* s = nm ? PyUnicode_AsUTF8(nm) : nullptr;
        PyObject* ca = s ? PyObject_CallMethod(obj, "column", "n", i) : nullptr;
        PyObject* arr = nullptr;
        if (ca) {
            if (PyObject_HasAttrString(ca, "combine_chunks"))
                arr = PyObject_CallMethod(ca, "combine_chunks", nullptr);
            else {
                arr = ca;
                Py_INCREF(arr);
            }
        }
        Series col =
            arr ? dftracer::utils::python::column_from_arrow(arr) : Series{};
        Py_XDECREF(arr);
        Py_XDECREF(ca);
        if (!col.valid()) {
            Py_XDECREF(nm);
            Py_DECREF(names);
            return nullptr;
        }
        b.names.emplace_back(s);
        b.columns.push_back(std::move(col));
        Py_DECREF(nm);
    }
    Py_DECREF(names);
    return make_dataframe(std::move(b));
}

}  // namespace

namespace dftracer::utils::python {

PyObject* wrap_dataframe(dftracer::utils::dataframe::DataFrame&& batch) {
    return make_dataframe(std::move(batch));
}

int init_dataframe(PyObject* m) {
    DataFrame_as_mapping.mp_subscript = DataFrame_subscript;
    DataFrame_as_sequence.sq_contains = DataFrame_contains;
    DataFrameType = {};
    DataFrameType.ob_base = {PyObject_HEAD_INIT(nullptr) 0};
    DataFrameType.tp_name = "dftracer_utils_ext._DataFrame";
    DataFrameType.tp_basicsize = sizeof(DataFrameObject);
    DataFrameType.tp_flags = Py_TPFLAGS_DEFAULT;
    DataFrameType.tp_doc =
        "A native vec batch (named vec columns; Arrow only at the edge).";
    DataFrameType.tp_dealloc = reinterpret_cast<destructor>(DataFrame_dealloc);
    DataFrameType.tp_as_mapping = &DataFrame_as_mapping;
    DataFrameType.tp_as_sequence = &DataFrame_as_sequence;
    DataFrameType.tp_methods = DataFrame_methods;
    DataFrameType.tp_getset = DataFrame_getset;
    DataFrameType.tp_new = nullptr;  // created only by the View
    if (register_type(m, &DataFrameType, "_DataFrame") < 0) return -1;

    static PyMethodDef from_arrow_def = {
        "_dataframe_from_arrow", vec_batch_from_arrow, METH_O,
        "vec_batch_from_arrow(table) -> DataFrame: import a pyarrow Table's "
        "columns into a native vec batch."};
    PyObject* fn = PyCFunction_NewEx(&from_arrow_def, nullptr, nullptr);
    if (!fn) return -1;
    if (PyModule_AddObject(m, "_dataframe_from_arrow", fn) < 0) {
        Py_DECREF(fn);
        return -1;
    }
    return 0;
}

}  // namespace dftracer::utils::python

#else   // !DFTRACER_UTILS_ENABLE_ARROW

namespace dftracer::utils::python {
int init_dataframe(PyObject*) { return 0; }
}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_ENABLE_ARROW
