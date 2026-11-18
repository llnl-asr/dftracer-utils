// SPDX-License-Identifier: MIT
// A native vec batch exposed to Python: a named set of dataframe::Columns (our
// SIMD columnar format) held as a single STRUCT column. Series access,
// filtering, and row ops stay in vec; Arrow/pandas/polars are produced only on
// an explicit to_arrow()/to_pandas()/to_polars(), zero-copy through the Arrow C
// interface.

#include <dftracer/utils/arrow/arrow.h>
#include <dftracer/utils/arrow/internal.h>
#include <dftracer/utils/arrow/ipc.h>
#include <dftracer/utils/arrow/stream.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/internal/frame_native.h>
#include <dftracer/utils/dataframe/kernels/kernels.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/plan.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/errc.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/python/core/arrow_helpers.h>
#include <dftracer/utils/python/core/call.h>
#include <dftracer/utils/python/core/errors.h>
#include <dftracer/utils/python/core/method.h>
#include <dftracer/utils/python/core/scalar_helpers.h>
#include <dftracer/utils/python/core/type_helpers.h>
#include <dftracer/utils/python/frame/agg_helpers.h>
#include <dftracer/utils/python/frame/columnar_eval.h>
#include <dftracer/utils/python/frame/dataframe.h>
#include <dftracer/utils/python/frame/frame_op_helpers.h>
#include <dftracer/utils/python/frame/join_helpers.h>
#include <dftracer/utils/python/frame/lazyframe.h>
#include <dftracer/utils/python/frame/series.h>

#include <cstdint>
#include <cstring>
#include <nanoarrow/nanoarrow.hpp>
#include <new>
#include <stdexcept>
#include <utility>

using dftracer::utils::python::core::as_native;
using dftracer::utils::python::core::guard;
using dftracer::utils::python::core::parse_kw;
using dftracer::utils::python::core::release_gil;
using dftracer::utils::python::core::without_gil;

using namespace dftracer::utils;

namespace {

using dataframe::DataFrame;
using dataframe::Series;
using dftracer::utils::python::core::parse_int_seq;
using dftracer::utils::python::core::parse_seq;
using dftracer::utils::python::core::parse_string_seq;
using dftracer::utils::python::frame::aggs_from_seq;
using dftracer::utils::python::frame::descending_from_obj;
using dftracer::utils::python::frame::group_agg_from_spec;
using dftracer::utils::python::frame::join_how_from_str;
using dftracer::utils::python::frame::join_keys_from_objs;
using dftracer::utils::python::frame::strings_from_str_or_seq;

// A DataFrame owns its columns as one STRUCT column: child(i) hands out a
// column sharing the struct's buffers (O(1)), and the struct exports to Arrow
// in one zero-copy step.
struct DataFrameObject {
    PyObject_HEAD PyObject* weaklist;
    Series st;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::SharedString> names;
    // The row index names, empty for none (DataFrame::index_names).
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::SharedString>
        index_names;
    // The lookup hash of the index labels (RowIndex::cache), kept here because
    // each call rebuilds a DataFrame view of this object.
    dftracer::utils::dstd::SharedPtr<dftracer::utils::dataframe::LabelCache>
        label_cache;
};

PyTypeObject DataFrameType;
// The type the factories allocate: DataFrameType until _bind_types gives the
// Python subclass.
PyTypeObject* g_dataframe_type = &DataFrameType;

}  // namespace

template <>
struct dftracer::utils::python::core::NativeTraits<DataFrameObject> {
    static PyTypeObject* type() { return &DataFrameType; }
    static constexpr const char* EXPECTED = "expected a DataFrame";
    static DataFrameObject* get(PyObject* o) {
        return reinterpret_cast<DataFrameObject*>(o);
    }
};

namespace {

PyObject* make_dataframe(DataFrame&& b) {
    // A column of a frame is labelled by the frame's row index alone.
    for (Series& column : b.columns)
        if (column.has_index()) column = column.without_index();
    Series st = Series::structs(b.names, std::move(b.columns));
    auto* self = reinterpret_cast<DataFrameObject*>(
        g_dataframe_type->tp_alloc(g_dataframe_type, 0));
    if (!self) return nullptr;
    new (&self->st) Series(std::move(st));
    new (&self->names)
        dftracer::utils::dstd::Vector<dftracer::utils::dstd::SharedString>(
            std::move(b.names));
    new (&self->index_names)
        dftracer::utils::dstd::Vector<dftracer::utils::dstd::SharedString>();
    new (&self->label_cache) dftracer::utils::dstd::SharedPtr<
        dftracer::utils::dataframe::LabelCache>();
    if (b.row_index) {
        self->index_names = std::move(b.row_index->names);
        self->label_cache = std::move(b.row_index->cache);
    }
    return reinterpret_cast<PyObject*>(self);
}

void DataFrame_dealloc(DataFrameObject* self) {
    if (self->weaklist)
        PyObject_ClearWeakRefs(reinterpret_cast<PyObject*>(self));
    self->label_cache.~SharedPtr();
    self->index_names.~vector();
    self->names.~vector();
    self->st.~Series();
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

std::int64_t index_of(const DataFrameObject* self,
                      dftracer::utils::dstd::StringView name) {
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
    if (!self->index_names.empty()) {
        b.row_index = dftracer::utils::dstd::make_unique<dataframe::RowIndex>();
        b.row_index->names = self->index_names;
        b.row_index->cache = self->label_cache;
    }
    return b;
}

// Wrap a batch_ops call over the frame `b` that returns a frame or a frame
// result, turning its error and any C++ exception into a typed Python error.
// `fn` runs with the GIL released for a large frame, so it touches no Python
// object. It reads a view of the frame taken under the GIL, as `_replace`
// rebinds a DataFrame object's columns in place, and a Series argument is
// shared under the GIL first, as `s.name = x` renames a Series object in place.
template <class Fn>
PyObject* run_batch_op(const DataFrameObject* b, Fn&& fn) {
    return guard([&]() -> PyObject* {
        const DataFrame frame = to_dataframe(b);
        auto out = without_gil(frame.num_rows(), [&] {
            if constexpr (std::is_invocable_v<Fn&, const DataFrame&>)
                return fn(frame);
            else
                return fn();
        });
        if constexpr (std::is_same_v<decltype(out), DataFrame>) {
            return make_dataframe(std::move(out));
        } else {
            if (!out) {
                dftracer::utils::python::core::set_py_error(out.error());
                return nullptr;
            }
            return make_dataframe(std::move(*out));
        }
    });
}

PyObject* DataFrame_filter(PyObject* self, PyObject* mask);
PyObject* DataFrame_select(PyObject* self, PyObject* args);

// df["a"] is a column; df[["a", "b"]] a projection and df[mask] the rows where
// a Bool Series holds (the pandas spellings), both under the frame op index
// rules.
PyObject* DataFrame_subscript(PyObject* self, PyObject* key) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    if (!PyUnicode_Check(key)) {
        if (dftracer::utils::python::frame::unwrap_vec_column(key))
            return DataFrame_filter(self, key);
        PyObject* names = PySequence_Tuple(key);
        if (!names) {
            PyErr_SetString(PyExc_TypeError,
                            "DataFrame key must be a column name, a list of "
                            "names or a Bool Series");
            return nullptr;
        }
        for (Py_ssize_t k = 0; k < PyTuple_GET_SIZE(names); ++k) {
            PyObject* text = PyObject_Str(PyTuple_GET_ITEM(names, k));
            if (!text) {
                Py_DECREF(names);
                return nullptr;
            }
            PyTuple_SET_ITEM(names, k, text);
        }
        PyObject* out = DataFrame_select(self, names);
        Py_DECREF(names);
        return out;
    }
    const char* name = PyUnicode_AsUTF8(key);
    if (!name) return nullptr;
    std::int64_t i = index_of(b, name);
    if (i < 0) {
        PyErr_Format(PyExc_KeyError, "no column named '%s'", name);
        return nullptr;
    }
    // The labels a column was built with are not the frame's: only its own
    // row index labels it.
    Series column = b->st.child(i).rename(name);
    if (column.has_index()) column = column.without_index();
    if (b->index_names.empty())
        return dftracer::utils::python::frame::wrap_vec_column(
            std::move(column));
    // The frame's row index travels with the column as its labels.
    dstd::Vector<dstd::StringView> level_names;
    dstd::Vector<Series> levels;
    for (const dstd::SharedString& index_name : b->index_names) {
        const std::int64_t at = index_of(b, index_name.c_str());
        if (at < 0) continue;
        level_names.emplace_back(index_name);
        levels.push_back(b->st.child(at));
    }
    Series labelled = column.with_index(level_names, levels);
    return dftracer::utils::python::frame::wrap_vec_column(
        labelled.valid() ? std::move(labelled) : std::move(column));
}

int DataFrame_contains(PyObject* self, PyObject* key) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return -1;
    // Only a name can be a column; anything else is simply not in the
    // frame, with no error left behind.
    if (!PyUnicode_Check(key)) return 0;
    const char* name = PyUnicode_AsUTF8(key);
    if (!name) return -1;
    return index_of(b, name) >= 0 ? 1 : 0;
}

