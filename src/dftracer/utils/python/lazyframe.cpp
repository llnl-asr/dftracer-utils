// A native LazyFrame exposed to Python: a deferred query over an in-memory
// dataframe::DataFrame. Builder methods record ops and return a new LazyFrame;
// nothing runs until collect(), which materializes a native _DataFrame. A
// filter/with_column expr's col(i) refers to the i-th column of the frame at
// that point, so the Python wrapper resolves names to indices against schema().

#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/python/lazyframe.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/core/common/memory_budget.h>  // NO_SPILL_BUDGET
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/python/columnar_eval.h>
#include <dftracer/utils/python/dataframe.h>
#include <dftracer/utils/python/py_agg_helpers.h>
#include <dftracer/utils/python/py_frame_op_helpers.h>
#include <dftracer/utils/python/py_join_helpers.h>
#include <dftracer/utils/python/py_list_helpers.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_runtime_mixin.h>  // run_blocking
#include <dftracer/utils/python/py_scalar_helpers.h>
#include <dftracer/utils/python/py_seq_helpers.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/series.h>
#include <dftracer/utils/python/streaming_iterator.h>

#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dataframe = dftracer::utils::dataframe;

namespace {

using dataframe::GroupAgg;
using dataframe::LazyFrame;
using dftracer::utils::python::aggs_from_seq;
using dftracer::utils::python::group_agg_from_spec;
using dftracer::utils::python::parse_string_seq;
using dftracer::utils::python::strings_from_str_or_seq;

struct LazyFrameObject {
    PyObject_HEAD LazyFrame lf;
};

PyTypeObject LazyFrameType;

LazyFrameObject* as_lazyframe(PyObject* o) {
    if (!PyObject_TypeCheck(o, &LazyFrameType)) {
        PyErr_SetString(PyExc_TypeError, "expected a LazyFrame");
        return nullptr;
    }
    return reinterpret_cast<LazyFrameObject*>(o);
}

PyObject* make_lazyframe(LazyFrame&& lf) {
    auto* self = reinterpret_cast<LazyFrameObject*>(
        LazyFrameType.tp_alloc(&LazyFrameType, 0));
    if (!self) return nullptr;
    new (&self->lf) LazyFrame(std::move(lf));
    return reinterpret_cast<PyObject*>(self);
}

void LazyFrame_dealloc(LazyFrameObject* self) {
    self->lf.~LazyFrame();
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

// Wrap a builder call that returns a LazyFrame, turning C++ exceptions into
// Python errors.
template <class Fn>
PyObject* run_lazy_op(Fn&& fn) {
    try {
        return make_lazyframe(fn());
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
        return nullptr;
    }
}

// filter(ast) -> LazyFrame. `ast` is the columnar.py post-order Expr AST; the
// engine rebuilds a dataframe::Expr referencing columns by position.
PyObject* LazyFrame_filter(PyObject* self, PyObject* ast) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    dataframe::Expr pred;
    if (!dftracer::utils::python::build_expr_from_ast(ast, &pred))
        return nullptr;
    return run_lazy_op([&] { return b->lf.filter(pred); });
}

PyObject* LazyFrame_with_column(PyObject* self, PyObject* args) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    const char* name = nullptr;
    PyObject* ast = nullptr;
    if (!PyArg_ParseTuple(args, "sO", &name, &ast)) return nullptr;
    dataframe::Expr e;
    if (!dftracer::utils::python::build_expr_from_ast(ast, &e)) return nullptr;
    return run_lazy_op([&] { return b->lf.with_column(name, e); });
}

PyObject* LazyFrame_select(PyObject* self, PyObject* names) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    std::vector<std::string> ns;
    if (!parse_str_list(names, "names", ns)) return nullptr;
    return run_lazy_op([&] { return b->lf.select(std::move(ns)); });
}

PyObject* LazyFrame_rename(PyObject* self, PyObject* names) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    std::vector<std::string> ns;
    if (!parse_str_list(names, "names", ns)) return nullptr;
    return run_lazy_op([&] { return b->lf.rename(std::move(ns)); });
}

PyObject* LazyFrame_slice(PyObject* self, PyObject* args) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    long long offset = 0, len = 0;
    if (!PyArg_ParseTuple(args, "LL", &offset, &len)) return nullptr;
    return run_lazy_op([&] { return b->lf.slice(offset, len); });
}

PyObject* LazyFrame_head(PyObject* self, PyObject* n) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    long long v = PyLong_AsLongLong(n);
    if (v == -1 && PyErr_Occurred()) return nullptr;
    return run_lazy_op([&] { return b->lf.head(v); });
}

PyObject* LazyFrame_take(PyObject* self, PyObject* seq) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    std::vector<std::int64_t> idx;
    if (!dftracer::utils::python::parse_int_seq(
            seq, "take() expects a sequence of ints", idx))
        return nullptr;
    return run_lazy_op([&] { return b->lf.take(std::move(idx)); });
}

PyObject* LazyFrame_filter_mask(PyObject* self, PyObject* mask) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    const dataframe::Series* m =
        dftracer::utils::python::unwrap_vec_column(mask);
    if (!m) {
        PyErr_SetString(PyExc_TypeError,
                        "filter_mask() expects a Series boolean mask");
        return nullptr;
    }
    return run_lazy_op([&] { return b->lf.filter_mask(m->share()); });
}

PyObject* LazyFrame_reverse(PyObject* self, PyObject*) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    return run_lazy_op([&] { return b->lf.reverse(); });
}