// _replace(other): this frame takes the columns and the index names of
// `other`, in place. Kernels read a
// view taken under the GIL, so one running on another thread keeps the old
// columns.
PyObject* DataFrame_replace(PyObject* self, PyObject* other) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    DataFrameObject* o = as_native<DataFrameObject>(other);
    if (!b || !o) return nullptr;
    if (b == o) Py_RETURN_NONE;
    return guard([&]() -> PyObject* {
        DataFrame f = to_dataframe(o);
        Series st = Series::structs(f.names, std::move(f.columns));
        b->names = o->names;
        b->index_names = o->index_names;
        b->label_cache = o->label_cache;
        b->st = std::move(st);
        Py_RETURN_NONE;
    });
}

// share() -> DataFrame: a new object over the same columns and index names.
PyObject* DataFrame_share(PyObject* self, PyObject*) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    return guard(
        [&]() -> PyObject* { return make_dataframe(to_dataframe(b)); });
}

// index_names() -> list[str]: the row index columns, in order; [] for none.
PyObject* DataFrame_index_names(PyObject* self, PyObject*) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* list = PyList_New(static_cast<Py_ssize_t>(b->index_names.size()));
    if (!list) return nullptr;
    for (std::size_t i = 0; i < b->index_names.size(); ++i) {
        PyObject* s = PyUnicode_FromString(b->index_names[i].c_str());
        if (!s) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), s);
    }
    return list;
}

// with_index(names) -> DataFrame: this frame's columns under the row index
// `names` (a list of str; [] drops it). KeyError for a name that is no column,
// ValueError for a repeated name or a column of a nested type.
PyObject* DataFrame_with_index(PyObject* self, PyObject* names) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* seq =
        PySequence_Fast(names, "with_index() expects a list of names");
    if (!seq) return nullptr;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> keys;
    const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    for (Py_ssize_t k = 0; k < n; ++k) {
        PyObject* item = PySequence_Fast_GET_ITEM(seq, k);
        const char* text =
            PyUnicode_Check(item) ? PyUnicode_AsUTF8(item) : nullptr;
        if (!text) {
            if (!PyErr_Occurred())
                PyErr_SetString(PyExc_TypeError,
                                "with_index() names must be str");
            Py_DECREF(seq);
            return nullptr;
        }
        keys.emplace_back(text);
    }
    Py_DECREF(seq);
    return guard([&]() -> PyObject* {
        dftracer::utils::dstd::Vector<dftracer::utils::dstd::StringView> views;
        views.reserve(keys.size());
        for (const auto& key : keys) views.emplace_back(key);
        auto out = to_dataframe(b).with_index(views);
        if (!out) {
            dftracer::utils::python::core::set_py_error(out.error());
            return nullptr;
        }
        return make_dataframe(std::move(*out));
    });
}

PyObject* column_name_list(PyObject* self, bool hide_index) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* list = PyList_New(0);
    if (!list) return nullptr;
    for (std::size_t i = 0; i < b->names.size(); ++i) {
        bool is_index = false;
        if (hide_index)
            for (const auto& index_name : b->index_names)
                if (dftracer::utils::dstd::StringView(index_name) ==
                    dftracer::utils::dstd::StringView(b->names[i]))
                    is_index = true;
        if (is_index) continue;
        PyObject* s = PyUnicode_FromString(b->names[i].c_str());
        if (!s || PyList_Append(list, s) < 0) {
            Py_XDECREF(s);
            Py_DECREF(list);
            return nullptr;
        }
        Py_DECREF(s);
    }
    return list;
}

PyObject* DataFrame_keys(PyObject* self, PyObject*) {
    return column_name_list(self, true);
}

PyObject* DataFrame_filter(PyObject* self, PyObject* mask) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    const Series* m = dftracer::utils::python::frame::unwrap_vec_column(mask);
    if (!m) {
        PyErr_SetString(PyExc_TypeError,
                        "filter() expects a Series boolean mask");
        return nullptr;
    }
    const Series mask_copy = m->share();
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::filter(frame, mask_copy);
    });
}

// query(dsl=None, *, group_by=None, aggs=None, select=None, order_by=None,
// descending=False, limit=None) -> DataFrame: a query plan over this batch -
// filter by the DSL predicate (a SIMD mask, pandas.query-style), group_by/aggs,
// project `select`, sort by `order_by`, cap at `limit`. Raises
// DFTUtilsDuqlError on a parse error or a predicate with no columnar lowering
// (regex/like/ordered-string/absent field).
PyObject* DataFrame_query(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    const char* dsl = nullptr;
    const char* group_by = nullptr;
    PyObject* aggs_obj = nullptr;
    PyObject* select_obj = nullptr;
    const char* order_by = nullptr;
    int descending = 0;
    PyObject* limit_obj = nullptr;
    if (!parse_kw<"|zzOOzpO">(args, kwds,
                              {"dsl", "group_by", "aggs", "select", "order_by",
                               "descending", "limit"},
                              &dsl, &group_by, &aggs_obj, &select_obj,
                              &order_by, &descending, &limit_obj))
        return nullptr;

    return guard([&]() -> PyObject* {
        DataFrame bat = to_dataframe(b);
        dataframe::QueryPlan plan;
        dftracer::utils::dstd::Optional<duql::Query>
            q;  // keep root() alive across execute
        if (dsl && *dsl) {
            auto parsed = duql::Query::from_string(dsl);
            if (!parsed)
                throw dftracer::utils::python::core::PyErrorException(
                    dftracer::utils::dstd::Error{duql::DuqlErrc::Parse,
                                                 parsed.error().format()});
            q.emplace(std::move(*parsed));
            plan.where = &duql::detail::QueryAccess::root(*q);
        }
        if (group_by && *group_by) plan.group_by = group_by;
        if (aggs_obj && aggs_obj != Py_None) {
            if (!parse_seq<dataframe::GroupAgg>(
                    aggs_obj, "aggs must be a sequence of strings", plan.aggs,
                    [](PyObject* item,
                       dftracer::utils::dstd::Vector<dataframe::GroupAgg>& o) {
                        const char* s = PyUnicode_AsUTF8(item);
                        if (!s) return false;
                        return guard([&] {
                            o.push_back(group_agg_from_spec(s));
                            return true;
                        });
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
        auto out = dataframe::execute(plan, bat);
        if (!out) {
            dftracer::utils::python::core::set_py_error(out.error());
            return nullptr;
        }
        return make_dataframe(std::move(*out));
    });
}

PyObject* DataFrame_select(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> names;
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    for (Py_ssize_t i = 0; i < n; ++i) {
        const char* s = PyUnicode_AsUTF8(PyTuple_GET_ITEM(args, i));
        if (!s) {
            PyErr_SetString(PyExc_TypeError, "select() names must be strings");
            return nullptr;
        }
        names.emplace_back(s);
    }
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::select(frame, names);
    });
}

PyObject* DataFrame_rename(PyObject* self, PyObject* mapping) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    if (!PyDict_Check(mapping)) {
        PyErr_SetString(PyExc_TypeError, "rename() expects a {old: new} dict");
        return nullptr;
    }
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> new_names;
    new_names.reserve(b->names.size());
    for (const dftracer::utils::dstd::SharedString& name : b->names) {
        PyObject* v = PyDict_GetItemString(mapping, name.c_str());
        if (v) {
            const char* s = PyUnicode_AsUTF8(v);
            if (!s) {
                PyErr_SetString(PyExc_TypeError, "rename() values must be str");
                return nullptr;
            }
            new_names.emplace_back(s);
        } else {
            new_names.emplace_back(name.view());
        }
    }
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::rename(frame, new_names);
    });
}

PyObject* DataFrame_with_column(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    const char* name = nullptr;
    PyObject* col = nullptr;
    if (!PyArg_ParseTuple(args, "sO", &name, &col)) return nullptr;
    const Series* c = dftracer::utils::python::frame::unwrap_vec_column(col);
    if (!c) {
        PyErr_SetString(PyExc_TypeError,
                        "with_column() expects (name, Series)");
        return nullptr;
    }
    const Series col_copy = c->share();
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::with_column(frame, name, col_copy);
    });
}

PyObject* DataFrame_take(PyObject* self, PyObject* seq) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    dftracer::utils::dstd::Vector<std::int64_t> idx;
    if (!parse_int_seq(seq, "take() expects a sequence of ints", idx))
        return nullptr;
    return run_batch_op(
        b, [&](const DataFrame& frame) { return frame.take(idx); });
}

// _locate(keys) -> (positions, missing): the rows whose index labels equal the
// key columns, one per level (see DataFrame::locate).
PyObject* DataFrame_locate(PyObject* self, PyObject* keys_obj) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    dstd::Vector<Series> keys;
    if (!dftracer::utils::python::frame::series_list_from_py(
            keys_obj, "_locate: keys must be Series", keys))
        return nullptr;
    return guard([&]() -> PyObject* {
        const DataFrame frame = to_dataframe(b);
        return dftracer::utils::python::frame::located_to_py(
            without_gil(frame.num_rows(), [&] { return frame.locate(keys); }));
    });
}

// _locate_range(start, stop) -> (offset, length): the rows between two index
// label bounds (lists of one-row Series, empty for an open end).
PyObject* DataFrame_locate_range(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* start_obj = nullptr;
    PyObject* stop_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &start_obj, &stop_obj)) return nullptr;
    dstd::Vector<Series> start;
    dstd::Vector<Series> stop;
    if (!dftracer::utils::python::frame::series_list_from_py(
            start_obj, "_locate_range: bounds must be Series", start) ||
        !dftracer::utils::python::frame::series_list_from_py(
            stop_obj, "_locate_range: bounds must be Series", stop))
        return nullptr;
    return guard([&]() -> PyObject* {
        const DataFrame frame = to_dataframe(b);
        return dftracer::utils::python::frame::label_span_to_py(without_gil(
            frame.num_rows(), [&] { return frame.locate_range(start, stop); }));
    });
}

// _reindex(keys, method, limit, tolerance, fill) -> DataFrame on the index
// label key Series (one per level); see DataFrame::reindex.
PyObject* DataFrame_reindex(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* keys_obj = nullptr;
    PyObject* fill_obj = nullptr;
    PyObject* nanos_obj = Py_None;
    int method = 0;
    long long limit = -1;
    double tolerance = 0;
    if (!PyArg_ParseTuple(args, "OiLdO|O", &keys_obj, &method, &limit,
                          &tolerance, &fill_obj, &nanos_obj))
        return nullptr;
    dstd::Vector<Series> keys;
    if (!dftracer::utils::python::frame::series_list_from_py(
            keys_obj, "_reindex: keys must be Series", keys))
        return nullptr;
    if (method < 0 || method > 3) {
        PyErr_SetString(PyExc_ValueError, "_reindex: method must be 0..3");
        return nullptr;
    }
    dataframe::ReindexOptions options;
    options.method = static_cast<dataframe::FillMethod>(method);
    options.limit = limit;
    options.tolerance = tolerance;
    if (nanos_obj != Py_None) {
        options.tolerance_nanos = PyLong_AsLongLong(nanos_obj);
        if (options.tolerance_nanos == -1 && PyErr_Occurred()) return nullptr;
        options.tolerance_is_nanos = true;
    }
    Series fill;
    if (fill_obj != Py_None) {
        const Series* f =
            dftracer::utils::python::frame::unwrap_vec_column(fill_obj);
        if (!f) {
            PyErr_SetString(PyExc_TypeError, "_reindex: fill must be a Series");
            return nullptr;
        }
        fill = f->share();
    }
    return run_batch_op(b, [&](const DataFrame& frame) {
        return frame.reindex(keys, options, fill);
    });
}

// _align_rows(other, how, fill) -> (left, right): the rows of two frames on
// the joined index labels; see dataframe::align_rows.
PyObject* DataFrame_align_rows(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* other_obj = nullptr;
    PyObject* fill_obj = nullptr;
    int how = 0;
    if (!PyArg_ParseTuple(args, "OiO", &other_obj, &how, &fill_obj))
        return nullptr;
    DataFrameObject* o = as_native<DataFrameObject>(other_obj);
    if (!o) return nullptr;
    Series fill;
    if (fill_obj != Py_None) {
        const Series* f =
            dftracer::utils::python::frame::unwrap_vec_column(fill_obj);
        if (!f) {
            PyErr_SetString(PyExc_TypeError,
                            "_align_rows: fill must be a Series");
            return nullptr;
        }
        fill = f->share();
    }
    return guard([&]() -> PyObject* {
        const DataFrame left = to_dataframe(b);
        const DataFrame right = to_dataframe(o);
        auto r = without_gil(left.num_rows() + right.num_rows(), [&] {
            return dataframe::align_rows(
                left, right, static_cast<dataframe::JoinHow>(how), fill);
        });
        if (!r) {
            dftracer::utils::python::core::set_py_error(r.error());
            return nullptr;
        }
        PyObject* l = make_dataframe(std::move(r->left));
        PyObject* rt = l ? make_dataframe(std::move(r->right)) : nullptr;
        if (!rt) {
            Py_XDECREF(l);
            return nullptr;
        }
        return Py_BuildValue("NN", l, rt);
    });
}

PyObject* DataFrame_tail(PyObject* self, PyObject* arg) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    long long n = PyLong_AsLongLong(arg);
    if (n == -1 && PyErr_Occurred()) return nullptr;
    return run_batch_op(
        b, [&](const DataFrame& frame) { return dataframe::tail(frame, n); });
}

PyObject* DataFrame_reverse(PyObject* self, PyObject*) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    return run_batch_op(
        b, [&](const DataFrame& frame) { return dataframe::reverse(frame); });
}

PyObject* DataFrame_drop_nulls(PyObject* self, PyObject*) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::drop_nulls(frame);
    });
}

PyObject* DataFrame_fill_null(PyObject* self, PyObject* value) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    dftu_scalar s{};
    if (!py_to_scalar(value, &s)) return nullptr;
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::fill_null(frame, s);
    });
}

// Read a sequence (or single str) of column names into `out`. Returns false and
// sets a Python error on a non-string element.
bool names_from_obj(
    PyObject* obj,
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& out) {
    if (PyUnicode_Check(obj)) {
        out.emplace_back(PyUnicode_AsUTF8(obj));
        return true;
    }
    return parse_string_seq(obj, "expected a str or sequence of str", out);
}

// unique(subset=None): subset is a name or a sequence of names; None or an
// empty sequence keys on every column.
PyObject* DataFrame_unique(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* subset_obj = Py_None;
    if (!parse_kw<"|O">(args, kwds, {"subset"}, &subset_obj)) return nullptr;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> subset;
    if (subset_obj != Py_None && !names_from_obj(subset_obj, subset))
        return nullptr;
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::unique(frame, subset);
    });
}

PyObject* DataFrame_sort_by_multi(PyObject* self, PyObject* args,
                                  PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* names_obj = nullptr;
    PyObject* descending_obj = nullptr;
    PyObject* nulls_last_obj = nullptr;
    if (!parse_kw<"O|OO">(args, kwds, {"names", "descending", "nulls_last"},
                          &names_obj, &descending_obj, &nulls_last_obj))
        return nullptr;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> names;
    if (!parse_string_seq(names_obj, "names must be a sequence", names))
        return nullptr;

    // `descending` is either a single bool (broadcasts) or a sequence of bool,
    // one per name; a bare Python list is never truthy-coerced (that was the
    // old "any non-empty list sorts descending" bug).
    dftracer::utils::dstd::Vector<bool> descending;
    if (!descending_from_obj(descending_obj, descending)) return nullptr;
    dftracer::utils::dstd::Vector<bool> nulls_last;
    if (!nulls_last_obj)
        nulls_last.push_back(true);
    else if (!descending_from_obj(
                 nulls_last_obj, nulls_last,
                 "nulls_last must be a bool or a sequence of bool"))
        return nullptr;
    dftracer::utils::dstd::Vector<std::uint8_t> desc_flags, last_flags;
    for (bool d : descending) desc_flags.push_back(d ? 1 : 0);
    for (bool l : nulls_last) last_flags.push_back(l ? 1 : 0);
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::sort_by_multi(frame, names, desc_flags, last_flags);
    });
}

PyObject* DataFrame_sample(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    Py_ssize_t n = 0;
    unsigned long long seed = 0;
    if (!parse_kw<"n|K">(args, kwds, {"n", "seed"}, &n, &seed)) return nullptr;
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::sample(frame, static_cast<std::int64_t>(n),
                                 static_cast<std::uint64_t>(seed));
    });
}