PyObject* LazyFrame_sort_by_multi(PyObject* self, PyObject* args,
                                  PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    PyObject* names_obj = nullptr;
    PyObject* descending_obj = nullptr;
    static const char* kw[] = {"names", "descending", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O", const_cast<char**>(kw),
                                     &names_obj, &descending_obj))
        return nullptr;
    std::vector<std::string> names;
    if (!strings_from_str_or_seq(names_obj, names)) return nullptr;
    std::vector<bool> descending;
    if (!descending_obj) {
        descending.push_back(false);
    } else if (PyBool_Check(descending_obj) || PyLong_Check(descending_obj)) {
        descending.push_back(PyObject_IsTrue(descending_obj) != 0);
    } else {
        PyObject* dseq = PySequence_Fast(
            descending_obj, "descending must be a bool or a sequence of bool");
        if (!dseq) return nullptr;
        const Py_ssize_t n = PySequence_Fast_GET_SIZE(dseq);
        for (Py_ssize_t i = 0; i < n; ++i)
            descending.push_back(
                PyObject_IsTrue(PySequence_Fast_GET_ITEM(dseq, i)) != 0);
        Py_DECREF(dseq);
    }
    return run_lazy_op([&] {
        return b->lf.sort_by_multi(std::move(names), std::move(descending));
    });
}

PyObject* LazyFrame_tail(PyObject* self, PyObject* n) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    long long v = PyLong_AsLongLong(n);
    if (v == -1 && PyErr_Occurred()) return nullptr;
    return run_lazy_op([&] { return b->lf.tail(v); });
}

PyObject* LazyFrame_drop_nulls(PyObject* self, PyObject*) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    return run_lazy_op([&] { return b->lf.drop_nulls(); });
}

PyObject* LazyFrame_fill_null(PyObject* self, PyObject* value) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    dftu_scalar s{};
    if (!py_to_scalar(value, &s)) return nullptr;
    return run_lazy_op([&] { return b->lf.fill_null(s); });
}

PyObject* LazyFrame_with_row_index(PyObject* self, PyObject* name) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    const char* s = PyUnicode_AsUTF8(name);
    if (!s) return nullptr;
    return run_lazy_op([&] { return b->lf.with_row_index(s); });
}

PyObject* LazyFrame_null_count(PyObject* self, PyObject*) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    return run_lazy_op([&] { return b->lf.null_count(); });
}

PyObject* LazyFrame_reduce(PyObject* self, PyObject* arg) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    const char* agg = PyUnicode_AsUTF8(arg);
    if (!agg) return nullptr;
    return run_lazy_op(
        [&] { return b->lf.reduce(dataframe::agg_from_string(agg)); });
}

PyObject* LazyFrame_group_transform(PyObject* self, PyObject* args,
                                    PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    dftracer::utils::python::GroupwiseArgs a;
    if (!dftracer::utils::python::parse_groupwise_args(
            args, kwds, strings_from_str_or_seq, a))
        return nullptr;
    return run_lazy_op([&] {
        return b->lf.group_by(a.keys).transform(
            static_cast<dataframe::GroupwiseOp>(a.kind), a.n,
            static_cast<dataframe::RankMethod>(a.method), a.ascending != 0);
    });
}

// reduce_specs(agg, keys) -> list[str]: the "op:column:out" specs that
// broadcast `agg` over every eligible non-key column of the plan's schema.
PyObject* LazyFrame_reduce_specs(PyObject* self, PyObject* args) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    const char* agg = nullptr;
    PyObject* keys_obj = Py_None;
    if (!PyArg_ParseTuple(args, "s|O", &agg, &keys_obj)) return nullptr;
    std::vector<std::string> keys;
    if (keys_obj != Py_None && !strings_from_str_or_seq(keys_obj, keys))
        return nullptr;
    std::vector<std::string> specs;
    try {
        specs = dftracer::utils::python::reduce_spec_strings(
            b->lf.reduce_specs(dataframe::agg_from_string(agg), keys));
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

PyObject* LazyFrame_explode(PyObject* self, PyObject* column) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    const char* s = PyUnicode_AsUTF8(column);
    if (!s) return nullptr;
    return run_lazy_op([&] { return b->lf.explode(s); });
}

PyObject* LazyFrame_unnest(PyObject* self, PyObject* args, PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    const char* column = nullptr;
    int keep_empty = 0;
    static const char* kw[] = {"column", "keep_empty", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|p", const_cast<char**>(kw),
                                     &column, &keep_empty))
        return nullptr;
    return run_lazy_op([&] { return b->lf.unnest(column, keep_empty != 0); });
}

PyObject* LazyFrame_compare_agg(PyObject* self, PyObject* args) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    PyObject* other = nullptr;
    Py_ssize_t n_key = 1;
    if (!PyArg_ParseTuple(args, "On", &other, &n_key)) return nullptr;
    LazyFrameObject* o = as_lazyframe(other);
    if (!o) return nullptr;
    return run_lazy_op([&] {
        return b->lf.compare_agg(o->lf, static_cast<std::int64_t>(n_key));
    });
}

// The output names of a two-frame kernel that keeps every left column and
// appends the right's columns other than `dropped` (the join keys), suffixing
// a right name that collides with a left one by "_right": the asof_join and
// interval_join contract. Empty when either schema is unknown.
std::vector<std::string> two_frame_out_names(
    const LazyFrame& left, const LazyFrame& right,
    const std::vector<std::string>& dropped) {
    std::vector<std::string> names = left.schema();
    const std::vector<std::string> rnames = right.schema();
    if (names.empty() || rnames.empty()) return {};
    for (const std::string& r : rnames) {
        if (std::find(dropped.begin(), dropped.end(), r) != dropped.end())
            continue;
        const bool collides =
            std::find(names.begin(), names.end(), r) != names.end();
        names.push_back(collides ? r + "_right" : r);
    }
    return names;
}

// The four Arrow-layer relational kernels as plan steps: each builds the C
// operand bag its dftu.frame.* row takes and appends it through frame_op,
// which copies every operand into the plan.
PyObject* LazyFrame_window(PyObject* self, PyObject* args, PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    static const char* kw[] = {"partition_by", "order_by", "specs", nullptr};
    PyObject* part = nullptr;
    PyObject* order = nullptr;
    PyObject* specs = nullptr;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOO", const_cast<char**>(kw),
                                     &part, &order, &specs))
        return nullptr;
    std::vector<std::string> pcols;
    std::vector<std::string> ocols;
    if (!parse_string_seq(part, "window: partition_by must be names", pcols) ||
        !parse_string_seq(order, "window: order_by must be names", ocols))
        return nullptr;
    dftracer::utils::python::WindowSpecs parsed;
    if (!dftracer::utils::python::parse_window_specs(specs, parsed))
        return nullptr;
    std::vector<const char*> pc;
    std::vector<const char*> oc;
    for (const std::string& s : pcols) pc.push_back(s.c_str());
    for (const std::string& s : ocols) oc.push_back(s.c_str());
    dataframe::OpArgs a;
    a.strlist(1, pc.data(), static_cast<std::int32_t>(pc.size()))
        .strlist(2, oc.data(), static_cast<std::int32_t>(oc.size()))
        .winlist(3, parsed.specs);
    return run_lazy_op([&] {
        // A window keeps every input column and appends one per spec.
        std::vector<std::string> names = b->lf.schema();
        if (!names.empty())
            for (const dftu_window_spec& s : parsed.specs)
                names.emplace_back(s.out);
        return b->lf.frame_op("dftu.frame.window", a, {}, std::move(names));
    });
}

// column_op(column, op, column2=None, a=0, b=0, text=None): the registered
// column op `op` over `column` as a plan step (dftu.frame.column_op, a
// breaker), the column replaced in place.
PyObject* LazyFrame_column_op(PyObject* self, PyObject* args, PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    static const char* kw[] = {"column", "op",   "column2", "a",
                               "b",      "text", nullptr};
    const char* column = nullptr;
    const char* op = nullptr;
    const char* column2 = nullptr;
    PyObject* a_obj = nullptr;
    PyObject* b_obj = nullptr;
    const char* text = nullptr;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "ss|zOOz",
                                     const_cast<char**>(kw), &column, &op,
                                     &column2, &a_obj, &b_obj, &text))
        return nullptr;
    dftu_scalar sa{};
    dftu_scalar sb{};
    sa.kind = DFTU_SCALAR_TAG_I64;
    sb.kind = DFTU_SCALAR_TAG_I64;
    if (a_obj && a_obj != Py_None && !py_to_scalar(a_obj, &sa)) return nullptr;
    if (b_obj && b_obj != Py_None && !py_to_scalar(b_obj, &sb)) return nullptr;
    const std::string c2 = column2 ? column2 : "";
    const std::string tx = text ? text : "";
    dataframe::OpArgs a;
    a.str(1, column).str(2, op).str(3, c2).scalar(4, sa).scalar(5, sb).str(6,
                                                                           tx);
    return run_lazy_op([&] {
        return b->lf.frame_op("dftu.frame.column_op", a, {}, b->lf.schema());
    });
}

PyObject* LazyFrame_gap_fill(PyObject* self, PyObject* args, PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    static const char* kw[] = {"partition_by", "time",  "bucket", "values",
                               "mode",         "start", "end",    nullptr};
    PyObject* part = nullptr;
    const char* time = nullptr;
    long long bucket = 0;
    PyObject* values = nullptr;
    const char* mode = nullptr;
    PyObject* start_obj = Py_None;
    PyObject* end_obj = Py_None;
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "OsLOs|OO", const_cast<char**>(kw), &part, &time,
            &bucket, &values, &mode, &start_obj, &end_obj))
        return nullptr;
    dftu_gap_fill_mode m;
    if (!dftracer::utils::python::gap_fill_mode_from_str(mode, &m))
        return nullptr;
    std::vector<std::string> pcols;
    std::vector<std::string> vcols;
    if (!parse_string_seq(part, "gap_fill: partition_by must be names",
                          pcols) ||
        !parse_string_seq(values, "gap_fill: values must be names", vcols))
        return nullptr;
    std::vector<std::int64_t> range;
    if (start_obj != Py_None) {
        const long long s = PyLong_AsLongLong(start_obj);
        const long long e = PyLong_AsLongLong(end_obj);
        if (PyErr_Occurred()) return nullptr;
        range = {static_cast<std::int64_t>(s), static_cast<std::int64_t>(e)};
    }
    std::vector<const char*> pc;
    std::vector<const char*> vc;
    for (const std::string& s : pcols) pc.push_back(s.c_str());
    for (const std::string& s : vcols) vc.push_back(s.c_str());
    dataframe::OpArgs a;
    a.strlist(1, pc.data(), static_cast<std::int32_t>(pc.size()))
        .str(2, time)
        .i64(3, static_cast<std::int64_t>(bucket))
        .strlist(4, vc.data(), static_cast<std::int32_t>(vc.size()))
        .i32(5, static_cast<std::int32_t>(m))
        .i64list(6, range);
    return run_lazy_op([&] {
        return b->lf.frame_op("dftu.frame.gap_fill", a, {}, b->lf.schema());
    });
}