PyObject* DataFrame_with_row_index(PyObject* self, PyObject* arg) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    const char* name = PyUnicode_AsUTF8(arg);
    if (!name) {
        PyErr_SetString(PyExc_TypeError, "with_row_index() expects a str");
        return nullptr;
    }
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::with_row_index(frame, name);
    });
}

PyObject* DataFrame_describe(PyObject* self, PyObject*) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    return run_batch_op(
        b, [&](const DataFrame& frame) { return dataframe::describe(frame); });
}

PyObject* DataFrame_null_count(PyObject* self, PyObject*) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::null_count(frame);
    });
}

// reduce(agg): `agg` is an aggregate name (sum, mean, ...) broadcast over
// every eligible column, one row.
PyObject* DataFrame_reduce(PyObject* self, PyObject* arg) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    const char* agg = PyUnicode_AsUTF8(arg);
    if (!agg) return nullptr;
    return run_batch_op(b,
                        [&](const DataFrame& frame)
                            -> dftracer::utils::dstd::Result<DataFrame> {
                            const auto op = dataframe::agg_from_string(agg);
                            if (!op) return op.error();
                            return dataframe::reduce(frame, *op);
                        });
}

PyObject* DataFrame_group_transform(PyObject* self, PyObject* args,
                                    PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    dftracer::utils::python::frame::GroupwiseArgs a;
    if (!dftracer::utils::python::frame::parse_groupwise_args(
            args, kwds, names_from_obj, a))
        return nullptr;
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::GroupBy(frame, a.keys)
            .transform(static_cast<dataframe::GroupwiseOp>(a.kind), a.n,
                       static_cast<dataframe::RankMethod>(a.method),
                       a.ascending != 0);
    });
}

// reduce_specs(agg, keys) -> list[str]: the "op:column:out" specs that
// broadcast `agg` over every eligible non-key column, for group_by.
PyObject* DataFrame_reduce_specs(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    const char* agg = nullptr;
    PyObject* keys_obj = Py_None;
    if (!PyArg_ParseTuple(args, "s|O", &agg, &keys_obj)) return nullptr;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> keys;
    if (keys_obj != Py_None && !names_from_obj(keys_obj, keys)) return nullptr;
    const auto op = dataframe::agg_from_string(agg);
    if (!op) {
        dftracer::utils::python::core::set_py_error(op.error());
        return nullptr;
    }
    const auto found = dataframe::reduce_specs(to_dataframe(b), *op, keys);
    if (!found) {
        dftracer::utils::python::core::set_py_error(found.error());
        return nullptr;
    }
    const dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> specs =
        dftracer::utils::python::frame::reduce_spec_strings(*found);
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
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    auto out = dataframe::is_duplicated(to_dataframe(b));
    if (!out) {
        dftracer::utils::python::core::set_py_error(out.error());
        return nullptr;
    }
    return dftracer::utils::python::frame::wrap_vec_column(std::move(*out));
}

PyObject* DataFrame_is_unique(PyObject* self, PyObject*) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    auto out = dataframe::is_unique(to_dataframe(b));
    if (!out) {
        dftracer::utils::python::core::set_py_error(out.error());
        return nullptr;
    }
    return dftracer::utils::python::frame::wrap_vec_column(std::move(*out));
}

PyObject* DataFrame_column_index(PyObject* self, PyObject* arg) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    const char* name = PyUnicode_AsUTF8(arg);
    if (!name) {
        PyErr_SetString(PyExc_TypeError, "column_index() expects a str");
        return nullptr;
    }
    return PyLong_FromLongLong(to_dataframe(b).column_index(name));
}

PyObject* DataFrame_head(PyObject* self, PyObject* arg) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    long long n = PyLong_AsLongLong(arg);
    if (n == -1 && PyErr_Occurred()) return nullptr;
    return run_batch_op(
        b, [&](const DataFrame& frame) { return dataframe::head(frame, n); });
}

PyObject* DataFrame_slice(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    long long offset = 0, length = 0;
    if (!PyArg_ParseTuple(args, "LL", &offset, &length)) return nullptr;
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::slice(frame, offset, length);
    });
}

PyObject* DataFrame_sort_by(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    const char* name = nullptr;
    int descending = 0;
    int nulls_last = 1;
    if (!parse_kw<"s|pp">(args, kwds, {"name", "descending", "nulls_last"},
                          &name, &descending, &nulls_last))
        return nullptr;
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::sort_by(frame, name, descending != 0,
                                  nulls_last != 0);
    });
}

PyObject* DataFrame_topk(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    const char* name = nullptr;
    Py_ssize_t k = 0;
    int largest = 1;
    if (!parse_kw<"sn|p">(args, kwds, {"name", "k", "largest"}, &name, &k,
                          &largest))
        return nullptr;
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::topk(frame, name, static_cast<std::int64_t>(k),
                               largest != 0);
    });
}

PyObject* DataFrame_partition_id(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* keys_obj = nullptr;
    Py_ssize_t n_parts = 0;
    if (!PyArg_ParseTuple(args, "On", &keys_obj, &n_parts)) return nullptr;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> keys;
    if (PyUnicode_Check(keys_obj)) {
        keys.emplace_back(PyUnicode_AsUTF8(keys_obj));
    } else if (!parse_string_seq(keys_obj, "keys must be a str or list",
                                 keys)) {
        return nullptr;
    }
    auto out = dataframe::partition_id(to_dataframe(b), keys,
                                       static_cast<std::int64_t>(n_parts));
    if (!out) {
        dftracer::utils::python::core::set_py_error(out.error());
        return nullptr;
    }
    return dftracer::utils::python::frame::wrap_vec_column(std::move(*out));
}