PyObject* LazyFrame_asof(PyObject* self, PyObject* args, PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    static const char* kw[] = {"other",     "on",        "by",
                               "direction", "tolerance", nullptr};
    PyObject* other = nullptr;
    const char* on = nullptr;
    PyObject* by = nullptr;
    const char* direction = nullptr;
    PyObject* tol_obj = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OsOsO",
                                     const_cast<char**>(kw), &other, &on, &by,
                                     &direction, &tol_obj))
        return nullptr;
    LazyFrameObject* o = as_lazyframe(other);
    if (!o) return nullptr;
    dftu_asof_direction dir;
    if (!dftracer::utils::python::asof_direction_from_str(direction, &dir))
        return nullptr;
    std::vector<std::string> equi;
    if (!parse_string_seq(by, "asof: by must be names", equi)) return nullptr;
    std::int64_t tol = -1;
    if (tol_obj != Py_None) {
        const long long t = PyLong_AsLongLong(tol_obj);
        if (PyErr_Occurred()) return nullptr;
        if (t < 0) {
            PyErr_SetString(PyExc_ValueError,
                            "asof: tolerance must not be negative");
            return nullptr;
        }
        tol = static_cast<std::int64_t>(t);
    }
    std::vector<const char*> ec;
    for (const std::string& s : equi) ec.push_back(s.c_str());
    dataframe::OpArgs a;
    a.str(2, on)
        .strlist(3, ec.data(), static_cast<std::int32_t>(ec.size()))
        .i32(4, static_cast<std::int32_t>(dir))
        .i64(5, tol);
    return run_lazy_op([&] {
        std::vector<std::string> dropped = equi;
        dropped.emplace_back(on);
        return b->lf.frame_op("dftu.frame.asof", a, {o->lf},
                              two_frame_out_names(b->lf, o->lf, dropped));
    });
}

PyObject* LazyFrame_interval(PyObject* self, PyObject* args, PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    static const char* kw[] = {"other", "point", "lo",   "hi",
                               "by",    "outer", nullptr};
    PyObject* other = nullptr;
    const char* point = nullptr;
    const char* lo = nullptr;
    const char* hi = nullptr;
    PyObject* by = nullptr;
    int outer = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OsssOp",
                                     const_cast<char**>(kw), &other, &point,
                                     &lo, &hi, &by, &outer))
        return nullptr;
    LazyFrameObject* o = as_lazyframe(other);
    if (!o) return nullptr;
    std::vector<std::string> equi;
    if (!parse_string_seq(by, "interval: by must be names", equi))
        return nullptr;
    std::vector<const char*> ec;
    for (const std::string& s : equi) ec.push_back(s.c_str());
    dataframe::OpArgs a;
    a.str(2, point)
        .str(3, lo)
        .str(4, hi)
        .strlist(5, ec.data(), static_cast<std::int32_t>(ec.size()))
        .i32(6, outer != 0 ? 1 : 0);
    return run_lazy_op([&] {
        std::vector<std::string> dropped = equi;
        dropped.emplace_back(lo);
        dropped.emplace_back(hi);
        return b->lf.frame_op("dftu.frame.interval", a, {o->lf},
                              two_frame_out_names(b->lf, o->lf, dropped));
    });
}

// unpivot(id_vars, value_vars); melt is the same call.
PyObject* LazyFrame_unpivot(PyObject* self, PyObject* args) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    PyObject* id_obj = nullptr;
    PyObject* val_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &id_obj, &val_obj)) return nullptr;
    std::vector<std::string> id_vars, value_vars;
    if (!parse_str_list(id_obj, "id_vars", id_vars) ||
        !parse_str_list(val_obj, "value_vars", value_vars))
        return nullptr;
    return run_lazy_op([&] {
        return b->lf.unpivot(std::move(id_vars), std::move(value_vars));
    });
}

PyObject* LazyFrame_topk(PyObject* self, PyObject* args, PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    const char* name = nullptr;
    long long k = 0;
    int largest = 1;
    static const char* kw[] = {"name", "k", "largest", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sL|p", const_cast<char**>(kw),
                                     &name, &k, &largest))
        return nullptr;
    return run_lazy_op([&] { return b->lf.topk(name, k, largest != 0); });
}

// group_by(key, aggs): key is a column name or a sequence of names (composite
// key); aggs is a sequence of "op[:column]" string specs.
PyObject* LazyFrame_group_by(PyObject* self, PyObject* args) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    PyObject* key_obj = nullptr;
    PyObject* aggs_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &key_obj, &aggs_obj)) return nullptr;
    std::vector<std::string> keys;
    if (!strings_from_str_or_seq(key_obj, keys)) return nullptr;
    std::vector<GroupAgg> aggs;
    if (!aggs_from_seq(aggs_obj, aggs)) return nullptr;
    return run_lazy_op(
        [&] { return b->lf.group_by(std::move(keys), std::move(aggs)); });
}

PyObject* LazyFrame_sort_by(PyObject* self, PyObject* args, PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    const char* name = nullptr;
    int descending = 0;
    static const char* kw[] = {"name", "descending", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|p", const_cast<char**>(kw),
                                     &name, &descending))
        return nullptr;
    return run_lazy_op([&] { return b->lf.sort_by(name, descending != 0); });
}

PyObject* LazyFrame_join(PyObject* self, PyObject* args, PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    PyObject* other = nullptr;
    PyObject* on = nullptr;
    const char* how = "inner";
    PyObject* left_on = nullptr;
    PyObject* right_on = nullptr;
    const char* suffix = "_right";
    static const char* kw[] = {"other",    "on",     "how",  "left_on",
                               "right_on", "suffix", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|OsOOs",
                                     const_cast<char**>(kw), &other, &on, &how,
                                     &left_on, &right_on, &suffix))
        return nullptr;
    LazyFrameObject* o = as_lazyframe(other);
    if (!o) return nullptr;
    dataframe::JoinHow jh;
    if (!dftracer::utils::python::join_how_from_str(how, jh)) return nullptr;
    std::vector<std::string> l;
    std::vector<std::string> r;
    if (!dftracer::utils::python::join_keys_from_objs(on, left_on, right_on, jh,
                                                      l, r))
        return nullptr;
    return run_lazy_op([&] {
        return b->lf.join(o->lf, std::move(l), std::move(r), jh, suffix);
    });
}