// hash_partition(keys, n_parts) -> list[DataFrame]; keys is a name or a
// sequence of names. The shuffle primitive for distributed group_by/join.
PyObject* DataFrame_hash_partition(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* keys_obj = nullptr;
    Py_ssize_t n_parts = 0;
    if (!PyArg_ParseTuple(args, "On", &keys_obj, &n_parts)) return nullptr;

    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> keys;
    if (PyUnicode_Check(keys_obj)) {
        keys.emplace_back(PyUnicode_AsUTF8(keys_obj));
    } else if (!parse_string_seq(keys_obj, "keys must be a str or list",
                                 keys)) {
        return nullptr;
    }

    auto found = dataframe::hash_partition(to_dataframe(b), keys,
                                           static_cast<std::int64_t>(n_parts));
    if (!found) {
        dftracer::utils::python::core::set_py_error(found.error());
        return nullptr;
    }
    dftracer::utils::dstd::Vector<DataFrame> parts = std::move(*found);
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

// One (op, ast, out[, param[, by_ast[, udaf[, interp]]]]) spec tuple, appended
// to `out`; `by_ast` and `udaf` may be None, `interp` is the interpolation code
// of a quantile.
bool agg_expr_spec_from_tuple(
    PyObject* t, dftracer::utils::dstd::Vector<dataframe::AggExprSpec>& out) {
    const Py_ssize_t tn = PyTuple_Check(t) ? PyTuple_GET_SIZE(t) : 0;
    if (tn < 3 || tn > 7) {
        PyErr_SetString(
            PyExc_TypeError,
            "each spec is (op, ast, out[, param[, by_ast[, udaf[, interp]]]])");
        return false;
    }
    dataframe::AggExprSpec s;
    s.op = static_cast<dataframe::AggOp>(PyLong_AsLong(PyTuple_GET_ITEM(t, 0)));
    if (tn >= 4) {
        s.param = PyFloat_AsDouble(PyTuple_GET_ITEM(t, 3));
        if (s.param == -1.0 && PyErr_Occurred()) return false;
    }
    PyObject* ast = PyTuple_GET_ITEM(t, 1);
    if (ast != Py_None) {
        dataframe::Expr v;
        if (!dftracer::utils::python::frame::build_expr_from_ast(ast, &v))
            return false;
        s.value = std::move(v);
    }
    if (tn >= 5) {
        PyObject* by_ast = PyTuple_GET_ITEM(t, 4);
        if (by_ast != Py_None) {
            dataframe::Expr by;
            if (!dftracer::utils::python::frame::build_expr_from_ast(by_ast,
                                                                     &by))
                return false;
            s.by = std::move(by);
        }
    }
    if (tn >= 6 && PyTuple_GET_ITEM(t, 5) != Py_None) {
        // (name, [arg_ast, ...]): the UDAF the spec calls and its
        // arguments after `value`.
        PyObject* call = PyTuple_GET_ITEM(t, 5);
        PyObject* fn = PyTuple_Check(call) && PyTuple_GET_SIZE(call) == 2
                           ? PyTuple_GET_ITEM(call, 0)
                           : nullptr;
        PyObject* arg_asts = fn ? PyTuple_GET_ITEM(call, 1) : nullptr;
        if (!fn || !PyList_Check(arg_asts)) {
            PyErr_SetString(PyExc_TypeError,
                            "a udaf spec is (name, [arg_ast, ...])");
            return false;
        }
        const char* fn_name = PyUnicode_AsUTF8(fn);
        if (!fn_name) return false;
        s.fn = fn_name;
        for (Py_ssize_t j = 0; j < PyList_GET_SIZE(arg_asts); ++j) {
            dataframe::Expr arg;
            if (!dftracer::utils::python::frame::build_expr_from_ast(
                    PyList_GET_ITEM(arg_asts, j), &arg))
                return false;
            s.args.push_back(std::move(arg));
        }
    }
    if (tn == 7) {
        const long interp = PyLong_AsLong(PyTuple_GET_ITEM(t, 6));
        if (interp == -1 && PyErr_Occurred()) return false;
        if (interp < 0 || interp > 4) {
            PyErr_SetString(PyExc_ValueError,
                            "a quantile interpolation code is 0 to 4");
            return false;
        }
        s.interp = static_cast<dataframe::Interpolation>(interp);
    }
    const char* out_name = PyUnicode_AsUTF8(PyTuple_GET_ITEM(t, 2));
    if (!out_name) return false;
    s.out = out_name;
    out.push_back(std::move(s));
    return true;
}

// _group_agg_expr(key, specs): the expression-aggregate workhorse. `key` is a
// column name or a sequence of names (composite key). `specs` is a list of
// (op_int, value_ast_or_None, out_name[, param[, by_ast]]); value ASTs
// reference this batch's columns by index. `by_ast` is ArgMax's maximized
// value. All value expressions compile in one CSE'd, pruned pass
// (dataframe::group_agg_expr). The Python GroupBy serializes to this.
PyObject* DataFrame_group_agg_expr(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* key_obj = nullptr;
    PyObject* specs = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &key_obj, &specs)) return nullptr;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> keys;
    if (!strings_from_str_or_seq(key_obj, keys)) return nullptr;
    DataFrame bat = to_dataframe(b);
    dftracer::utils::dstd::Vector<dataframe::Expr> key_exprs;
    key_exprs.reserve(keys.size());
    for (const dftracer::utils::dstd::String& key : keys) {
        std::int32_t ki = -1;
        for (std::size_t i = 0; i < bat.names.size(); ++i)
            if (bat.names[i] == key) {
                ki = static_cast<std::int32_t>(i);
                break;
            }
        if (ki < 0) {
            PyErr_Format(PyExc_KeyError, "group_by: no column named %s",
                         key.c_str());
            return nullptr;
        }
        key_exprs.push_back(dataframe::expr_col(ki));
    }

    dftracer::utils::dstd::Vector<dataframe::AggExprSpec> aggs;
    if (!parse_seq<dataframe::AggExprSpec>(
            specs, "specs must be a sequence", aggs,
            [](PyObject* t,
               dftracer::utils::dstd::Vector<dataframe::AggExprSpec>& o) {
                return agg_expr_spec_from_tuple(t, o);
            }))
        return nullptr;

    dftracer::utils::dstd::Vector<const Series*> inputs;
    inputs.reserve(bat.columns.size());
    for (const Series& c : bat.columns) inputs.push_back(&c);
    // An aggregate over a per-batch Python udf runs on the Python lane, which
    // needs the GIL; nothing in this region touches Python.
    auto grouped = release_gil([&] {
        return dataframe::group_agg_expr(key_exprs, aggs, inputs, keys);
    });
    if (!grouped) {
        dftracer::utils::python::core::set_py_error(grouped.error());
        return nullptr;
    }
    return make_dataframe(std::move(*grouped));
}

// Two-step / expression forms delegate to the Python GroupBy, which serializes
// each spec and calls _group_agg_expr. `args[0:nk]` are the key names (a tuple,
// even for one key); `args[nk:]` are the specs.
PyObject* delegate_groupby(PyObject* self, PyObject* args, Py_ssize_t nk) {
    PyObject* mod = PyImport_ImportModule("dftracer.utils.groupby");
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
    DataFrameObject* b = as_native<DataFrameObject>(self);
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
        const dftracer::utils::dstd::String sv(s);
        if (sv == "count" ||
            sv.find(':') != dftracer::utils::dstd::String::npos)
            break;
    }
    if (nk == 0) nk = 1;  // args[0] is always at least one key
    bool rest_legacy = n > nk;
    for (Py_ssize_t i = nk; i < n && rest_legacy; ++i)
        rest_legacy = PyUnicode_Check(PyTuple_GET_ITEM(args, i)) != 0;
    if (n == nk || !rest_legacy) return delegate_groupby(self, args, nk);

    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> keys;
    keys.reserve(static_cast<std::size_t>(nk));
    for (Py_ssize_t i = 0; i < nk; ++i) {
        const char* s = PyUnicode_AsUTF8(PyTuple_GET_ITEM(args, i));
        if (!s) return nullptr;
        keys.emplace_back(s);
    }
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> specs;
    specs.reserve(static_cast<std::size_t>(n - nk));
    for (Py_ssize_t i = nk; i < n; ++i) {
        const char* s = PyUnicode_AsUTF8(PyTuple_GET_ITEM(args, i));
        if (!s) return nullptr;
        specs.emplace_back(s);
    }
    return run_batch_op(b, [&](const DataFrame& frame) {
        dftracer::utils::dstd::Vector<dataframe::GroupAgg> aggs;
        aggs.reserve(specs.size());
        for (const dftracer::utils::dstd::String& spec : specs)
            aggs.push_back(group_agg_from_spec(spec));
        return dataframe::group_by(frame, keys, aggs);
    });
}

// join(other, how="inner", on=1): equi-join on the first `on` (leading) key
// columns; how is inner|left|right|full|semi|anti.
PyObject* DataFrame_join(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* other = nullptr;
    PyObject* on = nullptr;
    const char* how = "inner";
    PyObject* left_on = nullptr;
    PyObject* right_on = nullptr;
    const char* suffix = "_right";
    int nulls_equal = 0;
    if (!parse_kw<"O|OsOOsp">(args, kwds,
                              {"other", "on", "how", "left_on", "right_on",
                               "suffix", "nulls_equal"},
                              &other, &on, &how, &left_on, &right_on, &suffix,
                              &nulls_equal))
        return nullptr;
    DataFrameObject* o = as_native<DataFrameObject>(other);
    if (!o) return nullptr;
    dataframe::JoinHow jh;
    if (!join_how_from_str(how, jh)) return nullptr;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> l;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> r;
    if (!join_keys_from_objs(on, left_on, right_on, jh, l, r)) return nullptr;
    const DataFrame other_frame = to_dataframe(o);
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::join(frame, other_frame, l, r, jh, suffix,
                               nulls_equal != 0);
    });
}

PyObject* DataFrame_compare_agg(PyObject* self, PyObject* args) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* other = nullptr;
    Py_ssize_t n_key = 1;
    if (!PyArg_ParseTuple(args, "On", &other, &n_key)) return nullptr;
    DataFrameObject* o = as_native<DataFrameObject>(other);
    if (!o) return nullptr;
    const DataFrame other_frame = to_dataframe(o);
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::compare_agg(frame, other_frame,
                                      static_cast<std::int64_t>(n_key));
    });
}

// unpivot(id_vars, value_vars) / melt: reshape wide -> long.
PyObject* DataFrame_unpivot(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* id_obj = nullptr;
    PyObject* val_obj = nullptr;
    if (!parse_kw<"OO">(args, kwds, {"id_vars", "value_vars"}, &id_obj,
                        &val_obj))
        return nullptr;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> id_vars,
        value_vars;
    if (!names_from_obj(id_obj, id_vars)) return nullptr;
    if (!names_from_obj(val_obj, value_vars)) return nullptr;
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::unpivot(frame, id_vars, value_vars);
    });
}

PyObject* DataFrame_explode(PyObject* self, PyObject* arg) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    const char* name = PyUnicode_AsUTF8(arg);
    if (!name) {
        PyErr_SetString(PyExc_TypeError, "explode() expects a column name");
        return nullptr;
    }
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::explode(frame, name);
    });
}

PyObject* DataFrame_window(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* part = nullptr;
    PyObject* order = nullptr;
    PyObject* specs = nullptr;
    if (!parse_kw<"OOO">(args, kwds, {"partition_by", "order_by", "specs"},
                         &part, &order, &specs))
        return nullptr;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> pcols;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> ocols;
    if (!parse_string_seq(part, "window: partition_by must be names", pcols) ||
        !parse_string_seq(order, "window: order_by must be names", ocols))
        return nullptr;
    dftracer::utils::python::frame::WindowSpecs parsed;
    if (!dftracer::utils::python::frame::parse_window_specs(specs, parsed))
        return nullptr;
    dftracer::utils::dstd::Vector<dataframe::WindowColumn> specv;
    specv.reserve(parsed.specs.size());
    for (const dftu_window_spec& w : parsed.specs)
        specv.push_back(dataframe::window_column(w));
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::window(frame, pcols, ocols, specv);
    });
}

PyObject* DataFrame_gap_fill(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* part = nullptr;
    const char* time = nullptr;
    long long bucket = 0;
    PyObject* values = nullptr;
    const char* mode = nullptr;
    PyObject* start_obj = Py_None;
    PyObject* end_obj = Py_None;
    if (!parse_kw<"OsLOs|OO">(args, kwds,
                              {"partition_by", "time", "bucket", "values",
                               "mode", "start", "end"},
                              &part, &time, &bucket, &values, &mode, &start_obj,
                              &end_obj))
        return nullptr;
    dftu_gap_fill_mode mode_code;
    if (!dftracer::utils::python::frame::gap_fill_mode_from_str(mode,
                                                                &mode_code))
        return nullptr;
    const auto m = static_cast<dataframe::GapFillMode>(mode_code);
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> pcols;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> vcols;
    if (!parse_string_seq(part, "gap_fill: partition_by must be names",
                          pcols) ||
        !parse_string_seq(values, "gap_fill: values must be names", vcols))
        return nullptr;
    dftracer::utils::dstd::Optional<
        dftracer::utils::dstd::Pair<std::int64_t, std::int64_t>>
        range;
    if (start_obj != Py_None) {
        const long long s = PyLong_AsLongLong(start_obj);
        const long long e = PyLong_AsLongLong(end_obj);
        if (PyErr_Occurred()) return nullptr;
        range = dftracer::utils::dstd::Pair<std::int64_t, std::int64_t>{
            static_cast<std::int64_t>(s), static_cast<std::int64_t>(e)};
    }
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::gap_fill(frame, pcols, time,
                                   static_cast<std::int64_t>(bucket), vcols, m,
                                   range);
    });
}

PyObject* DataFrame_asof(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* other = nullptr;
    const char* on = nullptr;
    PyObject* by = nullptr;
    const char* direction = nullptr;
    PyObject* tol_obj = Py_None;
    if (!parse_kw<"OsOsO">(args, kwds,
                           {"other", "on", "by", "direction", "tolerance"},
                           &other, &on, &by, &direction, &tol_obj))
        return nullptr;
    DataFrameObject* o = as_native<DataFrameObject>(other);
    if (!o) return nullptr;
    dftu_asof_direction dir_code;
    if (!dftracer::utils::python::frame::asof_direction_from_str(direction,
                                                                 &dir_code))
        return nullptr;
    const auto dir = static_cast<dataframe::AsofDirection>(dir_code);
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> equi;
    if (!parse_string_seq(by, "asof: by must be names", equi)) return nullptr;
    dftracer::utils::dstd::Optional<double> tol;
    if (tol_obj != Py_None) {
        const double t = PyFloat_AsDouble(tol_obj);
        if (PyErr_Occurred()) return nullptr;
        if (!(t >= 0)) {
            PyErr_SetString(PyExc_ValueError,
                            "asof: tolerance must not be negative");
            return nullptr;
        }
        tol = t;
    }
    const DataFrame other_frame = to_dataframe(o);
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::asof(frame, other_frame, on, equi, dir, tol);
    });
}

PyObject* DataFrame_interval(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    PyObject* other = nullptr;
    const char* point = nullptr;
    const char* lo = nullptr;
    const char* hi = nullptr;
    PyObject* by = nullptr;
    int outer = 0;
    if (!parse_kw<"OsssOp">(args, kwds,
                            {"other", "point", "lo", "hi", "by", "outer"},
                            &other, &point, &lo, &hi, &by, &outer))
        return nullptr;
    DataFrameObject* o = as_native<DataFrameObject>(other);
    if (!o) return nullptr;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> equi;
    if (!parse_string_seq(by, "interval: by must be names", equi))
        return nullptr;
    const DataFrame other_frame = to_dataframe(o);
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::interval(frame, other_frame, point, lo, hi, equi,
                                   outer != 0);
    });
}

PyObject* DataFrame_unnest(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    const char* name = nullptr;
    int keep_empty = 0;
    if (!parse_kw<"s|p">(args, kwds, {"column", "keep_empty"}, &name,
                         &keep_empty))
        return nullptr;
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::unnest(frame, name, keep_empty != 0);
    });
}

PyObject* DataFrame_to_dummies(PyObject* self, PyObject* arg) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    const char* name = PyUnicode_AsUTF8(arg);
    if (!name) {
        PyErr_SetString(PyExc_TypeError, "to_dummies() expects a column name");
        return nullptr;
    }
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::to_dummies(frame, name);
    });
}

// pivot(index, columns, values, agg="first"): long -> wide.
PyObject* DataFrame_pivot(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    const char* index = nullptr;
    const char* columns = nullptr;
    const char* values = nullptr;
    const char* agg = "first";
    if (!parse_kw<"sss|s">(args, kwds, {"index", "columns", "values", "agg"},
                           &index, &columns, &values, &agg))
        return nullptr;
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::pivot(frame, index, columns, values, agg);
    });
}

// group_by_dynamic(time_col, every, period=None, aggs=[...]): tumbling/sliding
// time-window aggregation. aggs mirror the group_by string-spec format.
PyObject* DataFrame_group_by_dynamic(PyObject* self, PyObject* args,
                                     PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    const char* time_col = nullptr;
    long long every = 0;
    PyObject* period_obj = Py_None;
    PyObject* aggs_obj = Py_None;
    PyObject* origin_obj = Py_None;  // int (explicit) or "min"
    if (!parse_kw<"sL|OOO">(
            args, kwds, {"time_col", "every", "period", "aggs", "origin"},
            &time_col, &every, &period_obj, &aggs_obj, &origin_obj))
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
            if (dftracer::utils::dstd::String(s) != "min") {
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
    dftracer::utils::dstd::Vector<dataframe::GroupAgg> aggs;
    if (aggs_obj && aggs_obj != Py_None && !aggs_from_seq(aggs_obj, aggs))
        return nullptr;
    return run_batch_op(b, [&](const DataFrame& frame) {
        return dataframe::group_by_dynamic(frame, time_col,
                                           static_cast<std::int64_t>(every),
                                           period, aggs, origin, origin_min);
    });
}

PyObject* DataFrame_concat(PyObject* self, PyObject* args, PyObject* kwds) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
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
    dftracer::utils::dstd::Vector<DataFrame> owned;
    owned.reserve(1 + static_cast<std::size_t>(PyTuple_GET_SIZE(args)));
    owned.push_back(to_dataframe(b));
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(args); ++i) {
        DataFrameObject* o =
            as_native<DataFrameObject>(PyTuple_GET_ITEM(args, i));
        if (!o) return nullptr;
        owned.push_back(to_dataframe(o));
    }
    dftracer::utils::dstd::Vector<const DataFrame*> parts;
    parts.reserve(owned.size());
    for (const DataFrame& x : owned) parts.push_back(&x);
    return run_batch_op(b, [&] { return dataframe::concat(parts, how); });
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
        dftracer::utils::dstd::UniquePtr<ArrowSchema>(s).reset();
    }
}
void array_capsule_destructor(PyObject* cap) {
    auto* a =
        static_cast<ArrowArray*>(PyCapsule_GetPointer(cap, "arrow_array"));
    if (a) {
        if (a->release) a->release(a);
        dftracer::utils::dstd::UniquePtr<ArrowArray>(a).reset();
    }
}