PyObject* LazyFrame_concat(PyObject* self, PyObject* args) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    const Py_ssize_t n = PyTuple_GET_SIZE(args);
    std::vector<LazyFrameObject*> others;
    others.reserve(static_cast<std::size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        LazyFrameObject* o = as_lazyframe(PyTuple_GET_ITEM(args, i));
        if (!o) return nullptr;
        others.push_back(o);
    }
    return run_lazy_op([&] {
        dataframe::LazyFrame out = b->lf;
        for (LazyFrameObject* o : others) out = out.concat(o->lf);
        return out;
    });
}

PyObject* LazyFrame_unique(PyObject* self, PyObject* args, PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    PyObject* subset_obj = Py_None;
    static const char* kw[] = {"subset", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", const_cast<char**>(kw),
                                     &subset_obj))
        return nullptr;
    std::vector<std::string> subset;
    if (subset_obj != Py_None && !strings_from_str_or_seq(subset_obj, subset))
        return nullptr;
    return run_lazy_op([&] { return b->lf.unique(std::move(subset)); });
}

PyObject* LazyFrame_sample(PyObject* self, PyObject* args, PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    long long n = 0;
    unsigned long long seed = 0;
    static const char* kw[] = {"n", "seed", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "L|K", const_cast<char**>(kw),
                                     &n, &seed))
        return nullptr;
    return run_lazy_op([&] { return b->lf.sample(n, seed); });
}

PyObject* LazyFrame_is_duplicated(PyObject* self, PyObject*) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    return run_lazy_op([&] { return b->lf.is_duplicated(); });
}

PyObject* LazyFrame_is_unique(PyObject* self, PyObject*) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    return run_lazy_op([&] { return b->lf.is_unique(); });
}

// group_by_dynamic(time_col, every, period=None, aggs=[], origin=0,
// origin_min=False). period defaults to every (tumbling window).
PyObject* LazyFrame_group_by_dynamic(PyObject* self, PyObject* args,
                                     PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    const char* time_col = nullptr;
    long long every = 0;
    PyObject* period_obj = Py_None;
    PyObject* aggs_obj = nullptr;
    long long origin = 0;
    int origin_min = 0;
    static const char* kw[] = {"time_col", "every",      "period", "aggs",
                               "origin",   "origin_min", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "sL|OOLp", const_cast<char**>(kw), &time_col, &every,
            &period_obj, &aggs_obj, &origin, &origin_min))
        return nullptr;
    long long period = every;
    if (period_obj && period_obj != Py_None) {
        period = PyLong_AsLongLong(period_obj);
        if (period == -1 && PyErr_Occurred()) return nullptr;
    }
    std::vector<GroupAgg> aggs;
    if (aggs_obj && aggs_obj != Py_None && !aggs_from_seq(aggs_obj, aggs))
        return nullptr;
    return run_lazy_op([&] {
        return b->lf.group_by_dynamic(time_col, every, period, std::move(aggs),
                                      origin, origin_min != 0);
    });
}

PyObject* LazyFrame_pivot(PyObject* self, PyObject* args, PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    const char* index = nullptr;
    const char* on = nullptr;
    const char* values = nullptr;
    const char* agg = "first";
    static const char* kw[] = {"index", "on", "values", "agg", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sss|s",
                                     const_cast<char**>(kw), &index, &on,
                                     &values, &agg))
        return nullptr;
    return run_lazy_op([&] { return b->lf.pivot(index, on, values, agg); });
}

PyObject* LazyFrame_to_dummies(PyObject* self, PyObject* column) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    const char* s = PyUnicode_AsUTF8(column);
    if (!s) return nullptr;
    return run_lazy_op([&] { return b->lf.to_dummies(s); });
}

PyObject* LazyFrame_describe(PyObject* self, PyObject*) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    return run_lazy_op([&] { return b->lf.describe(); });
}

PyObject* LazyFrame_memory_budget(PyObject* self, PyObject* bytes) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    unsigned long long v = PyLong_AsUnsignedLongLong(bytes);
    if (v == static_cast<unsigned long long>(-1) && PyErr_Occurred())
        return nullptr;
    return run_lazy_op([&] { return b->lf.memory_budget(v); });
}

PyObject* LazyFrame_auto_spill(PyObject* self, PyObject*) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    return run_lazy_op([&] { return b->lf.auto_spill(); });
}

PyObject* LazyFrame_schema(PyObject* self, PyObject*) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    std::vector<std::string> names;
    try {
        names = b->lf.schema();
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
        return nullptr;
    }
    return str_list_from(names);
}

PyObject* LazyFrame_explain(PyObject* self, PyObject*) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    std::string text;
    try {
        text = b->lf.explain();
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
        return nullptr;
    }
    return PyUnicode_FromStringAndSize(text.data(),
                                       static_cast<Py_ssize_t>(text.size()));
}

PyObject* LazyFrame_collect(PyObject* self, PyObject* args, PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    long long morsel_rows = 0;
    PyObject* runtime_arg = nullptr;
    static const char* kw[] = {"morsel_rows", "runtime", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|LO", const_cast<char**>(kw),
                                     &morsel_rows, &runtime_arg))
        return nullptr;
    std::shared_ptr<dftracer::utils::Runtime> rt =
        runtime_from_arg(runtime_arg);
    if (!rt) return nullptr;
    dataframe::DataFrame out;
    if (!run_blocking(
            [&] { out = rt->submit(b->lf.collect(morsel_rows)).get(); }))
        return nullptr;
    return dftracer::utils::python::wrap_dataframe(std::move(out));
}

// Drains the plan's chunk generator into `state`, which the Python iterator
// pulls from with the GIL released.
dftracer::utils::coro::CoroTask<void> drain_stream(
    dftracer::utils::CoroScope&,
    std::shared_ptr<
        dftracer::utils::python::StreamingState<dataframe::DataFrame>>
        state,
    LazyFrame lf, std::int64_t morsel_rows) {
    try {
        auto gen = lf.stream(morsel_rows);
        while (auto df = co_await gen.next()) {
            if (state->cancelled()) break;
            const std::size_t bytes =
                static_cast<std::size_t>(df->num_rows()) *
                static_cast<std::size_t>(df->num_columns() + 1) * 16;
            if (!state->push(std::move(*df), bytes)) break;
        }
        state->complete();
    } catch (...) {
        state->fail(std::current_exception());
    }
}

PyObject* LazyFrame_stream(PyObject* self, PyObject* args, PyObject* kwds) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    long long morsel_rows = 0;
    PyObject* runtime_arg = nullptr;
    static const char* kw[] = {"morsel_rows", "runtime", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|LO", const_cast<char**>(kw),
                                     &morsel_rows, &runtime_arg))
        return nullptr;
    std::shared_ptr<dftracer::utils::Runtime> rt =
        runtime_from_arg(runtime_arg);
    if (!rt) return nullptr;
    namespace py = dftracer::utils::python;
    auto state = std::make_shared<py::StreamingState<dataframe::DataFrame>>(
        dftracer::utils::compute_memory_budget(0));
    auto* it = reinterpret_cast<py::ArrowStreamingIteratorObject*>(
        py::ArrowStreamingIteratorType.tp_new(&py::ArrowStreamingIteratorType,
                                              nullptr, nullptr));
    if (!it) return nullptr;
    it->cpp_state->state = state;
    it->cpp_state->pull_df = [state]() { return state->pull(); };
    it->cpp_state->get_error = [state]() { return state->error(); };
    it->cpp_state->cancel = [state]() { state->cancel(); };
    LazyFrame lf = b->lf;
    Py_BEGIN_ALLOW_THREADS rt->submit(
        dftracer::utils::run_coro_scope(rt->executor(), drain_stream, state,
                                        std::move(lf),
                                        static_cast<std::int64_t>(morsel_rows)),
        "lazyframe_stream");
    Py_END_ALLOW_THREADS return reinterpret_cast<PyObject*>(it);
}

PyObject* LazyFrame_output_schema(PyObject* self, PyObject*) {
    LazyFrameObject* b = as_lazyframe(self);
    if (!b) return nullptr;
    dataframe::Schema schema;
    try {
        schema = b->lf.output_schema();
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
        return nullptr;
    }
    PyObject* out = PyList_New(static_cast<Py_ssize_t>(schema.fields.size()));
    if (!out) return nullptr;
    for (std::size_t i = 0; i < schema.fields.size(); ++i) {
        const dataframe::Field& f = schema.fields[i];
        PyObject* item =
            Py_BuildValue("(si)", f.name.c_str(), static_cast<int>(f.type.id));
        if (!item) {
            Py_DECREF(out);
            return nullptr;
        }
        PyList_SET_ITEM(out, static_cast<Py_ssize_t>(i), item);
    }
    return out;
}