PyObject* DataFrame_arrow_c_array(PyObject* self, PyObject*) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    dftracer::utils::dstd::UniquePtr<ArrowSchema> schema;
    dftracer::utils::dstd::UniquePtr<ArrowArray> array;
    schema = dftracer::utils::dstd::make_unique<ArrowSchema>();
    array = dftracer::utils::dstd::make_unique<ArrowArray>();
    auto batch = arrow::to_arrow(to_dataframe(b));
    if (!batch) {
        dftracer::utils::python::core::set_py_error(batch.error());
        return nullptr;
    }
    batch->release_into(schema.get(), array.get());
    PyObject* scap =
        PyCapsule_New(schema.get(), "arrow_schema", schema_capsule_destructor);
    if (!scap) {
        if (schema->release) schema->release(schema.get());
        if (array->release) array->release(array.get());
        return nullptr;
    }
    schema.release();
    PyObject* acap =
        PyCapsule_New(array.get(), "arrow_array", array_capsule_destructor);
    if (!acap) {
        Py_DECREF(scap);
        if (array->release) array->release(array.get());
        return nullptr;
    }
    array.release();
    PyObject* result = PyTuple_Pack(2, scap, acap);
    Py_DECREF(scap);
    Py_DECREF(acap);
    return result;
}

// __arrow_c_stream__: a stream of the struct (one batch per chunk of a
// collected frame), so pa.table(batch) / pl.from_arrow(batch) import every
// column zero-copy without an explicit to_arrow(). The single struct array
// carries all fields.
PyObject* DataFrame_arrow_c_stream(PyObject* self, PyObject* args,
                                   PyObject* kwds) {
    PyObject* requested_schema = Py_None;
    if (!parse_kw<"|O">(args, kwds, {"requested_schema"}, &requested_schema))
        return nullptr;
    if (!dftracer::utils::python::core::no_requested_schema(requested_schema))
        return nullptr;
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    const dataframe::DataFrame frame = to_dataframe(b);
    auto stream = dftracer::utils::dstd::make_unique<ArrowArrayStream>();
    auto status = arrow::to_arrow_stream(frame, stream.get());
    if (!status) {
        dftracer::utils::python::core::set_py_error(status.error());
        return nullptr;
    }
    return dftracer::utils::python::core::stream_capsule(std::move(stream));
}

// to_ipc() -> bytes: the frame serialized as an Arrow IPC stream (schema + one
// record batch + EOS), so a consumer needs no pyarrow to produce .arrow bytes.
PyObject* DataFrame_to_ipc(PyObject* self, PyObject*) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    return guard([&]() -> PyObject* {
        auto writer = arrow::Writer::to_buffer();
        const DataFrame frame = to_dataframe(b);
        auto status = without_gil(frame.num_rows(), [&] {
            auto written = writer.write(frame);
            if (written) written = writer.finish();
            return written;
        });
        if (!status) {
            dftracer::utils::python::core::set_py_error(status.error());
            return nullptr;
        }
        const auto bytes = writer.take_bytes();
        return PyBytes_FromStringAndSize(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<Py_ssize_t>(bytes.size()));
    });
}

PyObject* DataFrame_to_bytes(PyObject* self, PyObject*) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    return guard([&]() -> PyObject* {
        const DataFrame frame = to_dataframe(b);
        const auto bytes = without_gil(frame.num_rows(), [&] {
            return dataframe::frame_to_native(frame);
        });
        if (!bytes) {
            PyErr_SetString(PyExc_TypeError, bytes.error().c_str());
            return nullptr;
        }
        return PyBytes_FromStringAndSize(
            bytes->data(), static_cast<Py_ssize_t>(bytes->size()));
    });
}

PyObject* dataframe_from_bytes(PyObject* /*self*/, PyObject* obj) {
    Py_buffer view;
    if (PyObject_GetBuffer(obj, &view, PyBUF_SIMPLE) < 0) return nullptr;
    auto f = dataframe::frame_from_native(
        dftracer::utils::dstd::StringView(static_cast<const char*>(view.buf),
                                          static_cast<std::size_t>(view.len)));
    PyBuffer_Release(&view);
    if (!f) {
        PyErr_SetString(PyExc_ValueError,
                        "not a DataFrame written by this version");
        return nullptr;
    }
    return make_dataframe(std::move(*f));
}

PyObject* DataFrame_get_num_rows(PyObject* self, void*) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    return PyLong_FromLongLong(b->st.length());
}
PyObject* DataFrame_get_num_columns(PyObject* self, void*) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    return PyLong_FromSsize_t(static_cast<Py_ssize_t>(b->names.size()));
}
PyObject* DataFrame_get_column_names(PyObject* self, void*) {
    return column_name_list(self, false);
}

// lazy() -> LazyFrame: start a deferred query over a copy of this batch.
PyObject* DataFrame_lazy(PyObject* self, PyObject*) {
    DataFrameObject* b = as_native<DataFrameObject>(self);
    if (!b) return nullptr;
    return guard([&] {
        return dftracer::utils::python::frame::wrap_lazyframe(
            dataframe::lazy(to_dataframe(b)));
    });
}

PyMethodDef DataFrame_methods[] = {
    {"lazy", DataFrame_lazy, METH_NOARGS,
     "lazy() -> LazyFrame, a deferred query over this batch."},
    {"keys", DataFrame_keys, METH_NOARGS,
     "Series names, in order; the row index columns are not listed."},
    {"_replace", DataFrame_replace, METH_O,
     "_replace(other) -> None\n\nTake the columns of `other`, in place."},
    {"share", DataFrame_share, METH_NOARGS,
     "share() -> DataFrame\n\nA new DataFrame over the same columns and "
     "index names."},
    {"index_names", DataFrame_index_names, METH_NOARGS,
     "index_names() -> list[str]\n\nThe row index columns, in order; [] "
     "when the frame has no index."},
    {"with_index", DataFrame_with_index, METH_O,
     "with_index(names) -> DataFrame\n\nThe same columns under the row index "
     "`names` ([] drops the index)."},
    {"filter", DataFrame_filter, METH_O,
     "filter(mask) -> DataFrame keeping rows where the Bool mask is true."},
    {"select", DataFrame_select, METH_VARARGS,
     "select(*names) -> DataFrame projecting the named columns (zero-copy)."},
    {"rename", DataFrame_rename, METH_O,
     "rename({old: new}) -> DataFrame with columns renamed (zero-copy)."},
    {"with_column", DataFrame_with_column, METH_VARARGS,
     "with_column(name, Series) -> DataFrame adding/replacing a column."},
    {"_locate", DataFrame_locate, METH_O,
     "_locate(keys) -> (positions, missing): the rows whose index labels equal "
     "the key Series (one per level), and the keys no label matches."},
    {"_locate_range", DataFrame_locate_range, METH_VARARGS,
     "_locate_range(start, stop) -> (offset, length): the rows between two "
     "index label bounds (lists of one-row Series, empty for an open end)."},
    {"_reindex", DataFrame_reindex, METH_VARARGS,
     "_reindex(keys, method, limit, tolerance, fill, nanos=None) -> DataFrame "
     "on the index "
     "label key Series (one per level); method 0 none, 1 pad, 2 backfill, 3 "
     "nearest, limit -1 for none, tolerance infinite for none, fill a one-row "
     "Series or None, nanos a tolerance as whole nanoseconds for temporal "
     "labels."},
    {"_align_rows", DataFrame_align_rows, METH_VARARGS,
     "_align_rows(other, how, fill) -> (left, right): the rows of two frames "
     "on the joined index labels; how is 0 inner, 1 left, 2 right, 3 outer."},
    {"take", DataFrame_take, METH_O,
     "take(indices) -> DataFrame gathered at the given row indices."},
    {"column_index",
     DFTU_PYCFUNCTION((dftracer::utils::python::core::kw_one<
                       "column_index", "name", DataFrame_column_index>)),
     METH_VARARGS | METH_KEYWORDS,
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
     "sort_by_multi(names, descending=False, nulls_last=True) -> DataFrame "
     "stably sorted "
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
    {"_group_transform", DFTU_PYCFUNCTION(DataFrame_group_transform),
     METH_VARARGS | METH_KEYWORDS,
     "_group_transform(keys, kind, n=0, method='average', ascending=True) -> "
     "DataFrame: the group-wise transform `kind` (cumsum, cummax, cummin, "
     "cumcount, shift, diff, pct_change, rank, ngroup, head, tail, nth) over "
     "the groups of `keys`, one value per input row in input order."},
    {"_reduce_specs", DataFrame_reduce_specs, METH_VARARGS,
     "_reduce_specs(agg, keys=None) -> list[str]: the 'op:column:out' specs "
     "that broadcast `agg` over every eligible non-key column, for group_by."},
    {"is_duplicated", DataFrame_is_duplicated, METH_NOARGS,
     "is_duplicated() -> Bool Series, true where the whole row is duplicated."},
    {"is_unique", DataFrame_is_unique, METH_NOARGS,
     "is_unique() -> Bool Series, true where the whole row is unique."},
    {"slice", DataFrame_slice, METH_VARARGS,
     "slice(offset, length) -> DataFrame of a row range."},
    {"sort_by", DFTU_PYCFUNCTION(DataFrame_sort_by),
     METH_VARARGS | METH_KEYWORDS,
     "sort_by(name, descending=False, nulls_last=True) -> DataFrame ordered "
     "by a column."},
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
     "functions; specs are normalized 12-tuples."},
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
     "(op_int, value_ast|None, out_name[, param[, by_ast[, udaf[, interp]]]]). "
     "Internal: the "
     "GroupBy expression path (CSE across value expressions + pruner)."},
    {"join", DFTU_PYCFUNCTION(DataFrame_join), METH_VARARGS | METH_KEYWORDS,
     "join(other, on=None, how='inner', left_on=None, right_on=None, "
     "suffix='_right') -> DataFrame hash-joined on the key columns; "
     "how=inner|left|right|outer|full|semi|anti|cross."},
    {"compare_agg", DataFrame_compare_agg, METH_VARARGS,
     "compare_agg(variant, n_key) -> DataFrame: FULL-join two aggregation "
     "results on the first n_key group-key columns and append delta_/pct_ per "
     "numeric metric (the CompareView result)."},
    {"partition_id",
     DFTU_PYCFUNCTION(
         (dftracer::utils::python::core::kw_pair<
             "partition_id", DataFrame_partition_id, "keys", "n_parts">)),
     METH_VARARGS | METH_KEYWORDS,
     "partition_id(keys, n_parts) -> Int32 Series: the part in [0, n_parts) "
     "each row lands in under a stable hash of the key columns."},
    {"hash_partition", DataFrame_hash_partition, METH_VARARGS,
     "hash_partition(keys, n_parts) -> list[DataFrame] partitioned by a stable "
     "hash of the key columns (the distributed shuffle primitive)."},
    {"__arrow_c_array__", DataFrame_arrow_c_array, METH_VARARGS,
     "Arrow PyCapsule export of the struct: (schema_capsule, array_capsule)."},
    {"__arrow_c_stream__", DFTU_PYCFUNCTION(DataFrame_arrow_c_stream),
     METH_VARARGS | METH_KEYWORDS,
     "Arrow PyCapsule stream export (one struct batch); pa.table(batch) uses "
     "this to import every column zero-copy."},
    {"to_ipc", DataFrame_to_ipc, METH_NOARGS,
     "to_ipc() -> bytes: the frame as an Arrow IPC stream (schema + one record "
     "batch + EOS); no pyarrow needed to produce .arrow bytes."},
    {"to_bytes", DataFrame_to_bytes, METH_NOARGS,
     "to_bytes() -> bytes: the frame in the native frame format."},
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

// Whether field `i` of `obj`'s schema carries the `arrow.json` extension in
// its metadata. A pyarrow without the registered extension type (before 18)
// keeps the name only there, and an Array's own export has no field metadata.
bool field_is_json(PyObject* obj, Py_ssize_t i) {
    PyObject* schema = PyObject_GetAttrString(obj, "schema");
    if (!schema) {
        PyErr_Clear();
        return false;
    }
    PyObject* field = PyObject_CallMethod(schema, "field", "n", i);
    Py_DECREF(schema);
    if (!field) {
        PyErr_Clear();
        return false;
    }
    PyObject* md = PyObject_GetAttrString(field, "metadata");
    Py_DECREF(field);
    if (!md) {
        PyErr_Clear();
        return false;
    }
    bool json = false;
    if (PyDict_Check(md)) {
        // pyarrow keys the metadata dict by bytes.
        PyObject* key = PyBytes_FromString("ARROW:extension:name");
        PyObject* name = key ? PyDict_GetItem(md, key) : nullptr;
        json = name && PyBytes_Check(name) &&
               std::strcmp(PyBytes_AsString(name), "arrow.json") == 0;
        Py_XDECREF(key);
    }
    Py_DECREF(md);
    return json;
}

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
            arr ? dftracer::utils::python::frame::column_from_arrow(arr)
                : Series{};
        Py_XDECREF(arr);
        Py_XDECREF(ca);
        if (col.valid() && !col.is_json() &&
            col.type() == dftracer::utils::dataframe::TypeId::String &&
            field_is_json(obj, i))
            col = col.as_json();
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

// _dataframe_from_columns(names, series) -> DataFrame: a frame of native
// Series, no Arrow involved.
PyObject* dataframe_from_columns(PyObject* /*self*/, PyObject* args) {
    PyObject* names_obj = nullptr;
    PyObject* series_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &names_obj, &series_obj)) return nullptr;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> names;
    if (!parse_string_seq(names_obj, "names must be strings", names))
        return nullptr;
    dftracer::utils::dstd::Vector<Series> cols;
    if (!parse_seq<Series>(
            series_obj, "series must be a sequence", cols,
            [](PyObject* item, dftracer::utils::dstd::Vector<Series>& o) {
                const Series* col =
                    dftracer::utils::python::frame::unwrap_vec_column(item);
                if (!col) {
                    PyErr_SetString(PyExc_TypeError, "expected native Series");
                    return false;
                }
                o.push_back(col->share());
                return true;
            }))
        return nullptr;
    if (cols.size() != names.size()) {
        PyErr_SetString(PyExc_ValueError, "names and series differ in length");
        return nullptr;
    }
    DataFrame b;
    for (std::size_t i = 0; i < cols.size(); ++i) {
        b.names.push_back(dstd::SharedString{names[i]});
        b.columns.push_back(std::move(cols[i]));
    }
    if (!b.columns.empty()) {
        const std::int64_t rows = b.columns.front().length();
        for (const Series& c : b.columns)
            if (c.length() != rows) {
                PyErr_SetString(PyExc_ValueError, "columns differ in length");
                return nullptr;
            }
    }
    return make_dataframe(std::move(b));
}

}  // namespace

namespace dftracer::utils::python::frame {

TypeBinding dataframe_binding() {
    return {"DataFrame", &DataFrameType, &g_dataframe_type};
}

PyObject* wrap_dataframe(dftracer::utils::dataframe::DataFrame&& batch) {
    return make_dataframe(std::move(batch));
}

int init_dataframe(PyObject* m) {
    DataFrame_as_mapping.mp_subscript = DataFrame_subscript;
    DataFrame_as_sequence.sq_contains = DataFrame_contains;
    DataFrameType = {};
    DataFrameType.ob_base = {PyObject_HEAD_INIT(nullptr) 0};
    DataFrameType.tp_name = "dftracer_utils_ext.DataFrame";
    DataFrameType.tp_basicsize = sizeof(DataFrameObject);
    DataFrameType.tp_weaklistoffset = WEAKLIST_OFFSET;
    DataFrameType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    DataFrameType.tp_doc =
        "A native vec batch (named vec columns; Arrow only at the edge).";
    DataFrameType.tp_dealloc = reinterpret_cast<destructor>(DataFrame_dealloc);
    DataFrameType.tp_as_mapping = &DataFrame_as_mapping;
    DataFrameType.tp_as_sequence = &DataFrame_as_sequence;
    DataFrameType.tp_methods = DataFrame_methods;
    DataFrameType.tp_getset = DataFrame_getset;
    DataFrameType.tp_new = nullptr;  // created only by a trace view
    if (register_type(m, &DataFrameType, "DataFrame") < 0) return -1;

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
    static PyMethodDef from_columns_def = {
        "_dataframe_from_columns", dataframe_from_columns, METH_VARARGS,
        "_dataframe_from_columns(names, series) -> DataFrame of native "
        "Series."};
    PyObject* cols_fn = PyCFunction_NewEx(&from_columns_def, nullptr, nullptr);
    if (!cols_fn) return -1;
    if (PyModule_AddObject(m, "_dataframe_from_columns", cols_fn) < 0) {
        Py_DECREF(cols_fn);
        return -1;
    }
    static PyMethodDef from_bytes_def = {
        "_dataframe_from_bytes", dataframe_from_bytes, METH_O,
        "_dataframe_from_bytes(bytes) -> DataFrame written by "
        "DataFrame.to_bytes."};
    PyObject* bytes_fn = PyCFunction_NewEx(&from_bytes_def, nullptr, nullptr);
    if (!bytes_fn) return -1;
    if (PyModule_AddObject(m, "_dataframe_from_bytes", bytes_fn) < 0) {
        Py_DECREF(bytes_fn);
        return -1;
    }
    return 0;
}

}  // namespace dftracer::utils::python::frame