PyMethodDef LazyFrame_methods[] = {
    {"filter", LazyFrame_filter, METH_O,
     "filter(ast) -> LazyFrame keeping rows where the predicate holds."},
    {"with_column", LazyFrame_with_column, METH_VARARGS,
     "with_column(name, ast) -> LazyFrame with a column added or replaced."},
    {"select", LazyFrame_select, METH_O,
     "select(names) -> LazyFrame projected to the given columns."},
    {"rename", LazyFrame_rename, METH_O,
     "rename(names) -> LazyFrame with columns renamed positionally."},
    {"slice", LazyFrame_slice, METH_VARARGS,
     "slice(offset, len) -> LazyFrame of the row window."},
    {"head", LazyFrame_head, METH_O,
     "head(n) -> LazyFrame of the first n rows."},
    {"tail", LazyFrame_tail, METH_O,
     "tail(n) -> LazyFrame of the last n rows."},
    {"drop_nulls", LazyFrame_drop_nulls, METH_NOARGS,
     "drop_nulls() -> LazyFrame with rows holding any null removed."},
    {"fill_null", LazyFrame_fill_null, METH_O,
     "fill_null(value) -> LazyFrame with nulls filled in every column."},
    {"with_row_index", LazyFrame_with_row_index, METH_O,
     "with_row_index(name) -> LazyFrame with a prepended Int64 index column."},
    {"null_count", LazyFrame_null_count, METH_NOARGS,
     "null_count() -> LazyFrame of each column's null count (one row)."},
    {"reduce", LazyFrame_reduce, METH_O,
     "reduce(agg) -> LazyFrame: the aggregate named `agg` over every eligible "
     "column, a streaming one-group group-by."},
    {"group_transform", DFTU_PYCFUNCTION(LazyFrame_group_transform),
     METH_VARARGS | METH_KEYWORDS,
     "group_transform(keys, kind, n=0, method='average', ascending=True) -> "
     "LazyFrame: the group-wise transform `kind` (cumsum, cummax, cummin, "
     "cumcount, shift, diff, pct_change, rank, ngroup, head, tail, nth) over "
     "the groups of `keys`, one value per input row in input order."},
    {"reduce_specs", LazyFrame_reduce_specs, METH_VARARGS,
     "reduce_specs(agg, keys=None) -> list[str]: the 'op:column:out' specs "
     "that broadcast `agg` over every eligible non-key column, for group_by."},
    {"explode", LazyFrame_explode, METH_O,
     "explode(column) -> LazyFrame with the List column expanded per element."},
    {"unnest", DFTU_PYCFUNCTION(LazyFrame_unnest), METH_VARARGS | METH_KEYWORDS,
     "unnest(column, keep_empty=False) -> LazyFrame expanding a List column "
     "one row per element; an empty/null list drops the row unless "
     "keep_empty, and a List<Struct> flattens into one column per field."},
    {"compare_agg", LazyFrame_compare_agg, METH_VARARGS,
     "compare_agg(variant, n_key) -> LazyFrame: DataFrame.compare_agg over "
     "the two collected plans."},
    {"window", DFTU_PYCFUNCTION(LazyFrame_window), METH_VARARGS | METH_KEYWORDS,
     "window(partition_by, order_by, specs) -> LazyFrame: SQL window "
     "functions over the collected plan; specs are normalized 9-tuples."},
    {"column_op", DFTU_PYCFUNCTION(LazyFrame_column_op),
     METH_VARARGS | METH_KEYWORDS,
     "column_op(column, op, column2=None, a=0, b=0, text=None) -> the "
     "registered column op over `column` as a plan step (a breaker)."},
    {"gap_fill", DFTU_PYCFUNCTION(LazyFrame_gap_fill),
     METH_VARARGS | METH_KEYWORDS,
     "gap_fill(partition_by, time, bucket, values, mode, start=None, "
     "end=None) -> LazyFrame: a regular time grid over the collected plan."},
    {"asof", DFTU_PYCFUNCTION(LazyFrame_asof), METH_VARARGS | METH_KEYWORDS,
     "asof(other, on, by, direction, tolerance) -> LazyFrame: temporal "
     "nearest-match join of the two collected plans."},
    {"interval", DFTU_PYCFUNCTION(LazyFrame_interval),
     METH_VARARGS | METH_KEYWORDS,
     "interval(other, point, lo, hi, by, outer) -> LazyFrame: point-in-range "
     "join of the two collected plans."},
    {"unpivot", LazyFrame_unpivot, METH_VARARGS,
     "unpivot(id_vars, value_vars) -> LazyFrame reshaped wide to long."},
    {"melt", LazyFrame_unpivot, METH_VARARGS, "melt(...) -> alias of unpivot."},
    {"topk", DFTU_PYCFUNCTION(LazyFrame_topk), METH_VARARGS | METH_KEYWORDS,
     "topk(name, k, largest=True) -> LazyFrame of the k extreme rows."},
    {"group_by", LazyFrame_group_by, METH_VARARGS,
     "group_by(key, aggs) -> LazyFrame grouped by key with string-spec aggs."},
    {"sort_by", DFTU_PYCFUNCTION(LazyFrame_sort_by),
     METH_VARARGS | METH_KEYWORDS,
     "sort_by(name, descending=False) -> LazyFrame sorted by one column."},
    {"take", LazyFrame_take, METH_O,
     "take(indices) -> LazyFrame keeping the rows at indices (any order, "
     "repeats allowed)."},
    {"filter_mask", LazyFrame_filter_mask, METH_O,
     "filter_mask(mask) -> LazyFrame keeping rows where the precomputed Bool "
     "mask is true."},
    {"reverse", LazyFrame_reverse, METH_NOARGS,
     "reverse() -> LazyFrame with rows in reverse order."},
    {"sort_by_multi", DFTU_PYCFUNCTION(LazyFrame_sort_by_multi),
     METH_VARARGS | METH_KEYWORDS,
     "sort_by_multi(names, descending=False) -> LazyFrame stably sorted "
     "lexicographically by several key columns."},
    {"join", DFTU_PYCFUNCTION(LazyFrame_join), METH_VARARGS | METH_KEYWORDS,
     "join(other, on=None, how='inner', left_on=None, right_on=None, "
     "suffix='_right') -> LazyFrame hash-joined with another LazyFrame; "
     "how=inner|left|right|outer|full|semi|anti|cross."},
    {"concat", LazyFrame_concat, METH_VARARGS,
     "concat(*others) -> LazyFrame with every row of this plan followed by "
     "every row of each other plan in turn; schemas must match."},
    {"unique", DFTU_PYCFUNCTION(LazyFrame_unique), METH_VARARGS | METH_KEYWORDS,
     "unique(subset=None) -> LazyFrame with duplicate rows removed (keep "
     "first), keyed on every column or on the subset names."},
    {"drop_duplicates", DFTU_PYCFUNCTION(LazyFrame_unique),
     METH_VARARGS | METH_KEYWORDS,
     "drop_duplicates(subset=None) -> alias of unique."},
    {"sample", DFTU_PYCFUNCTION(LazyFrame_sample), METH_VARARGS | METH_KEYWORDS,
     "sample(n, seed=0) -> LazyFrame deterministic n-row sample."},
    {"is_duplicated", LazyFrame_is_duplicated, METH_NOARGS,
     "is_duplicated() -> LazyFrame Bool column, true where the row repeats."},
    {"is_unique", LazyFrame_is_unique, METH_NOARGS,
     "is_unique() -> LazyFrame Bool column, true where the row is unique."},
    {"group_by_dynamic", DFTU_PYCFUNCTION(LazyFrame_group_by_dynamic),
     METH_VARARGS | METH_KEYWORDS,
     "group_by_dynamic(time_col, every, period=None, aggs=[], origin=0, "
     "origin_min=False) -> LazyFrame of time-window aggregates."},
    {"pivot", DFTU_PYCFUNCTION(LazyFrame_pivot), METH_VARARGS | METH_KEYWORDS,
     "pivot(index, on, values, agg='first') -> LazyFrame reshaped long to "
     "wide."},
    {"to_dummies", LazyFrame_to_dummies, METH_O,
     "to_dummies(column) -> LazyFrame one-hot encoding of the column."},
    {"describe", LazyFrame_describe, METH_NOARGS,
     "describe() -> LazyFrame of per-column summary statistics."},
    {"memory_budget", LazyFrame_memory_budget, METH_O,
     "memory_budget(bytes) -> LazyFrame with the out-of-core spill budget "
     "set."},
    {"auto_spill", LazyFrame_auto_spill, METH_NOARGS,
     "auto_spill() -> LazyFrame with the spill budget at ~1/3 of memory."},
    {"output_schema", LazyFrame_output_schema, METH_NOARGS,
     "output_schema() -> [(name, dtype id)] without running; a type the "
     "plan cannot know statically is 0 (unknown)."},
    {"stream", DFTU_PYCFUNCTION(LazyFrame_stream), METH_VARARGS | METH_KEYWORDS,
     "stream(morsel_rows=0, runtime=None) -> iterator of DataFrame chunks."},
    {"schema", LazyFrame_schema, METH_NOARGS,
     "schema() -> list[str] of output column names, or [] if data-dependent."},
    {"explain", LazyFrame_explain, METH_NOARGS,
     "explain() -> str of the optimized plan."},
    {"collect", DFTU_PYCFUNCTION(LazyFrame_collect),
     METH_VARARGS | METH_KEYWORDS,
     "collect(morsel_rows=65536) -> _DataFrame, running the pipeline."},
    {nullptr, nullptr, 0, nullptr}};

PyObject* collect_all_py(PyObject*, PyObject* args, PyObject* kwds) {
    PyObject* arg = nullptr;
    PyObject* runtime_arg = nullptr;
    static const char* kw[] = {"plans", "runtime", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O", const_cast<char**>(kw),
                                     &arg, &runtime_arg))
        return nullptr;
    std::shared_ptr<dftracer::utils::Runtime> rt =
        runtime_from_arg(runtime_arg);
    if (!rt) return nullptr;
    PyObject* seq = PySequence_Fast(arg, "collect_all expects a sequence");
    if (!seq) return nullptr;
    const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    std::vector<LazyFrame> plans;
    plans.reserve(static_cast<std::size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        LazyFrameObject* b = as_lazyframe(PySequence_Fast_GET_ITEM(seq, i));
        if (!b) {
            Py_DECREF(seq);
            return nullptr;
        }
        plans.push_back(b->lf);
    }
    Py_DECREF(seq);
    std::vector<dataframe::DataFrame> frames;
    if (!run_blocking([&] {
            frames = rt->submit(dataframe::collect_all(std::move(plans))).get();
        }))
        return nullptr;
    PyObject* out = PyList_New(static_cast<Py_ssize_t>(frames.size()));
    if (!out) return nullptr;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        PyObject* df =
            dftracer::utils::python::wrap_dataframe(std::move(frames[i]));
        if (!df) {
            Py_DECREF(out);
            return nullptr;
        }
        PyList_SET_ITEM(out, static_cast<Py_ssize_t>(i), df);
    }
    return out;
}

PyMethodDef lazyframe_module_methods[] = {
    {"collect_all", DFTU_PYCFUNCTION(collect_all_py),
     METH_VARARGS | METH_KEYWORDS,
     "collect_all(plans) -> list of DataFrames, one per plan in order; plans "
     "over the same trace base share one scan."},
    {nullptr, nullptr, 0, nullptr}};

}  // namespace

namespace dftracer::utils::python {

int init_lazyframe(PyObject* m) {
    LazyFrameType = {};
    LazyFrameType.ob_base = {PyObject_HEAD_INIT(nullptr) 0};
    LazyFrameType.tp_name = "dftracer_utils_ext._LazyFrame";
    LazyFrameType.tp_basicsize = sizeof(LazyFrameObject);
    LazyFrameType.tp_flags = Py_TPFLAGS_DEFAULT;
    LazyFrameType.tp_doc = "A deferred query over a native vec batch.";
    LazyFrameType.tp_dealloc = reinterpret_cast<destructor>(LazyFrame_dealloc);
    LazyFrameType.tp_methods = LazyFrame_methods;
    LazyFrameType.tp_new = nullptr;  // created only by DataFrame.lazy()
    if (register_type(m, &LazyFrameType, "_LazyFrame") < 0) return -1;
    return PyModule_AddFunctions(m, lazyframe_module_methods);
}

PyObject* wrap_lazyframe(dataframe::LazyFrame&& lf) {
    return make_lazyframe(std::move(lf));
}

const dataframe::LazyFrame* lazyframe_of(PyObject* o) {
    LazyFrameObject* b = as_lazyframe(o);
    return b ? &b->lf : nullptr;
}

}  // namespace dftracer::utils::python

#else   // !DFTRACER_UTILS_ENABLE_ARROW

namespace dftracer::utils::python {
int init_lazyframe(PyObject*) { return 0; }
PyObject* wrap_lazyframe(dftracer::utils::dataframe::LazyFrame&&) {
    PyErr_SetString(PyExc_RuntimeError, "LazyFrame requires the Arrow build");
    return nullptr;
}

const dftracer::utils::dataframe::LazyFrame* lazyframe_of(PyObject*) {
    PyErr_SetString(PyExc_RuntimeError, "LazyFrame requires the Arrow build");
    return nullptr;
}
}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_ENABLE_ARROW
